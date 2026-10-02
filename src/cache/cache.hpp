#ifndef SEQ_CACHE_CACHE_HPP_
#define SEQ_CACHE_CACHE_HPP_

#include <string>

#include "planner/planner.hpp"
#include "project/project.hpp"
#include "support/json.hpp"

namespace seq {

// SHA-256 over everything that determines a build: the normalized source,
// the input manifest, the locked model identity, generation settings, prompt
// and grammar versions, the runtime, and the toolchain.
std::string ComputeCacheKey(const Json& components);

// File names of the accepted build of workflow `name`.
struct ArtifactPaths {
  std::filesystem::path source;       // output/temp/<name>.c
  std::filesystem::path assembly;     // output/temp/<name>.s
  std::filesystem::path binary;       // output/temp/<name>.bin
  std::filesystem::path test_source;  // output/temp/test/<name>_test.cc
  std::filesystem::path test_binary;  // output/temp/test/<name>_test.bin
};

ArtifactPaths ArtifactPathsFor(const ProjectPaths& paths,
                               const std::string& name);

// Loads the accepted build if its key equals `cache_key` and its artifacts
// are present and unchanged. On a miss *reason says why.
bool LoadAcceptedBuild(const ProjectPaths& paths, const std::string& name,
                       const std::string& cache_key, Json* manifest, Plan* plan,
                       std::string* reason);

}  // namespace seq

#endif  // SEQ_CACHE_CACHE_HPP_
