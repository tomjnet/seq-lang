// `seqc model pull`: the only place seqc downloads anything. A compile never
// downloads implicitly.

#include <cstdlib>
#include <iostream>
#include <system_error>

#include "seq/exit_codes.hpp"
#include "seq/model.hpp"
#include "seq/version.hpp"
#include "support/process.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

// curl inherits only what it needs to reach the network.
std::vector<std::string> DownloadEnvironment() {
  std::vector<std::string> env;
  for (const char* name : {"PATH", "HOME", "HTTPS_PROXY", "https_proxy",
                           "HTTP_PROXY", "http_proxy", "NO_PROXY", "no_proxy",
                           "SSL_CERT_FILE", "SSL_CERT_DIR", "CURL_CA_BUNDLE"}) {
    if (const char* value = std::getenv(name); value != nullptr) {
      env.push_back(std::string(name) + "=" + value);
    }
  }
  return env;
}

std::string AllowedProtocols(const std::string& hub) {
  // Plain HTTP is accepted only when the hub itself was configured that way,
  // which the test suite does with a local server.
  return StartsWith(hub, "http://") ? "=http,https" : "=https";
}

bool FetchJson(const fs::path& curl, const std::string& hub,
               const std::string& url, const fs::path& scratch, Json* json,
               std::string* error) {
  ProcessSpec spec;
  spec.argv = {curl.string(),
               "--fail",
               "--silent",
               "--show-error",
               "--location",
               "--proto",
               AllowedProtocols(hub),
               "--max-time",
               "120",
               "--output",
               scratch.string(),
               url};
  spec.env = DownloadEnvironment();
  const ProcessResult result = RunProcess(spec);
  if (!result.Succeeded()) {
    *error = "cannot fetch " + url + ": " +
             (result.launched ? std::string(TrimWhitespace(result.err))
                              : result.launch_error);
    return false;
  }
  std::string text;
  if (!ReadFile(scratch, &text, error)) return false;
  std::error_code ec;
  fs::remove(scratch, ec);
  std::string parse_error;
  if (!Json::Parse(text, json, &parse_error)) {
    *error = "unexpected response from " + url + ": " + parse_error;
    return false;
  }
  return true;
}

// Re-resolves the newest revision of the GGUF repository and the hash the hub
// records for the file.
bool Reresolve(const fs::path& curl, const std::string& hub,
               const fs::path& scratch, ModelLock* lock, std::string* error) {
  Json info;
  if (!FetchJson(curl, hub, hub + "/api/models/" + lock->repo, scratch, &info,
                 error)) {
    return false;
  }
  const std::string revision = info.GetString("sha");
  if (revision.empty()) {
    *error = "the hub did not report a revision for " + lock->repo;
    return false;
  }
  Json tree;
  if (!FetchJson(curl, hub,
                 hub + "/api/models/" + lock->repo + "/tree/" + revision,
                 scratch, &tree, error)) {
    return false;
  }
  if (tree.is_array()) {
    for (const Json& entry : tree.AsArray()) {
      if (entry.GetString("path") != lock->file) continue;
      const Json* lfs = entry.Find("lfs");
      const std::string hash = lfs != nullptr ? lfs->GetString("oid") : "";
      if (hash.size() != 64) break;
      lock->revision = revision;
      lock->sha256 = hash;
      return true;
    }
  }
  *error = "the hub does not list " + lock->file + " with a SHA-256 in " +
           lock->repo + " at " + revision;
  return false;
}

}  // namespace

int ModelPull(const ModelRef& model, const Config& config,
              const fs::path& lock_path, bool update, std::ostream& out,
              std::ostream& err) {
  std::string error;
  ModelLock lock;
  std::error_code ec;
  const bool had_lock = fs::exists(lock_path, ec);
  if (had_lock) {
    if (!ReadModelLock(lock_path, &lock, &error)) {
      err << "seqc: error: " << error << "\n";
      return kExitModel;
    }
    if (lock.declared_url != model.url) {
      if (!update) {
        err << "seqc: error: seq.lock was written for " << lock.declared_url
            << " but the workflow declares " << model.url
            << "\n  run `seqc model pull --update` to lock the new model\n";
        return kExitModel;
      }
      if (!ResolveModelArtifact(model, config, &lock, &error)) {
        err << "seqc: error: " << error << "\n";
        return kExitModel;
      }
    }
  } else if (!ResolveModelArtifact(model, config, &lock, &error)) {
    err << "seqc: error: " << error << "\n";
    return kExitModel;
  }
  lock.declared_url = model.url;
  lock.declared_revision = model.revision;
  lock.grammar_version = kGrammarVersion;
  lock.prompt_version = kPromptVersion;

  const std::optional<fs::path> curl = FindProgram("curl");
  const std::string hub = config.Get("model.hub_url");
  const fs::path cache = Config::CacheDir() / "models";
  fs::create_directories(cache, ec);
  if (ec) {
    err << "seqc: error: cannot create " << cache.string() << ": "
        << ec.message() << "\n";
    return kExitFilesystem;
  }

  if (update) {
    if (!curl.has_value()) {
      err << "seqc: error: curl was not found; it is required to download "
             "models\n";
      return kExitPrerequisite;
    }
    out << "[resolve] " << hub << "/" << lock.repo << "\n";
    if (!Reresolve(*curl, hub, cache / "resolve.tmp", &lock, &error)) {
      err << "seqc: error: " << error << "\n";
      return kExitModel;
    }
  }

  out << "Model:        " << model.url << "\n"
      << "Artifact:     " << lock.repo << " / " << lock.file << "\n"
      << "Revision:     " << lock.revision << "\n"
      << "SHA-256:      " << lock.sha256 << "\n";

  const fs::path weights = ModelWeightsPath(lock);
  bool present = false;
  if (fs::is_regular_file(weights, ec)) {
    out << "[verify] " << weights.string() << "\n";
    std::string hash;
    if (!Sha256File(weights, &hash, &error)) {
      err << "seqc: error: " << error << "\n";
      return kExitFilesystem;
    }
    if (hash == lock.sha256) {
      present = true;
    } else {
      out << "[verify] cached file does not match; downloading again\n";
      fs::remove(weights, ec);
    }
  }

  if (!present) {
    if (!curl.has_value()) {
      err << "seqc: error: curl was not found; it is required to download "
             "models\n";
      return kExitPrerequisite;
    }
    const std::string url =
        hub + "/" + lock.repo + "/resolve/" + lock.revision + "/" + lock.file;
    fs::create_directories(weights.parent_path(), ec);
    fs::path partial = weights;
    partial += ".partial";
    out << "[download] " << url << "\n"
        << "           to " << weights.string() << "\n"
        << "           Only this file is fetched; nothing from the project "
           "is sent.\n";
    out.flush();

    ProcessSpec spec;
    spec.argv = {curl->string(),
                 "--fail",
                 "--location",
                 "--proto",
                 AllowedProtocols(hub),
                 "--retry",
                 "3",
                 "--continue-at",
                 "-",
                 "--progress-bar",
                 "--output",
                 partial.string(),
                 url};
    spec.env = DownloadEnvironment();
    spec.live_stderr = &err;
    const ProcessResult result = RunProcess(spec);
    if (!result.Succeeded()) {
      err << "seqc: error: download failed"
          << (result.launched ? "" : ": " + result.launch_error) << "\n";
      return kExitModel;
    }
    out << "[verify] " << partial.string() << "\n";
    std::string hash;
    if (!Sha256File(partial, &hash, &error)) {
      err << "seqc: error: " << error << "\n";
      return kExitFilesystem;
    }
    if (hash != lock.sha256) {
      fs::remove(partial, ec);
      err << "seqc: error: the downloaded file does not match the pinned "
             "SHA-256\n  expected "
          << lock.sha256 << "\n  got      " << hash
          << "\n  The file was deleted. Nothing was locked.\n";
      return kExitModel;
    }
    fs::rename(partial, weights, ec);
    if (ec) {
      err << "seqc: error: cannot store " << weights.string() << ": "
          << ec.message() << "\n";
      return kExitFilesystem;
    }
  }

  if (!WriteModelLock(lock_path, lock, &error)) {
    err << "seqc: error: " << error << "\n";
    return kExitFilesystem;
  }
  out << "[lock] " << lock_path.string() << "\n"
      << "Model is ready.\n";
  return kExitOk;
}

}  // namespace seq
