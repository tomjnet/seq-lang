#ifndef SEQ_MODEL_HPP_
#define SEQ_MODEL_HPP_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <ostream>
#include <string>

#include "seq/ast.hpp"
#include "support/config.hpp"
#include "support/json.hpp"

namespace seq {

// Sampling and budget settings for one build. Recorded in the build manifest
// and part of the build cache key.
struct GenerationSettings {
  double temperature = 0.0;
  std::int64_t seed = 1;
  int max_output_tokens = 4096;
  int context_tokens = 32768;
  int timeout_seconds = 900;
  std::size_t max_response_bytes = 512 * 1024;

  static GenerationSettings FromConfig(const Config& config);
  Json ToJson() const;
};

struct InferenceRequest {
  // "plan", "generate", "repair", "test", or "test-repair".
  std::string kind;
  std::string system;
  std::string user;
  // JSON schema that the response is constrained to.
  Json schema;
};

struct InferenceResult {
  bool ok = false;
  // The response text; a JSON document when ok.
  std::string content;
  std::string error;
  double seconds = 0.0;
};

// A way to run the declared model. The compiler never depends on one serving
// implementation: everything goes through this interface.
class ModelAdapter {
 public:
  virtual ~ModelAdapter() = default;

  // Short adapter name for messages, such as "llama-server".
  virtual std::string Name() const = 0;

  // What identifies the model this adapter will run. Computable without
  // starting anything, so the build cache can be checked first.
  virtual Json Identity() const = 0;

  // Makes the model ready for inference: verifies weights, starts or contacts
  // the runtime. Called once, only when synthesis is needed.
  virtual bool Start(std::string* error) = 0;

  // Counts prompt tokens so the context budget can be enforced before any
  // inference.
  virtual bool CountTokens(const std::string& text, int* tokens,
                           std::string* error) = 0;

  virtual InferenceResult Complete(const InferenceRequest& request,
                                   const GenerationSettings& settings) = 0;
};

// Model identity pinned for a project in seq.lock.
struct ModelLock {
  std::string declared_url;
  std::string declared_revision;
  std::string repo;
  std::string revision;
  std::string file;
  std::string quantization;
  std::string sha256;
  int grammar_version = 0;
  int prompt_version = 0;

  Json ToJson() const;
  static bool FromJson(const Json& json, ModelLock* lock, std::string* error);
};

bool ReadModelLock(const std::filesystem::path& path, ModelLock* lock,
                   std::string* error);
bool WriteModelLock(const std::filesystem::path& path, const ModelLock& lock,
                    std::string* error);

// Where the weights named by a lock are cached.
std::filesystem::path ModelWeightsPath(const ModelLock& lock);

// Looks up the runtime artifact for a declared model: first the user mapping
// in the configuration, then the built-in table. seqc never guesses a
// substitute; an unknown model is an error.
bool ResolveModelArtifact(const ModelRef& model, const Config& config,
                          ModelLock* lock, std::string* error);

// Creates the adapter selected by `model.adapter`. `lock_path` is the
// project's seq.lock; `log_dir` receives the inference server log.
std::unique_ptr<ModelAdapter> CreateModelAdapter(
    const Config& config, const Workflow& workflow,
    const std::filesystem::path& lock_path,
    const std::filesystem::path& log_dir, std::string* error);

// Implements `seqc model pull` for the project whose declared model is
// `model`. Returns a process exit status.
int ModelPull(const ModelRef& model, const Config& config,
              const std::filesystem::path& lock_path, bool update,
              std::ostream& out, std::ostream& err);

}  // namespace seq

#endif  // SEQ_MODEL_HPP_
