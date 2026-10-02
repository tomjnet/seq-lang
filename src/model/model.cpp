#include "seq/model.hpp"

#include <optional>
#include <system_error>

#include "model/adapters.hpp"
#include "seq/version.hpp"
#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

// Declared repository -> runtime artifact. The declared repository holds
// safetensors weights; llama.cpp needs a quantized GGUF file that is published
// in a different repository, so the mapping is explicit and pinned.
struct KnownModel {
  const char* declared_repo;
  const char* gguf_repo;
  const char* file;
  const char* quantization;
  const char* revision;
  const char* sha256;
};

// Revision and hash as reported by the Hugging Face API on 2026-09-30.
// `seqc model pull` verifies the download against the hash, so a wrong pin
// fails closed instead of running other weights.
constexpr KnownModel kKnownModels[] = {
    {"Qwen/Qwen2.5-Coder-1.5B-Instruct",
     "Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF",
     "qwen2.5-coder-1.5b-instruct-q4_k_m.gguf", "q4_k_m",
     "f86cb2c1fa58255f8052cc32aeede1b7482d4361",
     "cc324af070c2ecbfd324a30884d2f951a7ff756aba85cb811a6ec436933bb046"},
};

bool IsSha256Hex(const std::string& text) {
  if (text.size() != 64) return false;
  for (const char c : text) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

}  // namespace

GenerationSettings GenerationSettings::FromConfig(const Config& config) {
  GenerationSettings settings;
  settings.seed = config.GetInt("model.seed");
  settings.max_output_tokens =
      static_cast<int>(config.GetInt("model.max_output_tokens"));
  settings.context_tokens =
      static_cast<int>(config.GetInt("model.context_tokens"));
  settings.timeout_seconds =
      static_cast<int>(config.GetInt("model.timeout_seconds"));
  settings.max_response_bytes =
      static_cast<std::size_t>(config.GetInt("model.max_response_kib")) * 1024;
  return settings;
}

Json GenerationSettings::ToJson() const {
  return Json::MakeObject()
      .Set("temperature", temperature)
      .Set("seed", seed)
      .Set("max_output_tokens", max_output_tokens)
      .Set("context_tokens", context_tokens);
}

Json ModelLock::ToJson() const {
  return Json::MakeObject()
      .Set("lock_version", 1)
      .Set("declared_url", declared_url)
      .Set("declared_revision", declared_revision)
      .Set("repo", repo)
      .Set("revision", revision)
      .Set("file", file)
      .Set("quantization", quantization)
      .Set("sha256", sha256)
      .Set("grammar_version", grammar_version)
      .Set("prompt_version", prompt_version);
}

bool ModelLock::FromJson(const Json& json, ModelLock* lock,
                         std::string* error) {
  if (!json.is_object() || json.GetInt("lock_version") != 1) {
    *error = "unsupported lock file version";
    return false;
  }
  lock->declared_url = json.GetString("declared_url");
  lock->declared_revision = json.GetString("declared_revision");
  lock->repo = json.GetString("repo");
  lock->revision = json.GetString("revision");
  lock->file = json.GetString("file");
  lock->quantization = json.GetString("quantization");
  lock->sha256 = json.GetString("sha256");
  lock->grammar_version = static_cast<int>(json.GetInt("grammar_version"));
  lock->prompt_version = static_cast<int>(json.GetInt("prompt_version"));
  if (lock->declared_url.empty() || lock->repo.empty() ||
      lock->revision.empty() || !IsSafeRelativePath(lock->file) ||
      lock->file.find('/') != std::string::npos || !IsSha256Hex(lock->sha256)) {
    *error = "lock file is incomplete or malformed";
    return false;
  }
  return true;
}

bool ReadModelLock(const fs::path& path, ModelLock* lock, std::string* error) {
  std::string text;
  if (!ReadFile(path, &text, error, 1024 * 1024)) return false;
  Json json;
  std::string parse_error;
  if (!Json::Parse(text, &json, &parse_error)) {
    *error = path.string() + ": " + parse_error;
    return false;
  }
  if (!ModelLock::FromJson(json, lock, &parse_error)) {
    *error = path.string() + ": " + parse_error;
    return false;
  }
  return true;
}

bool WriteModelLock(const fs::path& path, const ModelLock& lock,
                    std::string* error) {
  return WriteFile(path, lock.ToJson().Dump() + "\n", error);
}

fs::path ModelWeightsPath(const ModelLock& lock) {
  return Config::CacheDir() / "models" / lock.sha256 / lock.file;
}

bool ResolveModelArtifact(const ModelRef& model, const Config& config,
                          ModelLock* lock, std::string* error) {
  lock->declared_url = model.url;
  lock->declared_revision = model.revision;
  lock->grammar_version = kGrammarVersion;
  lock->prompt_version = kPromptVersion;

  const std::string user_repo = config.Get("model.gguf_repo");
  const std::string user_file = config.Get("model.gguf_file");
  const std::string user_revision = config.Get("model.gguf_revision");
  const std::string user_sha = config.Get("model.gguf_sha256");
  if (!user_repo.empty() || !user_file.empty() || !user_revision.empty() ||
      !user_sha.empty()) {
    if (user_repo.empty() || user_file.empty() || user_revision.empty() ||
        user_sha.empty()) {
      *error =
          "the user model mapping is incomplete: model.gguf_repo, "
          "model.gguf_file, model.gguf_revision, and model.gguf_sha256 "
          "must all be set";
      return false;
    }
    if (!IsSha256Hex(user_sha)) {
      *error = "model.gguf_sha256 must be 64 lowercase hex digits";
      return false;
    }
    if (!IsSafeRelativePath(user_file) ||
        user_file.find('/') != std::string::npos) {
      *error = "model.gguf_file must be a plain file name";
      return false;
    }
    lock->repo = user_repo;
    lock->file = user_file;
    lock->revision = user_revision;
    lock->sha256 = user_sha;
    lock->quantization = config.Get("model.gguf_quantization");
    return true;
  }

  const std::string declared = model.owner + "/" + model.repo;
  for (const KnownModel& known : kKnownModels) {
    if (declared == known.declared_repo) {
      lock->repo = known.gguf_repo;
      lock->file = known.file;
      lock->quantization = known.quantization;
      lock->revision = known.revision;
      lock->sha256 = known.sha256;
      return true;
    }
  }
  *error = "no runtime artifact is known for " + model.url +
           "; set model.gguf_repo, model.gguf_file, model.gguf_revision, and "
           "model.gguf_sha256 in the configuration to map it to a GGUF file";
  return false;
}

std::unique_ptr<ModelAdapter> CreateModelAdapter(const Config& config,
                                                 const Workflow& workflow,
                                                 const fs::path& lock_path,
                                                 const fs::path& log_dir,
                                                 std::string* error) {
  const std::string kind = config.Get("model.adapter");

  std::optional<ModelLock> lock;
  std::error_code ec;
  if (fs::exists(lock_path, ec)) {
    ModelLock loaded;
    if (!ReadModelLock(lock_path, &loaded, error)) return nullptr;
    if (loaded.declared_url != workflow.model.url) {
      *error = "seq.lock was written for " + loaded.declared_url +
               " but the workflow declares " + workflow.model.url +
               "; run `seqc model pull --update`";
      return nullptr;
    }
    lock = std::move(loaded);
  }

  if (kind == "fake") return CreateFakeAdapter(config, lock, error);
  if (kind == "llama-server") {
    if (!lock.has_value()) {
      *error =
          "the model is not locked for this project; run `seqc model "
          "pull` first";
      return nullptr;
    }
    return CreateLlamaServerAdapter(config, *lock, log_dir, error);
  }
  *error = "unknown model adapter '" + kind +
           "' (setting model.adapter); use llama-server";
  return nullptr;
}

}  // namespace seq
