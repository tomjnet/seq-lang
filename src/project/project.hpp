#ifndef SEQ_PROJECT_PROJECT_HPP_
#define SEQ_PROJECT_PROJECT_HPP_

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

#include "support/config.hpp"
#include "support/json.hpp"

namespace seq {

// Where the compiler-owned runtime and templates live on this machine.
struct Installation {
  std::filesystem::path home;
  std::filesystem::path include_dir;    // seq_runtime.h
  std::filesystem::path lib_dir;        // libseqrt.a
  std::filesystem::path templates_dir;  // gitignore.template, Makefile.template
};

// Resolves the installation from the `paths.home` setting (SEQC_HOME) or, by
// default, <directory of seqc>/../lib/seqc.
Installation LocateInstallation(const Config& config);

// Checks that the two project templates are present.
bool HasTemplates(const Installation& install, std::string* error);
// Checks that the runtime header and library are present.
bool HasRuntime(const Installation& install, std::string* error);

// Paths of one Seq project. Everything is bound to the resolved project root,
// independent of the caller's working directory.
struct ProjectPaths {
  std::filesystem::path root;
  std::filesystem::path source;      // src/main.seq
  std::filesystem::path input;       // input/
  std::filesystem::path output;      // output/
  std::filesystem::path temp;        // output/temp/
  std::filesystem::path test;        // output/temp/test/
  std::filesystem::path runs;        // output/temp/runs/
  std::filesystem::path lock_file;   // seq.lock
  std::filesystem::path build_file;  // output/temp/build.json
  std::filesystem::path published;   // output/temp/published.json
  std::filesystem::path busy_lock;   // output/temp/.seqc.lock
};

ProjectPaths ProjectPathsForRoot(const std::filesystem::path& root);

// Derives the project from its entry point, which must be
// <project>/src/main.seq.
bool ResolveProject(const std::filesystem::path& source_file,
                    ProjectPaths* paths, std::string* error);

// The starter workflow written by `seqc new`.
std::string StarterSource(const std::string& name);

// Implements `seqc new <name>` in `parent`. Prints every created path to
// `out`. Returns false and removes anything it created on failure; an
// existing destination is refused and left untouched.
bool ScaffoldProject(const std::string& name,
                     const std::filesystem::path& parent,
                     const Installation& install, std::ostream& out,
                     std::string* error);

// Creates output/temp/ and output/temp/test/ and restores a missing
// .gitignore or Makefile from the templates. Existing copies are never
// overwritten.
bool EnsureProjectLayout(const ProjectPaths& paths, const Installation& install,
                         std::string* error);

struct InputFile {
  // Relative to input/, with forward slashes.
  std::string path;
  std::uintmax_t size = 0;
  // "text", "png", or "binary".
  std::string type;
  std::string sha256;
  // Leading bytes of a text file that are shown to the model.
  std::string preview;
  bool preview_truncated = false;
};

struct ExcludedInput {
  std::string path;
  std::string reason;
};

struct InputManifest {
  std::vector<InputFile> files;
  std::vector<ExcludedInput> excluded;
  std::uintmax_t total_bytes = 0;
  std::uint64_t preview_bytes = 0;

  // Full manifest, written to the run directory.
  Json ToJson() const;
  // Only what identifies the inputs (paths and hashes), for the cache key.
  Json Identity() const;
};

// Enumerates input/ recursively in a stable order. Symbolic links, special
// files, and exceeded limits are errors; the `.empty` marker is excluded.
bool EnumerateInputs(const ProjectPaths& paths, const Config& config,
                     InputManifest* manifest, std::string* error);

// Implements `seqc clean`: removes output/temp/runs/, and with `all` also the
// build artifacts. Published outputs, input/, the templates, and the record of
// published files are never touched.
bool CleanProject(const ProjectPaths& paths, bool all, std::ostream& out,
                  std::string* error);

}  // namespace seq

#endif  // SEQ_PROJECT_PROJECT_HPP_
