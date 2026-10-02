#include "support/config.hpp"

#include <cerrno>
#include <cstdlib>
#include <system_error>

#include "support/util.hpp"

namespace seq {

namespace fs = std::filesystem;

namespace {

std::string EnvOr(const char* name, std::string fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  return value;
}

fs::path HomeDir() {
#if defined(_WIN32)
  return fs::path(EnvOr("USERPROFILE", "."));
#else
  return fs::path(EnvOr("HOME", "."));
#endif
}

bool IsKnownKey(std::string_view key) {
  for (const Setting& setting : AllSettings()) {
    if (key == setting.key) return true;
  }
  return false;
}

std::string EnvNameFor(std::string_view key) {
  std::string name = "SEQC_";
  for (const char c : key) {
    name.push_back(c == '.' ? '_' : c);
  }
  return ToUpper(name);
}

}  // namespace

const std::vector<Setting>& AllSettings() {
  static const std::vector<Setting> settings = {
      {"paths.home", "",
       "Directory holding the seqc runtime and templates. Default: "
       "<seqc>/../lib/seqc."},
      {"model.adapter", "llama-server",
       "Inference adapter: llama-server or fake."},
      {"model.llama_server", "llama-server",
       "llama.cpp server program started for each build."},
      {"model.endpoint", "",
       "Use an already running llama.cpp server on a loopback address, for "
       "example http://127.0.0.1:8080."},
      {"model.startup_seconds", "180",
       "Time allowed for the inference server to load the model."},
      {"model.timeout_seconds", "900", "Time allowed for one inference call."},
      {"model.max_output_tokens", "4096",
       "Output budget reserved for one inference call."},
      {"model.context_tokens", "32768",
       "Model window for prompt and output combined."},
      {"model.seed", "1", "Sampling seed, recorded in the build manifest."},
      {"model.max_response_kib", "512", "Largest accepted inference response."},
      {"model.hub_url", "https://huggingface.co",
       "Base URL used by `seqc model pull`."},
      {"model.gguf_repo", "",
       "User mapping: repository holding the GGUF file for the declared "
       "model."},
      {"model.gguf_file", "", "User mapping: GGUF file name."},
      {"model.gguf_revision", "", "User mapping: pinned revision."},
      {"model.gguf_sha256", "", "User mapping: SHA-256 of the GGUF file."},
      {"model.gguf_quantization", "", "User mapping: quantization label."},
      {"model.fake_script", "",
       "Scripted responses for the fake adapter (tests)."},
      {"model.fake_log", "",
       "File that the fake adapter appends one line to per call (tests)."},
      {"toolchain.cc", "gcc", "C compiler."},
      {"toolchain.cxx", "g++", "C++ compiler used for generated tests."},
      {"toolchain.gtest_root", "",
       "Google Test prefix with include/ and lib/, when it is not installed "
       "system-wide."},
      {"build.repair_attempts", "2",
       "Repair attempts after the initial generation."},
      {"build.compile_timeout_seconds", "180",
       "Wall-clock limit for one compiler invocation."},
      {"build.compile_memory_mb", "4096",
       "Address-space limit for compiler processes."},
      {"limits.cpu_seconds", "30", "CPU time for a generated program."},
      {"limits.wall_seconds", "60", "Wall-clock time for a generated program."},
      {"limits.memory_mb", "1024",
       "Address-space limit for a generated program."},
      {"limits.file_mb", "64",
       "Largest single file a generated program may write."},
      {"limits.output_mb", "8",
       "Combined stdout and stderr allowed from a generated program."},
      {"limits.staging_mb", "256",
       "Total size of the files a generated program may leave in staging."},
      {"inputs.max_file_mb", "16", "Largest accepted input file."},
      {"inputs.max_total_mb", "64", "Total size of accepted input files."},
      {"inputs.max_files", "256", "Largest number of input files."},
      {"inputs.preview_bytes", "2048",
       "Bytes of each text input shown to the model."},
  };
  return settings;
}

bool ParseConfigToml(std::string_view text,
                     std::vector<std::pair<std::string, std::string>>* out,
                     std::string* error) {
  std::string section;
  std::size_t line_number = 0;
  for (const std::string& raw : SplitLines(text)) {
    ++line_number;
    const auto fail = [&](const std::string& message) {
      *error = "line " + std::to_string(line_number) + ": " + message;
      return false;
    };
    std::string_view line = TrimWhitespace(raw);
    if (line.empty() || line.front() == '#') continue;
    if (line.front() == '[') {
      if (line.back() != ']') return fail("unterminated section header");
      section = std::string(TrimWhitespace(line.substr(1, line.size() - 2)));
      if (section.empty()) return fail("empty section name");
      continue;
    }
    const std::size_t equals = line.find('=');
    if (equals == std::string_view::npos) return fail("expected key = value");
    const std::string key(TrimWhitespace(line.substr(0, equals)));
    std::string_view value = TrimWhitespace(line.substr(equals + 1));
    if (key.empty()) return fail("missing key");
    if (value.empty()) return fail("missing value");

    std::string decoded;
    if (value.front() == '"') {
      std::size_t i = 1;
      bool closed = false;
      for (; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '"') {
          closed = true;
          ++i;
          break;
        }
        if (c == '\\') {
          if (i + 1 >= value.size()) return fail("unterminated escape");
          const char e = value[++i];
          if (e == '"' || e == '\\') {
            decoded.push_back(e);
          } else if (e == 'n') {
            decoded.push_back('\n');
          } else if (e == 't') {
            decoded.push_back('\t');
          } else {
            return fail("unsupported escape in string");
          }
          continue;
        }
        decoded.push_back(c);
      }
      if (!closed) return fail("unterminated string");
      const std::string_view rest = TrimWhitespace(value.substr(i));
      if (!rest.empty() && rest.front() != '#') {
        return fail("unexpected text after string");
      }
    } else {
      const std::size_t comment = value.find('#');
      if (comment != std::string_view::npos) {
        value = TrimWhitespace(value.substr(0, comment));
      }
      if (value != "true" && value != "false") {
        std::size_t i = (value.front() == '-' || value.front() == '+') ? 1 : 0;
        if (i == value.size()) return fail("invalid value");
        for (; i < value.size(); ++i) {
          if (value[i] < '0' || value[i] > '9') {
            return fail(
                "value must be a quoted string, an integer, or "
                "true/false");
          }
        }
      }
      decoded = std::string(value);
    }
    out->emplace_back(section.empty() ? key : section + "." + key,
                      std::move(decoded));
  }
  return true;
}

Config Config::Defaults() {
  Config config;
  for (const Setting& setting : AllSettings()) {
    config.values_[setting.key] = setting.default_value;
  }
  return config;
}

bool Config::Load(const std::vector<std::string>& overrides, Config* out,
                  std::string* error) {
  *out = Defaults();

  const fs::path file = ConfigFilePath();
  std::error_code ec;
  if (fs::exists(file, ec)) {
    std::string text;
    if (!ReadFile(file, &text, error, 1024 * 1024)) return false;
    std::vector<std::pair<std::string, std::string>> entries;
    std::string parse_error;
    if (!ParseConfigToml(text, &entries, &parse_error)) {
      *error = file.string() + ": " + parse_error;
      return false;
    }
    for (auto& [key, value] : entries) {
      if (!IsKnownKey(key)) {
        *error = file.string() + ": unknown setting '" + key + "'";
        return false;
      }
      out->values_[key] = std::move(value);
    }
  }

  for (const Setting& setting : AllSettings()) {
    const char* value = std::getenv(EnvNameFor(setting.key).c_str());
    if (value != nullptr) out->values_[setting.key] = value;
  }
  // SEQC_HOME is the short, documented spelling of SEQC_PATHS_HOME.
  if (const char* home = std::getenv("SEQC_HOME"); home != nullptr) {
    out->values_["paths.home"] = home;
  }

  for (const std::string& item : overrides) {
    const std::size_t equals = item.find('=');
    if (equals == std::string::npos || equals == 0) {
      *error = "--set expects key=value, got '" + item + "'";
      return false;
    }
    const std::string key = item.substr(0, equals);
    if (!IsKnownKey(key)) {
      *error = "unknown setting '" + key + "'";
      return false;
    }
    out->values_[key] = item.substr(equals + 1);
  }

  // Numeric settings must be nonnegative integers.
  for (const Setting& setting : AllSettings()) {
    const std::string_view fallback = setting.default_value;
    if (fallback.empty() ||
        fallback.find_first_not_of("0123456789") != std::string_view::npos) {
      continue;
    }
    const std::string& value = out->values_[setting.key];
    if (value.empty() ||
        value.find_first_not_of("0123456789") != std::string::npos ||
        value.size() > 12) {
      *error = std::string("setting '") + setting.key +
               "' must be a nonnegative integer, got '" + value + "'";
      return false;
    }
  }
  return true;
}

std::string Config::Get(std::string_view key) const {
  const auto it = values_.find(key);
  return it == values_.end() ? std::string() : it->second;
}

std::int64_t Config::GetInt(std::string_view key) const {
  const std::string value = Get(key);
  errno = 0;
  const long long parsed = std::strtoll(value.c_str(), nullptr, 10);
  return errno == 0 ? parsed : 0;
}

void Config::Set(std::string_view key, std::string value) {
  values_[std::string(key)] = std::move(value);
}

fs::path Config::ConfigFilePath() {
  const std::string base = EnvOr("XDG_CONFIG_HOME", "");
  const fs::path root = base.empty() ? HomeDir() / ".config" : fs::path(base);
  return root / "seqc" / "config.toml";
}

fs::path Config::CacheDir() {
  const std::string base = EnvOr("XDG_CACHE_HOME", "");
  const fs::path root = base.empty() ? HomeDir() / ".cache" : fs::path(base);
  return root / "seqc";
}

}  // namespace seq
