// Lexer, parser, and validator: the language contract of docs/language.md.

#include <string>
#include <string_view>
#include <vector>

#include "seq/ast.hpp"
#include "seq/diagnostics.hpp"
#include "test_harness.hpp"
#include "validation/validator.hpp"

namespace {

using seq::Diagnostic;
using seq::Diagnostics;
using seq::SourceText;
using seq::Workflow;

const std::string kHeader =
    "model(\"https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct\")\n"
    "backend.C()\n"
    "name = \"demo\"\n";

const std::string kStep = "step s1():\n    ask(\"say hello\")\n";

struct Checked {
  bool ok = false;
  std::vector<Diagnostic> items;
  Workflow workflow;
  SourceText source;
  std::string printed;

  bool Has(std::string_view code) const { return Find(code) != nullptr; }

  const Diagnostic* Find(std::string_view code) const {
    for (const Diagnostic& item : items) {
      if (item.code == code) return &item;
    }
    return nullptr;
  }

  std::size_t Count(std::string_view code) const {
    std::size_t count = 0;
    for (const Diagnostic& item : items) {
      if (item.code == code) ++count;
    }
    return count;
  }

  // "line:column" of the first diagnostic with this code.
  std::string At(std::string_view code) const {
    const Diagnostic* item = Find(code);
    if (item == nullptr) return "none";
    return std::to_string(item->location.line) + ":" +
           std::to_string(item->location.column);
  }
};

Checked Check(std::string_view raw) {
  Checked result;
  Diagnostics diagnostics;
  result.ok = seq::CheckSourceText(raw, "test.seq", &result.source,
                                   &diagnostics, &result.workflow);
  result.items = diagnostics.items();
  std::ostringstream out;
  diagnostics.Print(out, result.source, false);
  result.printed = out.str();
  return result;
}

// Asserts that `raw` is rejected with exactly one diagnostic, `code`.
void ExpectOnly(std::string_view raw, std::string_view code, const char* file,
                int line) {
  const Checked result = Check(raw);
  if (!result.ok && result.items.size() == 1 && result.items[0].code == code) {
    return;
  }
  std::string got;
  for (const Diagnostic& item : result.items) got += item.code + " ";
  seqtest::ReportFailure(file, line,
                         "expected only " + std::string(code) + ", got: " +
                             (got.empty() ? "no diagnostics" : got));
}

#define EXPECT_ONLY(raw, code) ExpectOnly((raw), (code), __FILE__, __LINE__)

}  // namespace

SEQ_TEST(Language_ReferenceProgramParses) {
  const Checked result = Check(
      "model(\"https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct\")\n"
      "backend.C()\n"
      "name = \"top3Company\"\n"
      "\n"
      "step step1():\n"
      "    ask(\"create a database file company.txt with 5 sample stock "
      "transactions\")\n"
      "\n"
      "step step2():\n"
      "    ask(\"for each transaction created give me the total revenue of "
      "the company\")\n"
      "\n"
      "step step3():\n"
      "    ask(\"create an image chart showing the 3 companies with the "
      "highest revenue\")\n");
  CHECK(result.ok);
  CHECK_EQ(result.items.size(), std::size_t{0});
  CHECK_EQ(result.workflow.name, std::string("top3Company"));
  CHECK_EQ(result.workflow.model.owner, std::string("Qwen"));
  CHECK_EQ(result.workflow.model.repo,
           std::string("Qwen2.5-Coder-1.5B-Instruct"));
  CHECK(result.workflow.model.server.empty());
  CHECK_EQ(result.workflow.steps.size(), std::size_t{3});
  CHECK_EQ(result.workflow.steps[0].name, std::string("step1"));
  CHECK_EQ(result.workflow.steps[1].name, std::string("step2"));
  CHECK_EQ(result.workflow.steps[2].name, std::string("step3"));
  CHECK_EQ(result.workflow.steps[2].location.line, std::size_t{11});
}

SEQ_TEST(Language_RequestsKeepSourceOrder) {
  const Checked result = Check(kHeader +
                               "step a():\n"
                               "    ask(\"first\")\n"
                               "    ask(\"second\")\n"
                               "    ask(\"third\")\n"
                               "step b():\n"
                               "    ask(\"fourth\")\n");
  CHECK(result.ok);
  CHECK_EQ(result.workflow.steps.size(), std::size_t{2});
  CHECK_EQ(result.workflow.steps[0].asks.size(), std::size_t{3});
  CHECK_EQ(result.workflow.steps[0].asks[0].prompt, std::string("first"));
  CHECK_EQ(result.workflow.steps[0].asks[1].prompt, std::string("second"));
  CHECK_EQ(result.workflow.steps[0].asks[2].prompt, std::string("third"));
  CHECK_EQ(result.workflow.steps[1].asks[0].prompt, std::string("fourth"));
}

SEQ_TEST(Language_HeaderOrderIsUnrestricted) {
  const Checked result = Check(
      "name = \"demo\"\n"
      "backend.C()\n"
      "model(\"https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct\")\n" +
      kStep);
  CHECK(result.ok);
}

SEQ_TEST(Language_CrlfAndBomMatchLf) {
  const std::string lf = kHeader + kStep;
  std::string crlf;
  for (const char c : lf) {
    if (c == '\n') crlf += '\r';
    crlf += c;
  }
  const Checked plain = Check(lf);
  const Checked windows = Check(crlf);
  const Checked with_bom = Check("\xEF\xBB\xBF" + lf);
  CHECK(plain.ok);
  CHECK(windows.ok);
  CHECK(with_bom.ok);
  CHECK_EQ(windows.source.text, plain.source.text);
  CHECK_EQ(with_bom.source.text, plain.source.text);
  CHECK_EQ(windows.workflow.steps[0].asks[0].prompt,
           plain.workflow.steps[0].asks[0].prompt);
}

SEQ_TEST(Language_RejectsLoneCarriageReturn) {
  const Checked result = Check(kHeader + "step s1():\r    ask(\"x\")\n");
  CHECK(!result.ok);
  CHECK(result.Has("E0104"));
  CHECK_EQ(result.At("E0104"), std::string("4:11"));
}

SEQ_TEST(Language_RejectsTabs) {
  const Checked result = Check(kHeader + "step s1():\n\task(\"x\")\n");
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0103"), std::string("5:1"));
  // The line is reported once, not again as bad indentation.
  CHECK_EQ(result.items.size(), std::size_t{1});
}

SEQ_TEST(Language_RejectsNulAndControlCharacters) {
  CHECK(
      Check(kHeader + std::string("step s1():\n    ask(\"a") + '\0' + "b\")\n")
          .Has("E0102"));
  CHECK(Check(kHeader + "step s1():\n    ask(\"a\x01"
                        "b\")\n")
            .Has("E0102"));
}

SEQ_TEST(Language_RejectsInvalidUtf8) {
  const Checked result = Check(kHeader + "step s1():\n    ask(\"\xFF\")\n");
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0101"), std::string("5:10"));
  // Overlong encoding of '/'.
  CHECK(Check(kHeader + "step s1():\n    ask(\"\xC0\xAF\")\n").Has("E0101"));
}

SEQ_TEST(Language_AcceptsUnicodePrompts) {
  const Checked result =
      Check(kHeader +
            "step s1():\n    ask(\"caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC\")\n");
  CHECK(result.ok);
  CHECK_EQ(result.workflow.steps[0].asks[0].prompt,
           std::string("caf\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC"));
}

SEQ_TEST(Language_ColumnsCountCodePoints) {
  // The stray ')' follows a string holding two multi-byte characters.
  const Checked result =
      Check(kHeader + "step s1():\n    ask(\"\xC3\xA9\xC3\xA9\"))\n");
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0201"), std::string("5:14"));
}

SEQ_TEST(Language_DecodesEscapes) {
  const Checked result =
      Check(kHeader + "step s1():\n    ask(\"a \\\"q\\\" b\\\\c\\nd\\te\")\n");
  CHECK(result.ok);
  CHECK_EQ(result.workflow.steps[0].asks[0].prompt,
           std::string("a \"q\" b\\c\nd\te"));
}

SEQ_TEST(Language_RejectsUnknownEscape) {
  const Checked result = Check(kHeader + "step s1():\n    ask(\"a\\qb\")\n");
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0106"), std::string("5:11"));
  CHECK_EQ(result.items.size(), std::size_t{1});
}

SEQ_TEST(Language_RejectsUnterminatedString) {
  const Checked result = Check(kHeader + "step s1():\n    ask(\"abc)\n");
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0105"), std::string("5:9"));
}

SEQ_TEST(Language_CommentsAndBlankLines) {
  const Checked result = Check(
      "# leading comment\n"
      "\n" +
      kHeader +
      "   # an indented comment carries no indentation meaning\n"
      "step s1():   # trailing comment\n"
      "\n"
      "    # a comment in a body\n"
      "    ask(\"a # is not a comment inside a string\")   \n");
  CHECK(result.ok);
  CHECK_EQ(result.workflow.steps[0].asks[0].prompt,
           std::string("a # is not a comment inside a string"));
}

SEQ_TEST(Language_MissingFinalNewlineIsAccepted) {
  CHECK(Check(kHeader + "step s1():\n    ask(\"x\")").ok);
}

SEQ_TEST(Language_RejectsInconsistentIndentation) {
  const Checked two = Check(kHeader + "step s1():\n  ask(\"x\")\n");
  CHECK_EQ(two.At("E0108"), std::string("5:3"));
  const Checked eight = Check(kHeader + "step s1():\n        ask(\"x\")\n");
  CHECK(eight.Has("E0108"));
  const Checked stray = Check("    model(\"x\")\n" + kHeader + kStep);
  CHECK_EQ(stray.At("E0207"), std::string("1:5"));
}

SEQ_TEST(Language_EmptyFileReportsMissingHeaders) {
  const Checked result = Check("");
  CHECK(!result.ok);
  CHECK_EQ(result.items.size(), std::size_t{3});
  CHECK_EQ(result.At("E0301"), std::string("1:1"));
  CHECK_EQ(result.At("E0302"), std::string("1:1"));
  CHECK_EQ(result.At("E0303"), std::string("1:1"));
}

SEQ_TEST(Language_MissingHeaders) {
  EXPECT_ONLY("backend.C()\nname = \"demo\"\n" + kStep, "E0301");
  EXPECT_ONLY(
      "model(\"https://huggingface.co/a/b\")\nname = \"demo\"\n" + kStep,
      "E0302");
  EXPECT_ONLY("model(\"https://huggingface.co/a/b\")\nbackend.C()\n" + kStep,
              "E0303");
}

SEQ_TEST(Language_RejectsWorkflowWithoutSteps) {
  EXPECT_ONLY(kHeader, "E0314");
}

SEQ_TEST(Language_RejectsDuplicateHeaders) {
  const Checked result = Check(kHeader + "name = \"other\"\n" + kStep);
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0304"), std::string("4:1"));
  CHECK(Check(kHeader + "backend.C()\n" + kStep).Has("E0304"));
  CHECK(Check(kHeader + "model(\"https://huggingface.co/a/b\")\n" + kStep)
            .Has("E0304"));
}

SEQ_TEST(Language_RejectsDuplicateStepNames) {
  const Checked result = Check(kHeader + kStep + kStep);
  CHECK(!result.ok);
  CHECK_EQ(result.At("E0315"), std::string("6:6"));
}

SEQ_TEST(Language_RejectsReservedStepNames) {
  for (const char* word : {"model", "backend", "name", "step", "ask"}) {
    const Checked result =
        Check(kHeader + "step " + word + "():\n    ask(\"x\")\n");
    CHECK(!result.ok);
    CHECK_EQ(result.At("E0208"), std::string("4:6"));
  }
}

SEQ_TEST(Language_ModelCallForms) {
  const std::string rest = "backend.C()\nname = \"demo\"\n" + kStep;
  CHECK(Check("model(\"https://huggingface.co/a/b\")\n" + rest).ok);

  const Checked assignment =
      Check("model = \"https://huggingface.co/a/b\"\n" + rest);
  CHECK_EQ(assignment.At("E0202"), std::string("1:7"));
  CHECK(assignment.printed.find("model(\"https://huggingface.co/") !=
        std::string::npos);

  // The reserved server argument is recognized and rejected at its location.
  const Checked server =
      Check("model(\"https://huggingface.co/a/b\", \"10.0.0.45\")\n" + rest);
  CHECK_EQ(server.items.size(), std::size_t{1});
  CHECK_EQ(server.At("E0310"), std::string("1:37"));
  CHECK(server.items[0].message.find("not supported in v0.1") !=
        std::string::npos);

  EXPECT_ONLY("model()\n" + rest, "E0307");
  EXPECT_ONLY("model(\"https://huggingface.co/a/b\", \"x\", \"y\")\n" + rest,
              "E0307");
  EXPECT_ONLY("model(42)\n" + rest, "E0308");
  EXPECT_ONLY("model(qwen)\n" + rest, "E0308");
  EXPECT_ONLY("model(\"\")\n" + rest, "E0309");
  EXPECT_ONLY("model(\"   \")\n" + rest, "E0309");
}

SEQ_TEST(Language_ModelUrlForms) {
  const std::string rest = "backend.C()\nname = \"demo\"\n" + kStep;
  const Checked pinned =
      Check("model(\"https://huggingface.co/a/b/tree/abc123\")\n" + rest);
  CHECK(pinned.ok);
  CHECK_EQ(pinned.workflow.model.revision, std::string("abc123"));

  for (const char* url :
       {"http://huggingface.co/a/b", "https://example.com/a/b",
        "https://huggingface.co/a", "https://huggingface.co/a/b/",
        "https://huggingface.co/a/b/blob/main", "https://huggingface.co/a/../b",
        "https://huggingface.co/a/b/tree/", "Qwen/Qwen2.5-Coder-1.5B-Instruct",
        "https://huggingface.co/a/b?x=1"}) {
    const Checked result =
        Check(std::string("model(\"") + url + "\")\n" + rest);
    CHECK(!result.ok);
    CHECK_EQ(result.At("E0311"), std::string("1:7"));
  }
}

SEQ_TEST(Language_BackendForms) {
  const std::string model = "model(\"https://huggingface.co/a/b\")\n";
  const std::string rest = "name = \"demo\"\n" + kStep;
  const Checked rust = Check(model + "backend.Rust()\n" + rest);
  CHECK_EQ(rust.items.size(), std::size_t{1});
  CHECK_EQ(rust.At("E0305"), std::string("2:9"));
  EXPECT_ONLY(model + "backend.Go()\n" + rest, "E0306");
  EXPECT_ONLY(model + "backend = \"c\"\n" + rest, "E0203");
  EXPECT_ONLY(model + "backend.C(\"fast\")\n" + rest, "E0320");
  EXPECT_ONLY(model + "backend.C\n" + rest, "E0201");
  EXPECT_ONLY(model + "backend\n" + rest, "E0201");
}

SEQ_TEST(Language_NameRules) {
  const std::string head =
      "model(\"https://huggingface.co/a/b\")\nbackend.C()\n";
  for (const char* name :
       {"a b", "1abc", "a/b", "../x", "a.b", "", "a;rm", "caf\xC3\xA9"}) {
    const Checked result = Check(head + "name = \"" + name + "\"\n" + kStep);
    CHECK(!result.ok);
    CHECK_EQ(result.At("E0312"), std::string("3:8"));
  }
  CHECK(Check(head + "name = \"_ok-Name_9\"\n" + kStep).ok);
  CHECK(Check(head + "name = \"" + std::string(64, 'a') + "\"\n" + kStep).ok);
  EXPECT_ONLY(head + "name = \"" + std::string(65, 'a') + "\"\n" + kStep,
              "E0313");
  EXPECT_ONLY(head + "name(\"demo\")\n" + kStep, "E0201");
  EXPECT_ONLY(head + "name = demo\n" + kStep, "E0201");
}

SEQ_TEST(Language_AskForms) {
  EXPECT_ONLY(kHeader + "step s1():\n    ask \"hello\"\n", "E0204");
  EXPECT_ONLY(kHeader + "step s1():\n    ask()\n", "E0213");
  EXPECT_ONLY(kHeader + "step s1():\n    ask(hello)\n", "E0213");
  EXPECT_ONLY(kHeader + "step s1():\n    ask(\"a\", \"b\")\n", "E0213");
  EXPECT_ONLY(kHeader + "step s1():\n    ask(\"\")\n", "E0316");
  EXPECT_ONLY(kHeader + "step s1():\n    ask(\"  \")\n", "E0316");
  EXPECT_ONLY(kHeader + "step s1():\n    print(\"x\")\n", "E0212");
  EXPECT_ONLY(kHeader + "ask(\"x\")\n" + kStep, "E0206");
  EXPECT_ONLY(kHeader + "step s1():\n    ask(\"a\") ask(\"b\")\n", "E0201");
}

SEQ_TEST(Language_StepForms) {
  EXPECT_ONLY(kHeader + "step s1():\n", "E0209");
  EXPECT_ONLY(kHeader + "step s1():\nstep s2():\n    ask(\"x\")\n", "E0209");
  EXPECT_ONLY(kHeader + "step s1(x):\n    ask(\"x\")\n", "E0211");
  EXPECT_ONLY(kHeader + "step s1()\n    ask(\"x\")\n", "E0201");
  EXPECT_ONLY(kHeader + "step s1:\n    ask(\"x\")\n", "E0201");
  EXPECT_ONLY(kHeader + "step ():\n    ask(\"x\")\n", "E0201");
  EXPECT_ONLY(kHeader + "step s1(): ask(\"x\")\n", "E0201");
  EXPECT_ONLY(kHeader + "import os\n" + kStep, "E0201");
}

SEQ_TEST(Language_HeadersMustPrecedeSteps) {
  const Checked result =
      Check("model(\"https://huggingface.co/a/b\")\nbackend.C()\n" + kStep +
            "name = \"demo\"\n");
  CHECK_EQ(result.items.size(), std::size_t{1});
  CHECK_EQ(result.At("E0205"), std::string("5:1"));
}

SEQ_TEST(Language_Limits) {
  CHECK(Check(kHeader + "step s1():\n    ask(\"" +
              std::string(seq::kMaxPromptBytes, 'a') + "\")\n")
            .ok);
  EXPECT_ONLY(kHeader + "step s1():\n    ask(\"" +
                  std::string(seq::kMaxPromptBytes + 1, 'a') + "\")\n",
              "E0317");

  std::string many_asks = kHeader + "step s1():\n";
  for (std::size_t i = 0; i < seq::kMaxAsksPerStep; ++i) {
    many_asks += "    ask(\"x\")\n";
  }
  CHECK(Check(many_asks).ok);
  EXPECT_ONLY(many_asks + "    ask(\"one too many\")\n", "E0319");

  std::string many_steps = kHeader;
  for (std::size_t i = 0; i < seq::kMaxSteps; ++i) {
    many_steps += "step s" + std::to_string(i) + "():\n    ask(\"x\")\n";
  }
  CHECK(Check(many_steps).ok);
  EXPECT_ONLY(many_steps + "step extra():\n    ask(\"x\")\n", "E0318");

  EXPECT_ONLY(std::string(seq::kMaxSourceBytes + 1, '#'), "E0109");
}

SEQ_TEST(Language_SyntaxErrorsDoNotCascade) {
  // One broken header line must not also be reported as a missing header.
  EXPECT_ONLY("model(\nbackend.C()\nname = \"demo\"\n" + kStep, "E0201");
  // A broken step header must not flag its body as stray indentation.
  EXPECT_ONLY(kHeader + "step s1(\n    ask(\"x\")\n", "E0201");
}

SEQ_TEST(Language_SemanticErrorsAreReportedBesideSyntaxErrors) {
  const Checked result = Check(
      "model(\"https://huggingface.co/a/b\")\nbackend.Rust()\n"
      "name = \"a b\"\nstep s1():\n    ask \"x\"\n");
  CHECK(result.Has("E0305"));
  CHECK(result.Has("E0312"));
  CHECK(result.Has("E0204"));
}

SEQ_TEST(Diagnostics_Format) {
  const Checked result = Check(kHeader + "step s1():\n    ask \"hello\"\n");
  CHECK_EQ(result.printed,
           std::string("test.seq:5:9: error[E0204]: ask requires parentheses\n"
                       "        ask \"hello\"\n"
                       "            ^\n"
                       "    hint: write ask(\"...\")\n"));
}

SEQ_TEST(Diagnostics_AreSortedBySourcePosition) {
  const Checked result =
      Check("name = \"a b\"\nmodel(\"nope\")\nbackend.Rust()\n" + kStep);
  CHECK_EQ(result.items.size(), std::size_t{3});
  const std::size_t name_at = result.printed.find("E0312");
  const std::size_t model_at = result.printed.find("E0311");
  const std::size_t backend_at = result.printed.find("E0305");
  CHECK(name_at < model_at);
  CHECK(model_at < backend_at);
}
