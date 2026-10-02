// minigtest: a small stand-in for Google Test, used only by this repository's
// own test suite.
//
// seqc builds generated tests against an installed Google Test. So that the
// deterministic test suite can exercise that part of the pipeline on machines
// and CI runners without the Google Test package, this header and the two
// libraries built from ../../src provide the subset of the Google Test API
// that the fixtures use, with the same console output format. It is selected
// through the `toolchain.gtest_root` setting. It is not installed and is not
// a substitute for Google Test in real projects.

#ifndef MINIGTEST_GTEST_GTEST_H_
#define MINIGTEST_GTEST_GTEST_H_

#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>

namespace testing {

class Message {
 public:
  Message() = default;
  Message(const Message& other) { stream_ << other.str(); }

  template <typename T>
  Message& operator<<(const T& value) {
    stream_ << value;
    return *this;
  }

  std::string str() const { return stream_.str(); }

 private:
  std::ostringstream stream_;
};

class Test {
 public:
  virtual ~Test() = default;
  virtual void SetUp() {}
  virtual void TearDown() {}
  virtual void TestBody() = 0;
};

void InitGoogleTest(int* argc, char** argv);
void InitGoogleTest();
int RunAllTests();

namespace internal {

using Factory = Test* (*)();

int Register(const char* suite, const char* name, Factory factory);

class AssertHelper {
 public:
  AssertHelper(const char* file, int line, std::string summary)
      : file_(file), line_(line), summary_(std::move(summary)) {}
  // Reports the failure. Returning void lets ASSERT_* write
  // `return AssertHelper(...) = Message() << ...;` in a void function.
  void operator=(const Message& message) const;

 private:
  const char* file_;
  int line_;
  std::string summary_;
};

template <typename T, typename = void>
struct IsStreamable : std::false_type {};
template <typename T>
struct IsStreamable<
    T, std::void_t<decltype(std::declval<std::ostream&>()
                            << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string Print(const T& value) {
  if constexpr (std::is_same_v<T, bool>) {
    return value ? "true" : "false";
  } else if constexpr (IsStreamable<T>::value) {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  } else {
    return "<object>";
  }
}

template <typename A, typename B>
std::string Describe(const char* op, const char* a_text, const char* b_text,
                     const A& a, const B& b) {
  return std::string("Expected: (") + a_text + ") " + op + " (" + b_text +
         "), actual: " + Print(a) + " vs " + Print(b);
}

#define MINIGTEST_DEFINE_COMPARE_(name, op)                                 \
  template <typename A, typename B>                                         \
  std::string Compare##name(const char* a_text, const char* b_text,         \
                            const A& a, const B& b) {                       \
    if (a op b) return std::string();                                       \
    return Describe(#op, a_text, b_text, a, b);                             \
  }
MINIGTEST_DEFINE_COMPARE_(EQ, ==)
MINIGTEST_DEFINE_COMPARE_(NE, !=)
MINIGTEST_DEFINE_COMPARE_(LT, <)
MINIGTEST_DEFINE_COMPARE_(LE, <=)
MINIGTEST_DEFINE_COMPARE_(GT, >)
MINIGTEST_DEFINE_COMPARE_(GE, >=)
#undef MINIGTEST_DEFINE_COMPARE_

inline std::string CompareSTREQ(const char* a_text, const char* b_text,
                                const char* a, const char* b) {
  if (a == nullptr || b == nullptr) {
    if (a == b) return std::string();
  } else if (std::strcmp(a, b) == 0) {
    return std::string();
  }
  return Describe("==", a_text, b_text, std::string(a ? a : "(null)"),
                  std::string(b ? b : "(null)"));
}

inline std::string CompareSTRNE(const char* a_text, const char* b_text,
                                const char* a, const char* b) {
  const bool equal = (a == nullptr || b == nullptr)
                         ? a == b
                         : std::strcmp(a, b) == 0;
  if (!equal) return std::string();
  return Describe("!=", a_text, b_text, std::string(a ? a : "(null)"),
                  std::string(b ? b : "(null)"));
}

inline std::string CompareNEAR(const char* a_text, const char* b_text,
                               double a, double b, double tolerance) {
  if (std::fabs(a - b) <= tolerance) return std::string();
  return Describe("is near", a_text, b_text, a, b);
}

inline std::string CompareDOUBLE(const char* a_text, const char* b_text,
                                 double a, double b) {
  const double scale = std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
  if (std::fabs(a - b) <= 1e-12 * scale) return std::string();
  return Describe("==", a_text, b_text, a, b);
}

inline std::string CompareFLOAT(const char* a_text, const char* b_text, float a,
                                float b) {
  const float scale = std::fmax(1.0f, std::fmax(std::fabs(a), std::fabs(b)));
  if (std::fabs(a - b) <= 1e-5f * scale) return std::string();
  return Describe("==", a_text, b_text, a, b);
}

inline std::string CheckBool(const char* text, bool actual, bool expected) {
  if (actual == expected) return std::string();
  return std::string("Value of: ") + text +
         "\n  Actual: " + (actual ? "true" : "false") +
         "\nExpected: " + (expected ? "true" : "false");
}

}  // namespace internal
}  // namespace testing

#define RUN_ALL_TESTS() ::testing::RunAllTests()

#define MINIGTEST_CLASS_(suite, name) suite##_##name##_Test

#define MINIGTEST_TEST_(suite, name, base)                                  \
  class MINIGTEST_CLASS_(suite, name) : public base {                       \
   public:                                                                  \
    void TestBody() override;                                               \
    static int registered_;                                                 \
  };                                                                        \
  int MINIGTEST_CLASS_(suite, name)::registered_ =                          \
      ::testing::internal::Register(#suite, #name, []() -> ::testing::Test* { \
        return new MINIGTEST_CLASS_(suite, name);                           \
      });                                                                   \
  void MINIGTEST_CLASS_(suite, name)::TestBody()

#define TEST(suite, name) MINIGTEST_TEST_(suite, name, ::testing::Test)
#define TEST_F(fixture, name) MINIGTEST_TEST_(fixture, name, fixture)

// `switch (0) case 0: default:` keeps the macros safe inside an unbraced if.
#define MINIGTEST_CHECK_(expression, on_failure)                            \
  switch (0)                                                                \
  case 0:                                                                   \
  default:                                                                  \
    if (const std::string minigtest_summary = (expression);                 \
        minigtest_summary.empty())                                          \
      ;                                                                     \
    else                                                                    \
      on_failure ::testing::internal::AssertHelper(__FILE__, __LINE__,      \
                                                   minigtest_summary) =     \
          ::testing::Message()

#define MINIGTEST_COMPARE_(name, a, b, on_failure)                          \
  MINIGTEST_CHECK_(::testing::internal::Compare##name(#a, #b, (a), (b)),    \
                   on_failure)

#define EXPECT_TRUE(c) \
  MINIGTEST_CHECK_(::testing::internal::CheckBool(#c, static_cast<bool>(c), true), )
#define EXPECT_FALSE(c) \
  MINIGTEST_CHECK_(::testing::internal::CheckBool(#c, static_cast<bool>(c), false), )
#define ASSERT_TRUE(c) \
  MINIGTEST_CHECK_(::testing::internal::CheckBool(#c, static_cast<bool>(c), true), return)
#define ASSERT_FALSE(c) \
  MINIGTEST_CHECK_(::testing::internal::CheckBool(#c, static_cast<bool>(c), false), return)

#define EXPECT_EQ(a, b) MINIGTEST_COMPARE_(EQ, a, b, )
#define EXPECT_NE(a, b) MINIGTEST_COMPARE_(NE, a, b, )
#define EXPECT_LT(a, b) MINIGTEST_COMPARE_(LT, a, b, )
#define EXPECT_LE(a, b) MINIGTEST_COMPARE_(LE, a, b, )
#define EXPECT_GT(a, b) MINIGTEST_COMPARE_(GT, a, b, )
#define EXPECT_GE(a, b) MINIGTEST_COMPARE_(GE, a, b, )
#define EXPECT_STREQ(a, b) MINIGTEST_COMPARE_(STREQ, a, b, )
#define EXPECT_STRNE(a, b) MINIGTEST_COMPARE_(STRNE, a, b, )
#define EXPECT_DOUBLE_EQ(a, b) MINIGTEST_COMPARE_(DOUBLE, a, b, )
#define EXPECT_FLOAT_EQ(a, b) MINIGTEST_COMPARE_(FLOAT, a, b, )
#define EXPECT_NEAR(a, b, tolerance)                                        \
  MINIGTEST_CHECK_(                                                         \
      ::testing::internal::CompareNEAR(#a, #b, (a), (b), (tolerance)), )

#define ASSERT_EQ(a, b) MINIGTEST_COMPARE_(EQ, a, b, return)
#define ASSERT_NE(a, b) MINIGTEST_COMPARE_(NE, a, b, return)
#define ASSERT_LT(a, b) MINIGTEST_COMPARE_(LT, a, b, return)
#define ASSERT_LE(a, b) MINIGTEST_COMPARE_(LE, a, b, return)
#define ASSERT_GT(a, b) MINIGTEST_COMPARE_(GT, a, b, return)
#define ASSERT_GE(a, b) MINIGTEST_COMPARE_(GE, a, b, return)
#define ASSERT_STREQ(a, b) MINIGTEST_COMPARE_(STREQ, a, b, return)
#define ASSERT_STRNE(a, b) MINIGTEST_COMPARE_(STRNE, a, b, return)
#define ASSERT_DOUBLE_EQ(a, b) MINIGTEST_COMPARE_(DOUBLE, a, b, return)
#define ASSERT_FLOAT_EQ(a, b) MINIGTEST_COMPARE_(FLOAT, a, b, return)
#define ASSERT_NEAR(a, b, tolerance)                                        \
  MINIGTEST_CHECK_(                                                         \
      ::testing::internal::CompareNEAR(#a, #b, (a), (b), (tolerance)),      \
      return)

#define ADD_FAILURE() \
  MINIGTEST_CHECK_(std::string("Failed"), )
#define FAIL() MINIGTEST_CHECK_(std::string("Failed"), return)
#define SUCCEED() MINIGTEST_CHECK_(std::string(), )

#endif  // MINIGTEST_GTEST_GTEST_H_
