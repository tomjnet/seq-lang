// Adapter for llama.cpp's llama-server.
//
// The server is reached over plain HTTP on a loopback address. By default
// seqc starts it for the duration of one build with the weights pinned in
// seq.lock; `model.endpoint` selects an already running loopback server
// instead. v0.1 has no remote adapter, so nothing leaves the machine.

#include <chrono>
#include <thread>

#include "model/adapters.hpp"
#include "support/process.hpp"
#include "support/sha256.hpp"
#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

bool IsLoopbackHost(const std::string& host) {
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

// Parses http://host:port with nothing after the port.
bool ParseEndpoint(const std::string& endpoint, std::string* host, int* port) {
  constexpr std::string_view kScheme = "http://";
  if (!StartsWith(endpoint, kScheme)) return false;
  std::string rest = endpoint.substr(kScheme.size());
  while (!rest.empty() && rest.back() == '/') rest.pop_back();
  const std::size_t colon = rest.rfind(':');
  if (colon == std::string::npos || colon == 0) return false;
  *host = rest.substr(0, colon);
  const std::string port_text = rest.substr(colon + 1);
  if (port_text.empty() || port_text.size() > 5 ||
      port_text.find_first_not_of("0123456789") != std::string::npos) {
    return false;
  }
  *port = std::stoi(port_text);
  return *port > 0 && *port < 65536;
}

class LlamaServerAdapter : public ModelAdapter {
 public:
  LlamaServerAdapter(const Config& config, ModelLock lock, fs::path log_dir)
      : lock_(std::move(lock)),
        log_dir_(std::move(log_dir)),
        program_(config.Get("model.llama_server")),
        endpoint_(config.Get("model.endpoint")),
        startup_seconds_(
            static_cast<int>(config.GetInt("model.startup_seconds"))),
        context_tokens_(
            static_cast<int>(config.GetInt("model.context_tokens"))) {}

  std::string Name() const override { return "llama-server"; }

  Json Identity() const override {
    return Json::MakeObject()
        .Set("adapter", "llama-server")
        .Set("lock", lock_.ToJson());
  }

  bool Start(std::string* error) override {
    if (!endpoint_.empty()) return UseRunningServer(error);
    return StartOwnServer(error);
  }

  bool CountTokens(const std::string& text, int* tokens,
                   std::string* error) override {
    const std::string body = Json::MakeObject().Set("content", text).Dump(-1);
    const HttpResponse response =
        HttpRequest(host_, port_, "POST", "/tokenize", body, 120);
    Json json;
    std::string parse_error;
    if (!response.ok || response.status != 200 ||
        !Json::Parse(response.body, &json, &parse_error)) {
      *error = "cannot count tokens: " +
               (response.ok ? "HTTP " + std::to_string(response.status)
                            : response.error);
      return false;
    }
    const Json* list = json.Find("tokens");
    if (list == nullptr || !list->is_array()) {
      *error = "cannot count tokens: unexpected /tokenize response";
      return false;
    }
    *tokens = static_cast<int>(list->AsArray().size());
    return true;
  }

  InferenceResult Complete(const InferenceRequest& request,
                           const GenerationSettings& settings) override {
    InferenceResult result;
    Json messages = Json::MakeArray();
    messages.Push(Json::MakeObject()
                      .Set("role", "system")
                      .Set("content", request.system));
    messages.Push(
        Json::MakeObject().Set("role", "user").Set("content", request.user));

    // Constrained decoding: the server enforces the schema while sampling.
    Json format = Json::MakeObject();
    format.Set("type", "json_schema");
    format.Set("json_schema", Json::MakeObject()
                                  .Set("name", "seq_" + request.kind)
                                  .Set("strict", true)
                                  .Set("schema", request.schema));

    Json body = Json::MakeObject();
    body.Set("messages", std::move(messages));
    body.Set("temperature", settings.temperature);
    body.Set("seed", settings.seed);
    body.Set("max_tokens", settings.max_output_tokens);
    body.Set("stream", false);
    body.Set("cache_prompt", false);
    body.Set("response_format", std::move(format));

    const auto start = std::chrono::steady_clock::now();
    const HttpResponse response =
        HttpRequest(host_, port_, "POST", "/v1/chat/completions", body.Dump(-1),
                    settings.timeout_seconds);
    result.seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    if (!response.ok) {
      result.error = "inference request failed: " + response.error;
      return result;
    }
    if (response.status != 200) {
      result.error = "inference server answered HTTP " +
                     std::to_string(response.status) + ": " +
                     response.body.substr(0, 400);
      return result;
    }
    Json json;
    std::string parse_error;
    if (!Json::Parse(response.body, &json, &parse_error)) {
      result.error = "inference server sent malformed JSON: " + parse_error;
      return result;
    }
    const Json* choices = json.Find("choices");
    if (choices == nullptr || !choices->is_array() ||
        choices->AsArray().empty()) {
      result.error = "inference response has no choices";
      return result;
    }
    const Json& choice = choices->AsArray().front();
    if (choice.GetString("finish_reason") == "length") {
      result.error = "the response was cut off at the output budget of " +
                     std::to_string(settings.max_output_tokens) +
                     " tokens (setting model.max_output_tokens)";
      return result;
    }
    const Json* message = choice.Find("message");
    if (message == nullptr || message->Find("content") == nullptr ||
        !message->Find("content")->is_string()) {
      result.error = "inference response has no message content";
      return result;
    }
    result.ok = true;
    result.content = message->Find("content")->AsString();
    return result;
  }

 private:
  bool UseRunningServer(std::string* error) {
    if (!ParseEndpoint(endpoint_, &host_, &port_)) {
      *error = "model.endpoint must look like http://127.0.0.1:8080, got '" +
               endpoint_ + "'";
      return false;
    }
    if (!IsLoopbackHost(host_)) {
      *error =
          "model.endpoint must be a loopback address in v0.1; remote "
          "inference servers are not supported";
      return false;
    }
    const HttpResponse health =
        HttpRequest(host_, port_, "GET", "/health", "", 10);
    if (!health.ok || health.status != 200) {
      *error =
          "inference server at " + endpoint_ + " is not ready: " +
          (health.ok ? "HTTP " + std::to_string(health.status) : health.error);
      return false;
    }
    // The server must be serving the locked weights. There is no fallback to
    // whatever model happens to be loaded.
    const HttpResponse props =
        HttpRequest(host_, port_, "GET", "/props", "", 10);
    Json json;
    std::string parse_error;
    std::string served;
    if (props.ok && props.status == 200 &&
        Json::Parse(props.body, &json, &parse_error)) {
      served = fs::path(json.GetString("model_path")).filename().string();
    }
    if (served != lock_.file) {
      *error = "inference server at " + endpoint_ + " serves '" +
               (served.empty() ? "an unknown model" : served) +
               "', not the locked " + lock_.file;
      return false;
    }
    return true;
  }

  bool StartOwnServer(std::string* error) {
    const fs::path weights = ModelWeightsPath(lock_);
    std::error_code ec;
    if (!fs::is_regular_file(weights, ec)) {
      *error = "model weights are not downloaded (" + weights.string() +
               "); run `seqc model pull`";
      return false;
    }
    std::string hash;
    if (!Sha256File(weights, &hash, error)) return false;
    if (hash != lock_.sha256) {
      *error = "model weights do not match seq.lock (" + weights.string() +
               " has SHA-256 " + hash + ", expected " + lock_.sha256 +
               "); run `seqc model pull` to download them again";
      return false;
    }
    const std::optional<fs::path> program = FindProgram(program_);
    if (!program.has_value()) {
      *error = "inference runtime '" + program_ +
               "' was not found; install llama.cpp or set model.llama_server";
      return false;
    }
    host_ = "127.0.0.1";
    port_ = FindFreeLoopbackPort();
    if (port_ == 0) {
      *error = "cannot find a free loopback port for the inference server";
      return false;
    }
    fs::create_directories(log_dir_, ec);
    const fs::path log = log_dir_ / "llama-server.log";
    const std::vector<std::string> argv = {
        program->string(),
        "--model",
        weights.string(),
        "--host",
        host_,
        "--port",
        std::to_string(port_),
        "--ctx-size",
        std::to_string(context_tokens_),
        "--parallel",
        "1",
    };
    if (!server_.Start(argv, log, error)) return false;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(startup_seconds_);
    while (std::chrono::steady_clock::now() < deadline) {
      if (CancellationRequested()) {
        *error = "cancelled";
        return false;
      }
      if (!server_.Running()) {
        *error =
            "the inference server exited during startup; see " + log.string();
        return false;
      }
      const HttpResponse health =
          HttpRequest(host_, port_, "GET", "/health", "", 5);
      if (health.ok && health.status == 200) return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    *error = "the inference server did not become ready within " +
             std::to_string(startup_seconds_) +
             " seconds (setting model.startup_seconds); see " + log.string();
    return false;
  }

  ModelLock lock_;
  fs::path log_dir_;
  std::string program_;
  std::string endpoint_;
  int startup_seconds_;
  int context_tokens_;
  std::string host_;
  int port_ = 0;
  BackgroundProcess server_;
};

}  // namespace

std::unique_ptr<ModelAdapter> CreateLlamaServerAdapter(const Config& config,
                                                       const ModelLock& lock,
                                                       const fs::path& log_dir,
                                                       std::string*) {
  return std::make_unique<LlamaServerAdapter>(config, lock, log_dir);
}

}  // namespace seq
