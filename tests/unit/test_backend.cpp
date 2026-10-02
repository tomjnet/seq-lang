// C backend: unit composition and the hygiene rules.

#include <string>
#include <vector>

#include "seq/backend.hpp"
#include "test_harness.hpp"

namespace {

seq::Workflow TwoSteps() {
  seq::Workflow workflow;
  workflow.name = "demo";
  seq::StepDecl first;
  first.name = "load";
  seq::StepDecl second;
  second.name = "report";
  workflow.steps = {first, second};
  return workflow;
}

const char kGood[] =
    "static int total = 0;\n"
    "\n"
    "static int helper(int x) { return x * 2; }\n"
    "\n"
    "int seq_step_1(seq_ctx *ctx) {\n"
    "  (void)ctx;\n"
    "  total = helper(21);\n"
    "  return 0;\n"
    "}\n"
    "\n"
    "int seq_step_2(seq_ctx *ctx) {\n"
    "  if (total != 42) return seq_fail(ctx, \"bad total %d\", total);\n"
    "  printf(\"%d\\n\", total);\n"
    "  return 0;\n"
    "}\n";

std::unique_ptr<seq::Backend> MakeBackend() {
  return seq::CreateBackend(seq::BackendKind::kC, "/* runtime header */\n");
}

std::vector<seq::PolicyViolation> CheckC(const std::string& source) {
  return MakeBackend()->CheckStepFunctions(TwoSteps(), source);
}

std::vector<seq::PolicyViolation> CheckTests(const std::string& source) {
  return MakeBackend()->CheckTestCases(source);
}

bool Mentions(const std::vector<seq::PolicyViolation>& violations,
              const std::string& text) {
  for (const seq::PolicyViolation& violation : violations) {
    if (violation.message.find(text) != std::string::npos) return true;
  }
  return false;
}

// kGood with `extra` inserted before the step functions.
std::string With(const std::string& extra) { return extra + "\n" + kGood; }

}  // namespace

SEQ_TEST(Backend_AcceptsWellFormedStepFunctions) {
  const auto violations = CheckC(kGood);
  CHECK_EQ(violations.size(), std::size_t{0});
}

SEQ_TEST(Backend_ComposedUnitHasPreludeAndGuardedMain) {
  const std::string unit = MakeBackend()->ComposeUnit(TwoSteps(), kGood);
  CHECK(unit.find("#include \"seq_runtime.h\"") != std::string::npos);
  CHECK(unit.find("int seq_step_1(seq_ctx *ctx);") != std::string::npos);
  CHECK(unit.find("int seq_step_2(seq_ctx *ctx);") != std::string::npos);
  CHECK(unit.find("{\"load\", seq_step_1}") != std::string::npos);
  CHECK(unit.find("{\"report\", seq_step_2}") != std::string::npos);
  CHECK(unit.find("return seq_run(seq_steps, 2);") != std::string::npos);
  // main is compiled out when the unit is linked into the test binary.
  const std::size_t guard = unit.find("#ifndef SEQ_NO_MAIN");
  const std::size_t main_at = unit.find("int main(void)");
  const std::size_t model_at = unit.find("static int total = 0;");
  CHECK(guard != std::string::npos);
  CHECK(model_at < guard);
  CHECK(guard < main_at);
  CHECK(unit.find("#endif /* SEQ_NO_MAIN */") > main_at);
}

SEQ_TEST(Backend_ComposedTestHasExternCPrelude) {
  const std::string test = MakeBackend()->ComposeTest(
      TwoSteps(), "TEST(Demo, Runs) { EXPECT_EQ(1, 1); }\n");
  CHECK(test.find("#include <gtest/gtest.h>") != std::string::npos);
  const std::size_t open = test.find("extern \"C\" {");
  const std::size_t header = test.find("#include \"seq_runtime.h\"");
  const std::size_t prototype = test.find("int seq_step_2(seq_ctx *ctx);");
  const std::size_t cases = test.find("TEST(Demo, Runs)");
  CHECK(open < header);
  CHECK(header < prototype);
  CHECK(prototype < cases);
  CHECK(test.find("main") == std::string::npos);
}

SEQ_TEST(Backend_RejectsMain) {
  CHECK(Mentions(CheckC(With("int main(void) { return 0; }")), "main"));
}

SEQ_TEST(Backend_RequiresEveryStepExactlyOnce) {
  const std::string only_first =
      "int seq_step_1(seq_ctx *ctx) { (void)ctx; return 0; }\n";
  CHECK(Mentions(CheckC(only_first), "seq_step_2 is not defined"));

  const std::string twice =
      std::string(kGood) + "int seq_step_2(seq_ctx *ctx) { return 1; }\n";
  CHECK(Mentions(CheckC(twice), "defined more than once"));

  const std::string extra =
      std::string(kGood) + "int seq_step_3(seq_ctx *ctx) { return 0; }\n";
  CHECK(Mentions(CheckC(extra), "unexpected function seq_step_3"));

  // A prototype and a call are not definitions.
  const std::string with_prototype =
      "int seq_step_2(seq_ctx *ctx);\n" + std::string(kGood);
  CHECK_EQ(CheckC(with_prototype).size(), std::size_t{0});
}

SEQ_TEST(Backend_IncludeAllowlist) {
  CHECK_EQ(CheckC(With("#include <time.h>")).size(), std::size_t{0});
  CHECK_EQ(CheckC(With("#  include \"seq_runtime.h\"")).size(), std::size_t{0});
  for (const char* line :
       {"#include <unistd.h>", "#include <sys/socket.h>", "#include <signal.h>",
        "#include \"../secret.h\"", "#include \"/etc/passwd\"",
        "#include <gtest/gtest.h>", "#include HEADER"}) {
    CHECK(Mentions(CheckC(With(line)), "include"));
  }
  CHECK(Mentions(CheckC(With("#include_next <stdio.h>")), "not allowed"));
  CHECK(Mentions(CheckC(With("#embed \"data.bin\"")), "not allowed"));
  CHECK(Mentions(CheckC(With("#pragma GCC poison printf")), "not allowed"));
}

SEQ_TEST(Backend_RejectsProcessNetworkAndLoaderCalls) {
  for (const char* call :
       {"system(\"ls\")", "popen(\"ls\", \"r\")", "fork()", "execve(a, b, c)",
        "execl(a, b)", "socket(1, 2, 3)", "dlopen(\"x\", 0)", "syscall(57)",
        "connect(1, 0, 0)", "kill(1, 9)"}) {
    const std::string source =
        With(std::string("static void bad(void) { ") + call + "; }");
    CHECK(Mentions(CheckC(source), "not allowed"));
  }
  // A forbidden name hidden in a macro body is still found.
  CHECK(Mentions(CheckC(With("#define RUN system")), "'system'"));
  CHECK(Mentions(CheckC(With("static void f(void) { __asm__(\"nop\"); }")),
                 "'__asm__'"));
}

SEQ_TEST(Backend_IgnoresCommentsAndLiterals) {
  const std::string source = With(
      "/* system(\"x\"); int main() {} #include <unistd.h> */\n"
      "// fork(); main\n"
      "static const char *text = \"call system() from main {\";\n"
      "static const char quote = '\\'';\n"
      "static const char brace = '{';\n");
  CHECK_EQ(CheckC(source).size(), std::size_t{0});
}

SEQ_TEST(Backend_OrdinaryWordsAreAllowedAsNames) {
  // "bind" and "accept" are only rejected as calls.
  CHECK_EQ(CheckC(With("static int accept = 1;\nstatic int bind;\n")).size(),
           std::size_t{0});
}

SEQ_TEST(Backend_RejectsTricksThatHideDirectives) {
  // Written in two pieces so this file does not contain a trigraph itself.
  CHECK(Mentions(CheckC(With("?"
                             "?=include <unistd.h>")),
                 "trigraph"));
  CHECK(Mentions(CheckC(With("%:include <unistd.h>")), "digraph"));
  CHECK(Mentions(CheckC(With("#define GLUE(a, b) a##b")), "token pasting"));
  // A line splice cannot hide a name.
  CHECK(Mentions(CheckC(With("static void f(void) { sys\\\ntem(\"x\"); }")),
                 "'system'"));
  CHECK(Mentions(CheckC(With("#define SEQ_NO_MAIN")), "SEQ_NO_MAIN"));
}

SEQ_TEST(Backend_RejectsUnbalancedStructure) {
  // Either of these would swallow the compiler-owned main().
  CHECK(Mentions(CheckC(std::string(kGood) + "#if 0\n"), "conditionals"));
  CHECK(Mentions(CheckC(std::string(kGood) + "static void f(void) {\n"),
                 "unbalanced braces"));
  CHECK(Mentions(CheckC(std::string(kGood) + "}\n"), "unbalanced braces"));
  CHECK(Mentions(CheckC(With("#endif")), "#endif without"));
  CHECK(Mentions(CheckC(With("/* never closed")), "unterminated comment"));
}

SEQ_TEST(Backend_ReportsLineNumbers) {
  const auto violations = CheckC(With("\n\nstatic void f(void) { fork(); }"));
  CHECK_EQ(violations.size(), std::size_t{1});
  CHECK_EQ(violations[0].line, std::size_t{3});
  const std::string described = seq::DescribeViolations(
      violations, With("\n\nstatic void f(void) { fork(); }"));
  CHECK(described.find("line 3") != std::string::npos);
  CHECK(described.find("static void f(void) { fork(); }") != std::string::npos);
}

SEQ_TEST(Backend_RejectsOversizedOrBinarySource) {
  CHECK(Mentions(CheckC(std::string(300 * 1024, ' ')), "limit"));
  CHECK(Mentions(CheckC(std::string("int x;\0int y;", 13)), "UTF-8"));
}

SEQ_TEST(Backend_TestPolicy) {
  const std::string good =
      "TEST(Demo, StepsSucceed) {\n"
      "  seq_ctx ctx;\n"
      "  seq_ctx_init(&ctx);\n"
      "  EXPECT_EQ(0, seq_step_1(&ctx));\n"
      "  std::string raw = R\"(a \" system( main )\";\n"
      "  int big = 1'000'000;\n"
      "  (void)big;\n"
      "}\n";
  CHECK_EQ(CheckTests(good).size(), std::size_t{0});
  CHECK_EQ(
      CheckTests("#include <gtest/gtest.h>\n#include <map>\n" + good).size(),
      std::size_t{0});

  CHECK(Mentions(CheckTests("static int unused;\n"), "no TEST()"));
  CHECK(Mentions(CheckTests(good + "int main() { return 0; }\n"), "main"));
  CHECK(Mentions(CheckTests("#include <thread>\n" + good), "allowlist"));
  CHECK(Mentions(CheckTests("#include <filesystem>\n" + good), "allowlist"));
  for (const char* macro :
       {"EXPECT_DEATH(f(), \"\")", "ASSERT_DEATH(f(), \"\")",
        "EXPECT_EXIT(f(), a, \"\")", "ASSERT_DEATH_IF_SUPPORTED(f(), \"\")"}) {
    const std::string source = good + "TEST(Demo, Dies) { " + macro + "; }\n";
    CHECK(Mentions(CheckTests(source), "death tests"));
  }
  CHECK(
      Mentions(CheckTests(good + "TEST(Demo, Shell) { std::system(\"x\"); }\n"),
               "'system'"));
}
