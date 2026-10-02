// A minimal test harness for seqc's own deterministic tests. It has no
// dependencies, so the suite builds wherever seqc builds.

#ifndef SEQ_TESTS_UNIT_TEST_HARNESS_HPP_
#define SEQ_TESTS_UNIT_TEST_HARNESS_HPP_

#include <sstream>
#include <string>

namespace seqtest {

using TestFunction = void (*)();

struct Registrar {
  Registrar(const char* name, TestFunction function);
};

void ReportFailure(const char* file, int line, const std::string& message);

template <typename A, typename B>
void CheckEqual(const char* file, int line, const char* a_text,
                const char* b_text, const A& a, const B& b) {
  if (a == b) return;
  std::ostringstream message;
  message << "expected " << a_text << " == " << b_text << "\n    left:  " << a
          << "\n    right: " << b;
  ReportFailure(file, line, message.str());
}

}  // namespace seqtest

#define SEQ_TEST(name)                                              \
  static void name();                                               \
  static const ::seqtest::Registrar registrar_##name(#name, &name); \
  static void name()

#define CHECK(condition)                                                     \
  do {                                                                       \
    if (!(condition)) {                                                      \
      ::seqtest::ReportFailure(__FILE__, __LINE__, "expected: " #condition); \
    }                                                                        \
  } while (false)

#define CHECK_EQ(a, b) \
  ::seqtest::CheckEqual(__FILE__, __LINE__, #a, #b, (a), (b))

#endif  // SEQ_TESTS_UNIT_TEST_HARNESS_HPP_
