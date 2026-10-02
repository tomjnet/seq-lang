// Placeholder for platforms other than Linux. Seq targets Linux x86_64 only;
// on other hosts seqc can still scaffold, check, and clean projects.

#include "support/process.hpp"

namespace seq {

namespace {

constexpr char kUnsupported[] =
    "this operation is only supported on Linux x86_64";

}  // namespace

ProcessResult RunProcess(const ProcessSpec&) {
  ProcessResult result;
  result.launch_error = kUnsupported;
  return result;
}

SandboxSupport DetectSandbox() {
  SandboxSupport support;
  support.detail = "the sandbox is only implemented for Linux x86_64";
  return support;
}

std::optional<std::filesystem::path> FindProgram(std::string_view) {
  return std::nullopt;
}

void RequestCancellation() {}
bool CancellationRequested() { return false; }
void InstallCancellationHandlers() {}

BackgroundProcess::~BackgroundProcess() = default;

bool BackgroundProcess::Start(const std::vector<std::string>&,
                              const std::filesystem::path&,
                              std::string* error) {
  *error = kUnsupported;
  return false;
}

bool BackgroundProcess::Running() { return false; }
void BackgroundProcess::Stop() {}

FileLock::~FileLock() = default;

bool FileLock::TryAcquire(const std::filesystem::path&, bool* busy,
                          std::string*) {
  *busy = false;
  return true;
}

HttpResponse HttpRequest(const std::string&, int, const std::string&,
                         const std::string&, const std::string&, int) {
  HttpResponse response;
  response.error = kUnsupported;
  return response;
}

int FindFreeLoopbackPort() { return 0; }

}  // namespace seq
