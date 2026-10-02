// minigtest runner. See ../include/gtest/gtest.h.

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace testing {

namespace {

struct Registration {
  std::string suite;
  std::string name;
  internal::Factory factory;
};

std::vector<Registration>& Registry() {
  static std::vector<Registration> registry;
  return registry;
}

bool g_current_failed = false;

}  // namespace

namespace internal {

int Register(const char* suite, const char* name, Factory factory) {
  Registry().push_back(Registration{suite, name, factory});
  return 0;
}

void AssertHelper::operator=(const Message& message) const {
  g_current_failed = true;
  std::printf("%s:%d: Failure\n%s\n", file_, line_, summary_.c_str());
  const std::string extra = message.str();
  if (!extra.empty()) std::printf("%s\n", extra.c_str());
  std::fflush(stdout);
}

}  // namespace internal

void InitGoogleTest(int*, char**) {}
void InitGoogleTest() {}

int RunAllTests() {
  std::set<std::string> suites;
  for (const Registration& test : Registry()) suites.insert(test.suite);
  std::printf("[==========] Running %zu tests from %zu test suites.\n",
              Registry().size(), suites.size());

  std::vector<std::string> failed;
  for (const Registration& test : Registry()) {
    const std::string full_name = test.suite + "." + test.name;
    std::printf("[ RUN      ] %s\n", full_name.c_str());
    std::fflush(stdout);
    g_current_failed = false;
    Test* instance = test.factory();
    instance->SetUp();
    instance->TestBody();
    instance->TearDown();
    delete instance;
    if (g_current_failed) {
      failed.push_back(full_name);
      std::printf("[  FAILED  ] %s (0 ms)\n", full_name.c_str());
    } else {
      std::printf("[       OK ] %s (0 ms)\n", full_name.c_str());
    }
    std::fflush(stdout);
  }

  std::printf("[==========] %zu tests from %zu test suites ran. (0 ms total)\n",
              Registry().size(), suites.size());
  std::printf("[  PASSED  ] %zu tests.\n", Registry().size() - failed.size());
  if (!failed.empty()) {
    std::printf("[  FAILED  ] %zu tests, listed below:\n", failed.size());
    for (const std::string& name : failed) {
      std::printf("[  FAILED  ] %s\n", name.c_str());
    }
  }
  std::fflush(stdout);
  return failed.empty() ? 0 : 1;
}

}  // namespace testing
