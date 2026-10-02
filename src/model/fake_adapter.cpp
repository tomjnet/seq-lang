// Scripted model adapter for deterministic tests.
//
// The script is a JSON file with one array of responses per request family:
//
//   {
//     "plan":     [ {...} ],
//     "generate": [ {"step_functions": "..."}, {"step_functions": "..."} ],
//     "test":     [ {"test_cases": "..."} ]
//   }
//
// "generate" also serves repair requests and "test" also serves test-repair
// requests, in order. An object entry is returned as its JSON text, a string
// entry is returned verbatim (to script malformed responses), and
// {"$error": "message"} makes the call fail like a provider error.

#include <fstream>
#include <map>

#include "model/adapters.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"

namespace seq {

namespace {

class FakeAdapter : public ModelAdapter {
 public:
  FakeAdapter(Json script, std::string script_hash, std::string log_path,
              std::optional<ModelLock> lock)
      : script_(std::move(script)),
        script_hash_(std::move(script_hash)),
        log_path_(std::move(log_path)),
        lock_(std::move(lock)) {}

  std::string Name() const override { return "fake"; }

  Json Identity() const override {
    Json identity = Json::MakeObject();
    identity.Set("adapter", "fake");
    identity.Set("script_sha256", script_hash_);
    if (lock_.has_value()) identity.Set("lock", lock_->ToJson());
    return identity;
  }

  bool Start(std::string*) override { return true; }

  bool CountTokens(const std::string& text, int* tokens,
                   std::string*) override {
    *tokens = static_cast<int>((text.size() + 3) / 4);
    return true;
  }

  InferenceResult Complete(const InferenceRequest& request,
                           const GenerationSettings&) override {
    if (!log_path_.empty()) {
      std::ofstream log(log_path_, std::ios::app);
      log << request.kind << "\n";
    }
    std::string family = request.kind;
    if (family == "repair") family = "generate";
    if (family == "test-repair") family = "test";

    InferenceResult result;
    const Json* responses = script_.Find(family);
    const std::size_t index = next_[family]++;
    if (responses == nullptr || !responses->is_array() ||
        index >= responses->AsArray().size()) {
      result.error = "fake adapter has no scripted response number " +
                     std::to_string(index + 1) + " for '" + family + "'";
      return result;
    }
    const Json& entry = responses->AsArray()[index];
    if (entry.is_string()) {
      result.ok = true;
      result.content = entry.AsString();
      return result;
    }
    if (const Json* failure = entry.Find("$error"); failure != nullptr) {
      result.error = failure->is_string() ? failure->AsString()
                                          : "scripted provider error";
      return result;
    }
    result.ok = true;
    result.content = entry.Dump(-1);
    return result;
  }

 private:
  Json script_;
  std::string script_hash_;
  std::string log_path_;
  std::optional<ModelLock> lock_;
  std::map<std::string, std::size_t> next_;
};

}  // namespace

std::unique_ptr<ModelAdapter> CreateFakeAdapter(
    const Config& config, const std::optional<ModelLock>& lock,
    std::string* error) {
  const std::string script_path = config.Get("model.fake_script");
  if (script_path.empty()) {
    *error = "the fake adapter needs a script (setting model.fake_script)";
    return nullptr;
  }
  std::string text;
  if (!ReadFile(script_path, &text, error)) return nullptr;
  Json script;
  std::string parse_error;
  if (!Json::Parse(text, &script, &parse_error) || !script.is_object()) {
    *error = script_path + ": " +
             (parse_error.empty() ? "expected a JSON object" : parse_error);
    return nullptr;
  }
  return std::make_unique<FakeAdapter>(std::move(script), Sha256Hex(text),
                                       config.Get("model.fake_log"), lock);
}

}  // namespace seq
