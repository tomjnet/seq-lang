#include "execution/execution.hpp"

#include <algorithm>
#include <cstdlib>
#include <map>
#include <set>
#include <system_error>

#include "support/json.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMiB = 1024 * 1024;

std::vector<std::string> SplitWords(const std::string& line,
                                    std::size_t max_words) {
  std::vector<std::string> words;
  std::size_t pos = 0;
  while (pos < line.size() && words.size() + 1 < max_words) {
    const std::size_t space = line.find(' ', pos);
    if (space == std::string::npos) break;
    words.push_back(line.substr(pos, space - pos));
    pos = space + 1;
  }
  words.push_back(line.substr(pos));
  return words;
}

// Reads the record of files that earlier runs published: path -> SHA-256.
std::map<std::string, std::string> ReadPublishedRecord(const fs::path& path) {
  std::map<std::string, std::string> record;
  std::string text;
  std::string error;
  Json json;
  if (!ReadFile(path, &text, &error, 16 * kMiB) ||
      !Json::Parse(text, &json, &error)) {
    return record;
  }
  const Json* files = json.Find("files");
  if (files == nullptr || !files->is_object()) return record;
  for (const Json::Member& member : files->AsObject()) {
    if (member.second.is_string()) {
      record[member.first] = member.second.AsString();
    }
  }
  return record;
}

bool WritePublishedRecord(const fs::path& path,
                          const std::map<std::string, std::string>& record,
                          std::string* error) {
  Json files = Json::MakeObject();
  for (const auto& [name, hash] : record) files.Set(name, hash);
  Json json = Json::MakeObject();
  json.Set("schema_version", 1);
  json.Set("note",
           "Files seqc published into output/ and their hashes. A rerun may "
           "replace a listed file only while its hash still matches.");
  json.Set("files", std::move(files));
  return WriteFile(path, json.Dump() + "\n", error);
}

bool HasPngSignature(const fs::path& path) {
  std::string data;
  std::string error;
  if (!ReadFile(path, &data, &error, 4096 * kMiB)) return false;
  static constexpr char kSignature[] = "\x89PNG\r\n\x1a\n";
  return data.size() >= 8 && data.compare(0, 8, kSignature, 8) == 0;
}

bool IsUtf8File(const fs::path& path) {
  std::string data;
  std::string error;
  if (!ReadFile(path, &data, &error, 4096 * kMiB)) return false;
  return IsValidUtf8(data);
}

}  // namespace

ProcessResult RunIsolated(const fs::path& binary, const fs::path& staging,
                          const fs::path& input_dir, const Config& config,
                          std::ostream* live_stdout,
                          std::ostream* live_stderr) {
  ProcessSpec spec;
  spec.argv = {binary.string()};
  spec.env = {"LC_ALL=C", "TZ=UTC"};
  spec.cwd = staging;
  spec.report_pipe = true;
  spec.live_stdout = live_stdout;
  spec.live_stderr = live_stderr;
  spec.limits.cpu_seconds =
      static_cast<std::uint64_t>(config.GetInt("limits.cpu_seconds"));
  spec.limits.wall_seconds =
      static_cast<std::uint64_t>(config.GetInt("limits.wall_seconds"));
  spec.limits.memory_bytes =
      static_cast<std::uint64_t>(config.GetInt("limits.memory_mb")) * kMiB;
  spec.limits.file_bytes =
      static_cast<std::uint64_t>(config.GetInt("limits.file_mb")) * kMiB;
  spec.limits.output_bytes =
      static_cast<std::uint64_t>(config.GetInt("limits.output_mb")) * kMiB;

  std::error_code ec;
  if (fs::is_directory(input_dir, ec)) spec.input_dir = input_dir;
  spec.sandbox = RunSandboxPolicy(binary, staging, input_dir);
  return RunProcess(spec);
}

SandboxPolicy RunSandboxPolicy(const fs::path& binary, const fs::path& staging,
                               const fs::path& input_dir) {
  SandboxPolicy policy;
  policy.allow_process_creation = false;
  policy.read_exec = {binary};
  policy.read_write = {staging};
  std::error_code ec;
  if (fs::is_directory(input_dir, ec)) policy.read_only = {input_dir};
  return policy;
}

std::vector<StepRecord> ParseStepRecords(const std::string& report) {
  std::vector<StepRecord> records;
  const auto find = [&](int index) -> StepRecord* {
    for (StepRecord& record : records) {
      if (record.index == index) return &record;
    }
    return nullptr;
  };
  for (const std::string& line : SplitLines(report)) {
    // begin <i> <name> | ok <i> <name> | message <i> <text> |
    // fail <i> <name> <code>
    const std::vector<std::string> head = SplitWords(line, 3);
    if (head.size() < 3) continue;
    const int index = std::atoi(head[1].c_str());
    if (index <= 0) continue;
    if (head[0] == "begin") {
      StepRecord record;
      record.index = index;
      record.name = head[2];
      record.status = "running";
      records.push_back(std::move(record));
      continue;
    }
    StepRecord* record = find(index);
    if (record == nullptr) continue;
    if (head[0] == "ok") {
      record->status = "ok";
    } else if (head[0] == "message") {
      record->message = head[2];
    } else if (head[0] == "fail") {
      const std::vector<std::string> tail = SplitWords(head[2], 2);
      record->status = "failed";
      record->code = tail.size() == 2 ? std::atoi(tail[1].c_str()) : 0;
    }
  }
  return records;
}

std::vector<TestCaseResult> ParseGoogleTestOutput(const std::string& output) {
  // Google Test prints "[       OK ] Suite.Name (0 ms)" or
  // "[  FAILED  ] Suite.Name (0 ms)" as each test finishes, and repeats the
  // failed names in a summary. Only the first result per test is kept.
  constexpr std::string_view kOk = "[       OK ] ";
  constexpr std::string_view kFailed = "[  FAILED  ] ";
  std::vector<TestCaseResult> results;
  std::set<std::string> seen;
  for (const std::string& line : SplitLines(output)) {
    bool passed = false;
    std::string_view rest;
    if (StartsWith(line, kOk)) {
      passed = true;
      rest = std::string_view(line).substr(kOk.size());
    } else if (StartsWith(line, kFailed)) {
      rest = std::string_view(line).substr(kFailed.size());
    } else {
      continue;
    }
    const std::size_t end = rest.find_first_of(" ,");
    const std::string name(rest.substr(0, end));
    if (name.find('.') == std::string::npos) continue;
    if (!seen.insert(name).second) continue;
    results.push_back(TestCaseResult{name, passed});
  }
  return results;
}

std::string DescribeAbnormalEnd(const ProcessResult& result,
                                const Config& config) {
  if (!result.launched) {
    return "it could not be started: " + result.launch_error;
  }
  if (result.cancelled) return "it was cancelled";
  if (result.timed_out) {
    return "it exceeded the wall-clock limit of " +
           config.Get("limits.wall_seconds") +
           " seconds (setting limits.wall_seconds)";
  }
  if (result.output_limit_exceeded) {
    return "it wrote more than " + config.Get("limits.output_mb") +
           " MiB to stdout and stderr (setting limits.output_mb)";
  }
  switch (result.signal) {
    case 0:
      return "";
    case 24:  // SIGXCPU
      return "it exceeded the CPU limit of " +
             config.Get("limits.cpu_seconds") +
             " seconds (setting limits.cpu_seconds)";
    case 9:  // SIGKILL
      return "it was killed (SIGKILL), most likely for exceeding the CPU or "
             "memory limit";
    case 25:  // SIGXFSZ
      return "it tried to write a file larger than " +
             config.Get("limits.file_mb") + " MiB (setting limits.file_mb)";
    case 11:
      return "it crashed with a segmentation fault (SIGSEGV)";
    case 6:
      return "it aborted (SIGABRT)";
    case 8:
      return "it crashed with an arithmetic error (SIGFPE)";
    case 31:
      return "it was stopped for making a forbidden system call (SIGSYS)";
    default:
      return "it was terminated by signal " + std::to_string(result.signal);
  }
}

PublishOutcome ValidateAndPublish(const Plan& plan, const fs::path& staging,
                                  const ProjectPaths& paths, bool force,
                                  const Config& config) {
  PublishOutcome outcome;
  std::error_code ec;

  // 1. Everything in staging must be a regular file or a directory.
  std::map<std::string, std::uintmax_t> staged;
  std::uintmax_t total = 0;
  fs::recursive_directory_iterator it(staging, ec);
  for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    const std::string relative = GenericRelative(it->path(), staging);
    const fs::file_status status = it->symlink_status(ec);
    if (ec) break;
    if (fs::is_directory(status)) continue;
    if (!fs::is_regular_file(status)) {
      outcome.error =
          "the program left something that is not a regular file "
          "in its output directory: " +
          relative;
      return outcome;
    }
    const std::uintmax_t size = fs::file_size(it->path(), ec);
    if (ec) break;
    staged[relative] = size;
    total += size;
  }
  if (ec) {
    outcome.error = "cannot inspect the staging directory: " + ec.message();
    outcome.filesystem_failure = true;
    return outcome;
  }
  const std::uintmax_t staging_limit =
      static_cast<std::uintmax_t>(config.GetInt("limits.staging_mb")) * kMiB;
  if (total > staging_limit) {
    outcome.error = "the program wrote " + FormatBytes(total) +
                    " in total: the limit is " + FormatBytes(staging_limit) +
                    " (setting limits.staging_mb)";
    return outcome;
  }

  // 2. Every declared output must exist, be nonempty, and look like its kind.
  std::set<std::string> declared;
  for (const PlanOutput& output : plan.outputs) {
    declared.insert(output.path);
    const auto found = staged.find(output.path);
    if (found == staged.end()) {
      outcome.error = "declared output was not produced: " + output.path;
      return outcome;
    }
    if (found->second == 0) {
      outcome.error = "declared output is empty: " + output.path;
      return outcome;
    }
    const fs::path file = staging / fs::path(output.path);
    if (output.kind == "png" && !HasPngSignature(file)) {
      outcome.error = "declared PNG output is not a PNG file: " + output.path;
      return outcome;
    }
    if (output.kind == "text" && !IsUtf8File(file)) {
      outcome.error = "declared text output is not valid UTF-8: " + output.path;
      return outcome;
    }
  }
  for (const auto& [name, size] : staged) {
    if (declared.count(name) == 0) outcome.undeclared.push_back(name);
  }

  // 3. Collisions. A file seqc published before, and that nobody edited
  // since, may be replaced. Anything else is the user's and is left alone.
  std::map<std::string, std::string> record =
      ReadPublishedRecord(paths.published);
  for (const PlanOutput& output : plan.outputs) {
    const fs::path target = paths.output / fs::path(output.path);
    // No parent may be a link or a file: publishing must stay inside output/.
    fs::path parent = paths.output;
    const fs::path relative_parent = fs::path(output.path).parent_path();
    for (const fs::path& component : relative_parent) {
      parent /= component;
      const fs::file_status status = fs::symlink_status(parent, ec);
      if (fs::exists(status) && !fs::is_directory(status)) {
        outcome.filesystem_failure = true;
        outcome.error = "cannot publish " + output.path + ": output/" +
                        GenericRelative(parent, paths.output) +
                        " is not a directory";
        return outcome;
      }
    }
    const fs::file_status status = fs::symlink_status(target, ec);
    if (!fs::exists(status)) continue;
    if (fs::is_directory(status)) {
      outcome.filesystem_failure = true;
      outcome.error = "cannot publish " + output.path +
                      ": a directory with that name exists in output/";
      return outcome;
    }
    if (force) continue;
    bool replaceable = false;
    const auto known = record.find(output.path);
    if (known != record.end() && fs::is_regular_file(status)) {
      std::string hash;
      std::string error;
      replaceable = Sha256File(target, &hash, &error) && hash == known->second;
    }
    if (!replaceable) {
      outcome.filesystem_failure = true;
      outcome.error =
          "output/" + output.path +
          (known == record.end() ? " already exists and was not created by seqc"
                                 : " was modified after seqc created it") +
          "; move it away or rerun with --force to replace it";
      return outcome;
    }
  }

  // 4. Publish. Staging is on the same filesystem as output/, so each file
  // moves with one atomic rename.
  for (const PlanOutput& output : plan.outputs) {
    const fs::path source = staging / fs::path(output.path);
    const fs::path target = paths.output / fs::path(output.path);
    PublishedFile published;
    published.path = output.path;
    published.kind = output.kind;
    published.final_artifact = output.final_artifact;
    published.size = staged[output.path];
    std::string error;
    if (!Sha256File(source, &published.sha256, &error)) {
      outcome.filesystem_failure = true;
      outcome.error = error;
      return outcome;
    }
    fs::create_directories(target.parent_path(), ec);
    if (fs::is_symlink(fs::symlink_status(target, ec))) fs::remove(target, ec);
    fs::rename(source, target, ec);
    if (ec) {
      outcome.filesystem_failure = true;
      outcome.error = "cannot publish " + output.path + ": " + ec.message();
      return outcome;
    }
    record[output.path] = published.sha256;
    outcome.published.push_back(std::move(published));
  }
  if (!outcome.published.empty()) {
    std::string error;
    if (!WritePublishedRecord(paths.published, record, &error)) {
      outcome.filesystem_failure = true;
      outcome.error = error;
      return outcome;
    }
  }
  outcome.ok = true;
  return outcome;
}

}  // namespace seq
