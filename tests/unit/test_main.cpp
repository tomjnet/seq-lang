#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "test_harness.hpp"

namespace seqtest {

namespace {

struct Entry {
  const char* name;
  TestFunction function;
};

std::vector<Entry>& Tests() {
  static std::vector<Entry> tests;
  return tests;
}

int g_failures_in_current_test = 0;

}  // namespace

Registrar::Registrar(const char* name, TestFunction function) {
  Tests().push_back(Entry{name, function});
}

void ReportFailure(const char* file, int line, const std::string& message) {
  ++g_failures_in_current_test;
  std::printf("  %s:%d: %s\n", file, line, message.c_str());
}

}  // namespace seqtest

// Usage: seq_unit_tests [name-prefix]
int main(int argc, char* argv[]) {
  const char* prefix = argc > 1 ? argv[1] : "";
  int run = 0;
  int failed = 0;
  for (const auto& test : seqtest::Tests()) {
    if (std::strncmp(test.name, prefix, std::strlen(prefix)) != 0) continue;
    ++run;
    seqtest::g_failures_in_current_test = 0;
    test.function();
    if (seqtest::g_failures_in_current_test != 0) {
      ++failed;
      std::printf("FAILED %s\n", test.name);
    }
  }
  std::printf("%d test(s) run, %d failed\n", run, failed);
  if (run == 0) {
    std::printf("no test matches '%s'\n", prefix);
    return 1;
  }
  return failed == 0 ? 0 : 1;
}
