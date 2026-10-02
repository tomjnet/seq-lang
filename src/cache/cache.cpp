#include "cache/cache.hpp"

#include <system_error>

#include "support/sha256.hpp"
#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

std::string ComputeCacheKey(const Json& components) {
  return Sha256Hex(components.Dump(-1));
}

ArtifactPaths ArtifactPathsFor(const ProjectPaths& paths,
                               const std::string& name) {
  ArtifactPaths artifacts;
  artifacts.source = paths.temp / (name + ".c");
  artifacts.assembly = paths.temp / (name + ".s");
  artifacts.binary = paths.temp / (name + ".bin");
  artifacts.test_source = paths.test / (name + "_test.cc");
  artifacts.test_binary = paths.test / (name + "_test.bin");
  return artifacts;
}

bool LoadAcceptedBuild(const ProjectPaths& paths, const std::string& name,
                       const std::string& cache_key, Json* manifest, Plan* plan,
                       std::string* reason) {
  std::error_code ec;
  if (!fs::exists(paths.build_file, ec)) {
    *reason = "no accepted build yet";
    return false;
  }
  std::string text;
  std::string error;
  if (!ReadFile(paths.build_file, &text, &error) ||
      !Json::Parse(text, manifest, &error) || !manifest->is_object()) {
    *reason = "the build manifest is unreadable";
    return false;
  }
  if (manifest->GetString("cache_key") != cache_key) {
    *reason = "the workflow, its inputs, the model, or the toolchain changed";
    return false;
  }
  const Json* plan_json = manifest->Find("plan");
  if (plan_json == nullptr || !PlanFromJson(*plan_json, plan)) {
    *reason = "the build manifest has no usable plan";
    return false;
  }

  // The artifacts must be the ones the manifest describes: a stale or edited
  // executable is never run.
  const ArtifactPaths artifacts = ArtifactPathsFor(paths, name);
  const Json* hashes = manifest->Find("artifacts");
  const std::pair<const char*, fs::path> expected[] = {
      {"source", artifacts.source},
      {"assembly", artifacts.assembly},
      {"binary", artifacts.binary},
      {"test_source", artifacts.test_source},
      {"test_binary", artifacts.test_binary},
  };
  for (const auto& [key, path] : expected) {
    std::string hash;
    if (!fs::is_regular_file(path, ec) || !Sha256File(path, &hash, &error)) {
      *reason =
          "build artifact is missing: " + GenericRelative(path, paths.root);
      return false;
    }
    if (hashes == nullptr || hashes->GetString(key) != hash) {
      *reason = "build artifact changed since it was accepted: " +
                GenericRelative(path, paths.root);
      return false;
    }
  }
  return true;
}

}  // namespace seq
