#ifndef SEQ_MODEL_ADAPTERS_HPP_
#define SEQ_MODEL_ADAPTERS_HPP_

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "seq/model.hpp"

namespace seq {

// Deterministic adapter that replays scripted responses. Used by the test
// suite so that ordinary CI does not depend on a model or on sampling.
std::unique_ptr<ModelAdapter> CreateFakeAdapter(
    const Config& config, const std::optional<ModelLock>& lock,
    std::string* error);

// Adapter for llama.cpp's `llama-server`, reached over loopback HTTP. By
// default seqc starts the server itself with the locked weights.
std::unique_ptr<ModelAdapter> CreateLlamaServerAdapter(
    const Config& config, const ModelLock& lock,
    const std::filesystem::path& log_dir, std::string* error);

}  // namespace seq

#endif  // SEQ_MODEL_ADAPTERS_HPP_
