#include "project/project.hpp"

#include <algorithm>
#include <system_error>

#include "seq/ast.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"
#include "validation/validator.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

constexpr char kGitignoreTemplate[] = "gitignore.template";
constexpr char kMakefileTemplate[] = "Makefile.template";

constexpr char kEmptyMarker[] =
    "Files placed in this directory are read-only input for the workflow.\n"
    "The generated program can open them with seq_input_open(). This marker\n"
    "file itself is ignored.\n";

bool CopyTemplate(const fs::path& from, const fs::path& to,
                  std::string* error) {
  std::string data;
  if (!ReadFile(from, &data, error)) return false;
  return WriteFile(to, data, error);
}

bool MakeDirectory(const fs::path& path, std::string* error) {
  std::error_code ec;
  fs::create_directories(path, ec);
  if (ec) {
    *error = "cannot create " + path.string() + ": " + ec.message();
    return false;
  }
  return true;
}

std::string ClassifyContent(const std::string& data) {
  static constexpr char kPngSignature[] = "\x89PNG\r\n\x1a\n";
  if (data.size() >= 8 && data.compare(0, 8, kPngSignature, 8) == 0) {
    return "png";
  }
  if (data.find('\0') == std::string::npos && IsValidUtf8(data)) return "text";
  return "binary";
}

}  // namespace

Installation LocateInstallation(const Config& config) {
  Installation install;
  const std::string configured = config.Get("paths.home");
  std::error_code ec;
  if (!configured.empty()) {
    install.home = fs::absolute(fs::path(configured), ec);
  } else {
    const fs::path exe = ExecutablePath();
    if (!exe.empty()) {
      install.home = exe.parent_path().parent_path() / "lib" / "seqc";
    }
  }
  const fs::path canonical = fs::weakly_canonical(install.home, ec);
  if (!ec && !canonical.empty()) install.home = canonical;
  install.include_dir = install.home / "include";
  install.lib_dir = install.home / "lib";
  install.templates_dir = install.home / "templates";
  return install;
}

bool HasTemplates(const Installation& install, std::string* error) {
  std::error_code ec;
  for (const char* name : {kGitignoreTemplate, kMakefileTemplate}) {
    if (!fs::is_regular_file(install.templates_dir / name, ec)) {
      *error = "seqc template not found: " +
               (install.templates_dir / name).string() +
               " (set SEQC_HOME to the seqc installation directory)";
      return false;
    }
  }
  return true;
}

bool HasRuntime(const Installation& install, std::string* error) {
  std::error_code ec;
  for (const fs::path& path : {install.include_dir / "seq_runtime.h",
                               install.lib_dir / "libseqrt.a"}) {
    if (!fs::is_regular_file(path, ec)) {
      *error = "seqc runtime not found: " + path.string() +
               " (set SEQC_HOME to the seqc installation directory)";
      return false;
    }
  }
  return true;
}

ProjectPaths ProjectPathsForRoot(const fs::path& root) {
  ProjectPaths paths;
  paths.root = root;
  paths.source = root / "src" / "main.seq";
  paths.input = root / "input";
  paths.output = root / "output";
  paths.temp = paths.output / "temp";
  paths.test = paths.temp / "test";
  paths.runs = paths.temp / "runs";
  paths.lock_file = root / "seq.lock";
  paths.build_file = paths.temp / "build.json";
  paths.published = paths.temp / "published.json";
  paths.busy_lock = paths.temp / ".seqc.lock";
  return paths;
}

bool ResolveProject(const fs::path& source_file, ProjectPaths* paths,
                    std::string* error) {
  std::error_code ec;
  const fs::path canonical = fs::canonical(source_file, ec);
  if (ec) {
    *error = "file not found: " + source_file.string();
    return false;
  }
  if (canonical.filename() != "main.seq" ||
      canonical.parent_path().filename() != "src") {
    *error = "the entry point must be <project>/src/main.seq, got " +
             source_file.string();
    return false;
  }
  *paths = ProjectPathsForRoot(canonical.parent_path().parent_path());
  paths->source = canonical;
  return true;
}

std::string StarterSource(const std::string& name) {
  return std::string("model(\"") + kReferenceModelUrl +
         "\")\n"
         "backend.C()\n"
         "name = \"" +
         name +
         "\"\n"
         "\n"
         "step step1():\n"
         "    ask(\"print Hello from Seq\")\n";
}

bool ScaffoldProject(const std::string& name, const fs::path& parent,
                     const Installation& install, std::ostream& out,
                     std::string* error) {
  std::string reason;
  if (!IsValidProjectName(name, &reason)) {
    *error = "invalid project name '" + name + "': " + reason;
    return false;
  }
  if (!HasTemplates(install, error)) return false;

  const fs::path root = parent / name;
  std::error_code ec;
  if (fs::exists(fs::symlink_status(root, ec))) {
    *error = "destination already exists: " + root.string();
    return false;
  }

  out << "Seq Compiler\n\nCreating project: " << name << "\n\n";

  const ProjectPaths paths = ProjectPathsForRoot(root);
  const auto created = [&](const fs::path& path, bool directory) {
    out << "[create] " << name;
    const std::string relative = GenericRelative(path, root);
    if (relative != ".") out << "/" << relative;
    out << (directory ? "/" : "") << "\n";
  };
  const auto make_dir = [&](const fs::path& path) {
    if (!MakeDirectory(path, error)) return false;
    created(path, true);
    return true;
  };
  const auto make_file = [&](const fs::path& path, const std::string& data) {
    if (!WriteFile(path, data, error)) return false;
    created(path, false);
    return true;
  };
  const auto copy_template = [&](const char* source, const fs::path& path) {
    if (!CopyTemplate(install.templates_dir / source, path, error)) {
      return false;
    }
    created(path, false);
    return true;
  };

  const bool ok =
      make_dir(root) && make_dir(root / "src") &&
      make_file(paths.source, StarterSource(name)) && make_dir(paths.input) &&
      make_file(paths.input / ".empty", kEmptyMarker) &&
      make_dir(paths.output) && make_dir(paths.temp) &&
      copy_template(kGitignoreTemplate, paths.temp / ".gitignore") &&
      copy_template(kMakefileTemplate, paths.temp / "Makefile") &&
      make_dir(paths.test);
  if (!ok) {
    // Only this call created `root`, so removing it cannot touch user data.
    fs::remove_all(root, ec);
    return false;
  }

  out << "\nModel: " << kReferenceModelUrl
      << "\n"
         "Backend: C\n"
         "Target: Linux x86_64\n"
         "\n"
         "Project created successfully.\n"
         "\n"
         "Next:\n"
         "  cd "
      << name
      << "\n"
         "  seqc doctor\n"
         "  seqc model pull\n"
         "  seqc src/main.seq\n"
         "\n"
         "Execution requires a configured model runtime, GCC, g++, and Google "
         "Test.\n";
  return true;
}

bool EnsureProjectLayout(const ProjectPaths& paths, const Installation& install,
                         std::string* error) {
  if (!HasTemplates(install, error)) return false;
  if (!MakeDirectory(paths.temp, error) || !MakeDirectory(paths.test, error)) {
    return false;
  }
  std::error_code ec;
  const std::pair<const char*, fs::path> templates[] = {
      {kGitignoreTemplate, paths.temp / ".gitignore"},
      {kMakefileTemplate, paths.temp / "Makefile"},
  };
  for (const auto& [source, target] : templates) {
    if (fs::exists(fs::symlink_status(target, ec))) continue;
    if (!CopyTemplate(install.templates_dir / source, target, error)) {
      return false;
    }
  }
  return true;
}

Json InputManifest::ToJson() const {
  Json json = Json::MakeObject();
  json.Set("schema_version", 1);
  Json list = Json::MakeArray();
  for (const InputFile& file : files) {
    Json entry = Json::MakeObject();
    entry.Set("path", file.path);
    entry.Set("size", file.size);
    entry.Set("type", file.type);
    entry.Set("sha256", file.sha256);
    // What is shown to the model, as opposed to what the generated program
    // can read (the whole file).
    entry.Set("bytes_shown_to_model", file.preview.size());
    entry.Set("preview_truncated", file.preview_truncated);
    list.Push(std::move(entry));
  }
  json.Set("files", std::move(list));
  Json skipped = Json::MakeArray();
  for (const ExcludedInput& item : excluded) {
    skipped.Push(
        Json::MakeObject().Set("path", item.path).Set("reason", item.reason));
  }
  json.Set("excluded", std::move(skipped));
  json.Set("total_bytes", total_bytes);
  json.Set("preview_policy",
           "The model sees each file's path, size, type, and hash, and at "
           "most the first " +
               std::to_string(preview_bytes) +
               " bytes of each text file. Binary files are never shown.");
  return json;
}

Json InputManifest::Identity() const {
  Json list = Json::MakeArray();
  for (const InputFile& file : files) {
    list.Push(
        Json::MakeObject().Set("path", file.path).Set("sha256", file.sha256));
  }
  return list;
}

bool EnumerateInputs(const ProjectPaths& paths, const Config& config,
                     InputManifest* manifest, std::string* error) {
  *manifest = InputManifest();
  const std::uintmax_t max_file =
      static_cast<std::uintmax_t>(config.GetInt("inputs.max_file_mb")) * 1024 *
      1024;
  const std::uintmax_t max_total =
      static_cast<std::uintmax_t>(config.GetInt("inputs.max_total_mb")) * 1024 *
      1024;
  const std::size_t max_files =
      static_cast<std::size_t>(config.GetInt("inputs.max_files"));
  manifest->preview_bytes =
      static_cast<std::uint64_t>(config.GetInt("inputs.preview_bytes"));

  std::error_code ec;
  const fs::file_status root_status = fs::symlink_status(paths.input, ec);
  if (!fs::exists(root_status)) return true;  // No input directory: no inputs.
  if (fs::is_symlink(root_status) || !fs::is_directory(root_status)) {
    *error = "input/ must be a real directory, not a link or a file";
    return false;
  }

  std::vector<fs::path> found;
  fs::recursive_directory_iterator it(paths.input, ec);
  if (ec) {
    *error = "cannot read " + paths.input.string() + ": " + ec.message();
    return false;
  }
  for (; it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      *error = "cannot read " + paths.input.string() + ": " + ec.message();
      return false;
    }
    const fs::path path = it->path();
    const std::string relative = GenericRelative(path, paths.input);
    const fs::file_status status = it->symlink_status(ec);
    if (ec) {
      *error = "cannot inspect input/" + relative + ": " + ec.message();
      return false;
    }
    if (fs::is_symlink(status)) {
      *error = "input/" + relative +
               " is a symbolic link; links are not accepted as input";
      return false;
    }
    if (fs::is_directory(status)) continue;
    if (!fs::is_regular_file(status)) {
      *error = "input/" + relative +
               " is not a regular file; devices, sockets, and pipes are not "
               "accepted as input";
      return false;
    }
    if (!IsSafeRelativePath(relative)) {
      *error =
          "input file name is not acceptable (control characters, "
          "backslashes, or invalid UTF-8): input/" +
          relative;
      return false;
    }
    if (relative == ".empty") {
      manifest->excluded.push_back({relative, "placeholder marker"});
      continue;
    }
    found.push_back(path);
  }
  if (ec) {
    *error = "cannot read " + paths.input.string() + ": " + ec.message();
    return false;
  }

  // Byte order of the relative path: the same on every machine and locale.
  std::sort(found.begin(), found.end(),
            [&](const fs::path& a, const fs::path& b) {
              return GenericRelative(a, paths.input) <
                     GenericRelative(b, paths.input);
            });
  if (found.size() > max_files) {
    *error = "input/ holds " + std::to_string(found.size()) +
             " files: the limit is " + std::to_string(max_files) +
             " (setting inputs.max_files)";
    return false;
  }

  for (const fs::path& path : found) {
    InputFile file;
    file.path = GenericRelative(path, paths.input);
    file.size = fs::file_size(path, ec);
    if (ec) {
      *error = "cannot read input/" + file.path + ": " + ec.message();
      return false;
    }
    if (file.size > max_file) {
      *error = "input/" + file.path + " is " + FormatBytes(file.size) +
               ": the limit is " + FormatBytes(max_file) +
               " (setting inputs.max_file_mb)";
      return false;
    }
    manifest->total_bytes += file.size;
    if (manifest->total_bytes > max_total) {
      *error = "input/ is larger than " + FormatBytes(max_total) +
               " in total (setting inputs.max_total_mb)";
      return false;
    }
    std::string data;
    if (!ReadFile(path, &data, error, max_file)) return false;
    file.sha256 = Sha256Hex(data);
    file.type = ClassifyContent(data);
    if (file.type == "text") {
      std::size_t length =
          std::min<std::size_t>(data.size(), manifest->preview_bytes);
      // Do not cut a multi-byte character in half.
      while (length > 0 && length < data.size() &&
             (static_cast<unsigned char>(data[length]) & 0xC0) == 0x80) {
        --length;
      }
      file.preview = data.substr(0, length);
      file.preview_truncated = length < data.size();
    }
    manifest->files.push_back(std::move(file));
  }
  return true;
}

bool CleanProject(const ProjectPaths& paths, bool all, std::ostream& out,
                  std::string* error) {
  std::error_code ec;
  const auto remove_tree = [&](const fs::path& path) {
    if (!fs::exists(fs::symlink_status(path, ec))) return true;
    fs::remove_all(path, ec);
    if (ec) {
      *error = "cannot remove " + path.string() + ": " + ec.message();
      return false;
    }
    out << "[remove] " << GenericRelative(path, paths.root) << "\n";
    return true;
  };

  if (!remove_tree(paths.runs)) return false;
  if (!all) return true;

  if (!remove_tree(paths.build_file)) return false;
  // Build artifacts are recognized by their extension so that the templates
  // and the published-file record stay in place.
  const auto remove_matching =
      [&](const fs::path& directory,
          std::initializer_list<const char*> suffixes) {
        if (!fs::is_directory(directory, ec)) return true;
        std::vector<fs::path> doomed;
        for (const fs::directory_entry& entry :
             fs::directory_iterator(directory, ec)) {
          if (!entry.is_regular_file(ec) || entry.is_symlink(ec)) continue;
          const std::string name = entry.path().filename().string();
          for (const char* suffix : suffixes) {
            if (EndsWith(name, suffix)) {
              doomed.push_back(entry.path());
              break;
            }
          }
        }
        std::sort(doomed.begin(), doomed.end());
        for (const fs::path& path : doomed) {
          if (!remove_tree(path)) return false;
        }
        return true;
      };
  return remove_matching(paths.temp, {".c", ".s", ".bin"}) &&
         remove_matching(paths.test, {"_test.cc", "_test.bin", ".o"});
}

}  // namespace seq
