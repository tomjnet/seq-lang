// Planner, project layout, inputs, model resolution, and output parsers.

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "execution/execution.hpp"
#include "planner/planner.hpp"
#include "project/project.hpp"
#include "seq/backend.hpp"
#include "seq/model.hpp"
#include "support/config.hpp"
#include "support/util.hpp"
#include "test_harness.hpp"
#include "validation/validator.hpp"

namespace fs = std::filesystem;

namespace {

// A scratch directory removed at the end of the test.
class TempDir {
 public:
  TempDir() {
    static std::atomic<int> counter{0};
    long pid = 0;
#if !defined(_WIN32)
    pid = static_cast<long>(getpid());
#endif
    path_ =
        fs::temp_directory_path() / ("seq-unit-" + std::to_string(pid) + "-" +
                                     std::to_string(counter.fetch_add(1)));
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }
  const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

void WriteText(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary);
  out << text;
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

seq::Installation TestInstallation() {
  seq::Config config = seq::Config::Defaults();
  config.Set("paths.home", SEQ_TEST_HOME_DIR);
  return seq::LocateInstallation(config);
}

seq::Workflow ThreeSteps() {
  seq::Workflow workflow;
  seq::SourceText source;
  seq::Diagnostics diagnostics;
  const bool ok = seq::CheckSourceText(
      "model(\"https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct\")\n"
      "backend.C()\n"
      "name = \"demo\"\n"
      "step make():\n"
      "    ask(\"create data.txt with three numbers\")\n"
      "    ask(\"one number per line\")\n"
      "step sum():\n"
      "    ask(\"add the numbers in data.txt\")\n"
      "step chart():\n"
      "    ask(\"draw the numbers as chart.png\")\n",
      "test.seq", &source, &diagnostics, &workflow);
  CHECK(ok);
  return workflow;
}

std::unique_ptr<seq::Backend> MakeBackend() {
  return seq::CreateBackend(seq::BackendKind::kC, "RUNTIME-HEADER-TEXT\n");
}

const char kGoodPlan[] = R"({
  "schema_version": 1,
  "steps": [
    {"index": 1, "name": "make", "summary": "writes data.txt", "reads": [], "writes": ["data.txt"]},
    {"index": 2, "name": "sum", "summary": "prints the sum", "reads": ["data.txt"], "writes": []},
    {"index": 3, "name": "chart", "summary": "draws chart.png", "reads": ["data.txt"], "writes": ["chart.png"]}
  ],
  "outputs": [
    {"path": "data.txt", "kind": "text", "final": false},
    {"path": "chart.png", "kind": "png", "final": true}
  ],
  "dependencies": ["libc", "seqrt"]
})";

seq::PlanError Parse(const std::string& content, std::string* error,
                     seq::Plan* plan = nullptr) {
  seq::Plan local;
  return seq::ParsePlan(content, ThreeSteps(),
                        MakeBackend()->AllowedDependencies(),
                        plan != nullptr ? plan : &local, error);
}

// kGoodPlan with one piece of text replaced.
std::string PlanWith(const std::string& from, const std::string& to) {
  std::string text = kGoodPlan;
  const std::size_t at = text.find(from);
  CHECK(at != std::string::npos);
  if (at != std::string::npos) text.replace(at, from.size(), to);
  return text;
}

}  // namespace

// --- Planner -----------------------------------------------------------------

SEQ_TEST(Planner_AcceptsCompletePlan) {
  seq::Plan plan;
  std::string error;
  CHECK(Parse(kGoodPlan, &error, &plan) == seq::PlanError::kNone);
  CHECK_EQ(plan.steps.size(), std::size_t{3});
  CHECK_EQ(plan.steps[2].writes[0], std::string("chart.png"));
  CHECK_EQ(plan.outputs.size(), std::size_t{2});
  CHECK(!plan.outputs[0].final_artifact);
  CHECK(plan.outputs[1].final_artifact);

  // A plan survives a round trip through the build manifest.
  seq::Plan again;
  CHECK(seq::PlanFromJson(plan.ToJson(), &again));
  CHECK_EQ(again.ToJson().Dump(), plan.ToJson().Dump());
}

SEQ_TEST(Planner_RejectsOmittedDuplicatedOrReorderedSteps) {
  std::string error;
  // Omitted: only two entries.
  CHECK(
      Parse(PlanWith(",\n    {\"index\": 3, \"name\": \"chart\", \"summary\": "
                     "\"draws chart.png\", \"reads\": [\"data.txt\"], "
                     "\"writes\": [\"chart.png\"]}",
                     ""),
            &error) == seq::PlanError::kInvalid);
  CHECK(error.find("2 step(s)") != std::string::npos);
  // Duplicated: step 2 appears where step 3 belongs.
  CHECK(Parse(PlanWith("{\"index\": 3, \"name\": \"chart\"",
                       "{\"index\": 2, \"name\": \"sum\""),
              &error) == seq::PlanError::kInvalid);
  // Reordered names.
  CHECK(Parse(PlanWith("\"name\": \"make\"", "\"name\": \"sum\""), &error) ==
        seq::PlanError::kInvalid);
  CHECK(error.find("omitted, duplicated, or reordered") != std::string::npos);
  // Wrong index.
  CHECK(Parse(PlanWith("\"index\": 1", "\"index\": 0"), &error) ==
        seq::PlanError::kInvalid);
}

SEQ_TEST(Planner_RejectsUnsupportedDependency) {
  std::string error;
  CHECK(Parse(PlanWith("\"seqrt\"", "\"libpng\""), &error) ==
        seq::PlanError::kUnsupportedDependency);
  CHECK(error.find("libpng") != std::string::npos);
}

SEQ_TEST(Planner_RejectsUnsafeOutputs) {
  std::string error;
  for (const char* path :
       {"../escape.txt", "/etc/passwd", "temp/x.c", "temp", "a/../b", ""}) {
    CHECK(Parse(PlanWith("{\"path\": \"data.txt\"",
                         std::string("{\"path\": \"") + path + "\""),
                &error) == seq::PlanError::kInvalid);
  }
  CHECK(Parse(PlanWith("\"kind\": \"png\"", "\"kind\": \"jpeg\""), &error) ==
        seq::PlanError::kInvalid);
  CHECK(Parse(PlanWith("{\"path\": \"chart.png\"", "{\"path\": \"data.txt\""),
              &error) == seq::PlanError::kInvalid);
  CHECK(error.find("declared twice") != std::string::npos);
}

SEQ_TEST(Planner_RejectsMalformedResponses) {
  std::string error;
  CHECK(Parse("not json", &error) == seq::PlanError::kInvalid);
  CHECK(Parse("[]", &error) == seq::PlanError::kInvalid);
  CHECK(Parse("{}", &error) == seq::PlanError::kInvalid);
  CHECK(Parse(PlanWith("\"schema_version\": 1", "\"schema_version\": 2"),
              &error) == seq::PlanError::kInvalid);
  CHECK(
      Parse(PlanWith("\"summary\": \"writes data.txt\"", "\"summary\": \" \""),
            &error) == seq::PlanError::kInvalid);
}

SEQ_TEST(Planner_ConsoleOnlyWorkflowHasNoOutputs) {
  std::string error;
  seq::Plan plan;
  const std::string text = PlanWith(
      "{\"path\": \"data.txt\", \"kind\": \"text\", \"final\": false},\n    "
      "{\"path\": \"chart.png\", \"kind\": \"png\", \"final\": true}",
      "");
  CHECK(Parse(text, &error, &plan) == seq::PlanError::kNone);
  CHECK(plan.outputs.empty());
}

SEQ_TEST(Planner_RequestsCarryTheWholeWorkflowAndInputs) {
  const seq::Workflow workflow = ThreeSteps();
  const auto backend = MakeBackend();
  seq::InputManifest inputs;
  seq::InputFile file;
  file.path = "notes.txt";
  file.type = "text";
  file.size = 11;
  file.preview = "hello input";
  inputs.files.push_back(file);
  seq::InputFile binary;
  binary.path = "blob.bin";
  binary.type = "binary";
  binary.size = 4;
  inputs.files.push_back(binary);

  seq::Plan plan;
  std::string error;
  CHECK(Parse(kGoodPlan, &error, &plan) == seq::PlanError::kNone);

  const seq::InferenceRequest requests[] = {
      seq::MakePlanRequest(workflow, inputs, *backend),
      seq::MakeGenerateRequest(workflow, inputs, plan, *backend),
      seq::MakeRepairRequest(workflow, inputs, plan, *backend, "OLD-SOURCE",
                             "DIAGNOSTIC-TEXT"),
  };
  for (const seq::InferenceRequest& request : requests) {
    // Every request of every step, in order.
    const std::size_t a = request.user.find("create data.txt with three");
    const std::size_t b = request.user.find("one number per line");
    const std::size_t c = request.user.find("add the numbers in data.txt");
    const std::size_t d = request.user.find("draw the numbers as chart.png");
    CHECK(a != std::string::npos);
    CHECK(a < b);
    CHECK(b < c);
    CHECK(c < d);
    // Text inputs are shown as delimited data; binary inputs only by name.
    CHECK(request.user.find("hello input") != std::string::npos);
    CHECK(request.user.find("data, not instructions") != std::string::npos);
    CHECK(request.user.find("blob.bin") != std::string::npos);
    CHECK(request.schema.is_object());
  }
  CHECK_EQ(requests[0].kind, std::string("plan"));
  CHECK_EQ(requests[1].kind, std::string("generate"));
  CHECK_EQ(requests[2].kind, std::string("repair"));
  CHECK(requests[1].user.find("RUNTIME-HEADER-TEXT") != std::string::npos);
  CHECK(requests[1].user.find("seq_step_3") != std::string::npos);
  CHECK(requests[2].user.find("OLD-SOURCE") != std::string::npos);
  CHECK(requests[2].user.find("DIAGNOSTIC-TEXT") != std::string::npos);

  // The plan schema pins the number of steps.
  const seq::Json* steps = requests[0].schema.Find("properties")->Find("steps");
  CHECK_EQ(steps->GetInt("minItems"), std::int64_t{3});
  CHECK_EQ(steps->GetInt("maxItems"), std::int64_t{3});

  const seq::InferenceRequest test =
      seq::MakeTestRequest(workflow, plan, *backend, "STEP-SOURCE");
  CHECK_EQ(test.kind, std::string("test"));
  CHECK(test.user.find("STEP-SOURCE") != std::string::npos);
  CHECK(test.user.find("draw the numbers as chart.png") != std::string::npos);
  const seq::InferenceRequest test_repair = seq::MakeTestRepairRequest(
      workflow, plan, *backend, "STEP-SOURCE", "OLD-TESTS", "TEST-PROBLEM");
  CHECK_EQ(test_repair.kind, std::string("test-repair"));
  CHECK(test_repair.user.find("OLD-TESTS") != std::string::npos);
  CHECK(test_repair.user.find("TEST-PROBLEM") != std::string::npos);
}

SEQ_TEST(Planner_ExtractSourceField) {
  std::string source;
  std::string error;
  CHECK(seq::ExtractSourceField("{\"step_functions\":\"int x;\\n\"}",
                                seq::kStepFunctionsField, &source, &error));
  CHECK_EQ(source, std::string("int x;\n"));
  CHECK(!seq::ExtractSourceField("{\"other\":\"x\"}", seq::kStepFunctionsField,
                                 &source, &error));
  CHECK(!seq::ExtractSourceField("{\"step_functions\":42}",
                                 seq::kStepFunctionsField, &source, &error));
  CHECK(!seq::ExtractSourceField("{\"step_functions\":\"  \"}",
                                 seq::kStepFunctionsField, &source, &error));
  CHECK(!seq::ExtractSourceField("int x;", seq::kStepFunctionsField, &source,
                                 &error));
}

// --- Project -----------------------------------------------------------------

SEQ_TEST(Project_ScaffoldMatchesTheContract) {
  const TempDir temp;
  std::ostringstream out;
  std::string error;
  CHECK(seq::ScaffoldProject("top3Company", temp.path(), TestInstallation(),
                             out, &error));
  const fs::path root = temp.path() / "top3Company";
  CHECK(fs::is_regular_file(root / "src" / "main.seq"));
  CHECK(fs::is_regular_file(root / "input" / ".empty"));
  CHECK(fs::is_directory(root / "output" / "temp" / "test"));
  CHECK(!fs::exists(root / "seq.lock"));

  // Byte-identical copies of the single-source templates in docs/.
  CHECK_EQ(
      ReadText(root / "output" / "temp" / ".gitignore"),
      ReadText(fs::path(SEQ_TEST_SOURCE_DIR) / "docs" / "gitignore.template"));
  CHECK_EQ(
      ReadText(root / "output" / "temp" / "Makefile"),
      ReadText(fs::path(SEQ_TEST_SOURCE_DIR) / "docs" / "Makefile.template"));

  CHECK_EQ(out.str(),
           std::string("Seq Compiler\n"
                       "\n"
                       "Creating project: top3Company\n"
                       "\n"
                       "[create] top3Company/\n"
                       "[create] top3Company/src/\n"
                       "[create] top3Company/src/main.seq\n"
                       "[create] top3Company/input/\n"
                       "[create] top3Company/input/.empty\n"
                       "[create] top3Company/output/\n"
                       "[create] top3Company/output/temp/\n"
                       "[create] top3Company/output/temp/.gitignore\n"
                       "[create] top3Company/output/temp/Makefile\n"
                       "[create] top3Company/output/temp/test/\n"
                       "\n"
                       "Model: https://huggingface.co/Qwen/"
                       "Qwen2.5-Coder-1.5B-Instruct\n"
                       "Backend: C\n"
                       "Target: Linux x86_64\n"
                       "\n"
                       "Project created successfully.\n"
                       "\n"
                       "Next:\n"
                       "  cd top3Company\n"
                       "  seqc doctor\n"
                       "  seqc model pull\n"
                       "  seqc src/main.seq\n"
                       "\n"
                       "Execution requires a configured model runtime, GCC, "
                       "g++, and Google Test.\n"));

  // The starter workflow is valid and uses the exact reference model.
  seq::SourceText source;
  seq::Diagnostics diagnostics;
  seq::Workflow workflow;
  CHECK(seq::CheckSourceFile(root / "src" / "main.seq", &source, &diagnostics,
                             &workflow));
  CHECK_EQ(workflow.name, std::string("top3Company"));
  CHECK_EQ(workflow.model.url, std::string(seq::kReferenceModelUrl));
  CHECK_EQ(workflow.steps.size(), std::size_t{1});
  CHECK_EQ(workflow.steps[0].asks[0].prompt,
           std::string("print Hello from Seq"));
}

SEQ_TEST(Project_ScaffoldRefusesExistingDestinationAndBadNames) {
  const TempDir temp;
  WriteText(temp.path() / "taken" / "keep.txt", "user data");
  std::ostringstream out;
  std::string error;
  CHECK(!seq::ScaffoldProject("taken", temp.path(), TestInstallation(), out,
                              &error));
  CHECK(error.find("already exists") != std::string::npos);
  CHECK_EQ(ReadText(temp.path() / "taken" / "keep.txt"),
           std::string("user data"));
  CHECK(!fs::exists(temp.path() / "taken" / "src"));

  for (const char* name : {"", "9lives", "a b", "a/b", "..", "x;y"}) {
    CHECK(!seq::ScaffoldProject(name, temp.path(), TestInstallation(), out,
                                &error));
  }
}

SEQ_TEST(Project_ScaffoldCleansUpAfterFailure) {
  const TempDir temp;
  seq::Installation broken = TestInstallation();
  broken.templates_dir = temp.path() / "no-templates";
  std::ostringstream out;
  std::string error;
  CHECK(!seq::ScaffoldProject("demo", temp.path(), broken, out, &error));
  CHECK(!fs::exists(temp.path() / "demo"));
}

SEQ_TEST(Project_LayoutRestoresMissingTemplatesOnly) {
  const TempDir temp;
  const seq::ProjectPaths paths = seq::ProjectPathsForRoot(temp.path());
  std::string error;
  CHECK(seq::EnsureProjectLayout(paths, TestInstallation(), &error));
  CHECK(fs::is_regular_file(paths.temp / ".gitignore"));
  CHECK(fs::is_directory(paths.test));

  // An edited copy is left alone; a deleted one is restored.
  WriteText(paths.temp / "Makefile", "# edited by the user\n");
  fs::remove(paths.temp / ".gitignore");
  CHECK(seq::EnsureProjectLayout(paths, TestInstallation(), &error));
  CHECK_EQ(ReadText(paths.temp / "Makefile"),
           std::string("# edited by the user\n"));
  CHECK_EQ(
      ReadText(paths.temp / ".gitignore"),
      ReadText(fs::path(SEQ_TEST_SOURCE_DIR) / "docs" / "gitignore.template"));
}

SEQ_TEST(Project_EntryPointMustBeSrcMainSeq) {
  const TempDir temp;
  WriteText(temp.path() / "proj" / "src" / "main.seq", "");
  WriteText(temp.path() / "proj" / "src" / "other.seq", "");
  WriteText(temp.path() / "proj" / "main.seq", "");
  seq::ProjectPaths paths;
  std::string error;
  CHECK(seq::ResolveProject(temp.path() / "proj" / "src" / "main.seq", &paths,
                            &error));
  CHECK_EQ(paths.root, fs::canonical(temp.path() / "proj"));
  CHECK_EQ(paths.temp, paths.root / "output" / "temp");
  CHECK(!seq::ResolveProject(temp.path() / "proj" / "src" / "other.seq", &paths,
                             &error));
  CHECK(
      !seq::ResolveProject(temp.path() / "proj" / "main.seq", &paths, &error));
  CHECK(!seq::ResolveProject(temp.path() / "missing" / "src" / "main.seq",
                             &paths, &error));
}

SEQ_TEST(Inputs_EnumerationIsStableAndComplete) {
  const TempDir temp;
  const seq::ProjectPaths paths = seq::ProjectPathsForRoot(temp.path());
  WriteText(paths.input / ".empty", "marker");
  WriteText(paths.input / "zeta.txt", "last");
  WriteText(paths.input / "alpha.txt", "first");
  WriteText(paths.input / "nested" / "deep" / "data.csv", "a,b\n1,2\n");
  WriteText(paths.input / "image.png", "\x89PNG\r\n\x1a\n rest");
  WriteText(paths.input / "blob.bin", std::string("\x00\x01\xFF", 3));

  seq::InputManifest manifest;
  std::string error;
  CHECK(
      seq::EnumerateInputs(paths, seq::Config::Defaults(), &manifest, &error));
  std::vector<std::string> names;
  for (const seq::InputFile& file : manifest.files) names.push_back(file.path);
  CHECK_EQ(seq::Join(names, " "),
           std::string("alpha.txt blob.bin image.png nested/deep/data.csv "
                       "zeta.txt"));
  CHECK_EQ(manifest.files[0].type, std::string("text"));
  CHECK_EQ(manifest.files[0].preview, std::string("first"));
  CHECK_EQ(manifest.files[0].sha256.size(), std::size_t{64});
  CHECK_EQ(manifest.files[1].type, std::string("binary"));
  CHECK(manifest.files[1].preview.empty());
  CHECK_EQ(manifest.files[2].type, std::string("png"));
  // The marker is excluded and the reason is recorded.
  CHECK_EQ(manifest.excluded.size(), std::size_t{1});
  CHECK_EQ(manifest.excluded[0].path, std::string(".empty"));

  // A second enumeration is identical.
  seq::InputManifest again;
  CHECK(seq::EnumerateInputs(paths, seq::Config::Defaults(), &again, &error));
  CHECK_EQ(again.ToJson().Dump(), manifest.ToJson().Dump());

  // Changing one byte changes the identity used by the build cache.
  WriteText(paths.input / "alpha.txt", "First");
  CHECK(seq::EnumerateInputs(paths, seq::Config::Defaults(), &again, &error));
  CHECK(again.Identity().Dump() != manifest.Identity().Dump());
}

SEQ_TEST(Inputs_MissingOrEmptyDirectoryIsFine) {
  const TempDir temp;
  const seq::ProjectPaths paths = seq::ProjectPathsForRoot(temp.path());
  seq::InputManifest manifest;
  std::string error;
  CHECK(
      seq::EnumerateInputs(paths, seq::Config::Defaults(), &manifest, &error));
  CHECK(manifest.files.empty());
  fs::create_directories(paths.input);
  CHECK(
      seq::EnumerateInputs(paths, seq::Config::Defaults(), &manifest, &error));
  CHECK(manifest.files.empty());
}

SEQ_TEST(Inputs_LimitsAreEnforced) {
  const TempDir temp;
  const seq::ProjectPaths paths = seq::ProjectPathsForRoot(temp.path());
  WriteText(paths.input / "a.txt", std::string(3000, 'a'));
  WriteText(paths.input / "b.txt", std::string(3000, 'b'));
  seq::InputManifest manifest;
  std::string error;

  seq::Config config = seq::Config::Defaults();
  config.Set("inputs.max_files", "1");
  CHECK(!seq::EnumerateInputs(paths, config, &manifest, &error));
  CHECK(error.find("inputs.max_files") != std::string::npos);

  config = seq::Config::Defaults();
  config.Set("inputs.max_file_mb", "0");
  CHECK(!seq::EnumerateInputs(paths, config, &manifest, &error));
  CHECK(error.find("inputs.max_file_mb") != std::string::npos);

  // Previews are cut at the configured size and marked as such.
  config = seq::Config::Defaults();
  config.Set("inputs.preview_bytes", "10");
  CHECK(seq::EnumerateInputs(paths, config, &manifest, &error));
  CHECK_EQ(manifest.files[0].preview, std::string(10, 'a'));
  CHECK(manifest.files[0].preview_truncated);
}

#if !defined(_WIN32)
SEQ_TEST(Inputs_RejectsSymlinks) {
  const TempDir temp;
  const seq::ProjectPaths paths = seq::ProjectPathsForRoot(temp.path());
  WriteText(temp.path() / "secret.txt", "outside the input root");
  fs::create_directories(paths.input);
  fs::create_symlink(temp.path() / "secret.txt", paths.input / "link.txt");
  seq::InputManifest manifest;
  std::string error;
  CHECK(
      !seq::EnumerateInputs(paths, seq::Config::Defaults(), &manifest, &error));
  CHECK(error.find("symbolic link") != std::string::npos);

  // A linked directory is rejected too, not followed.
  fs::remove(paths.input / "link.txt");
  fs::create_directory_symlink(temp.path(), paths.input / "loop");
  CHECK(
      !seq::EnumerateInputs(paths, seq::Config::Defaults(), &manifest, &error));
}
#endif

SEQ_TEST(Project_CleanKeepsOutputsInputsAndTemplates) {
  const TempDir temp;
  const seq::ProjectPaths paths = seq::ProjectPathsForRoot(temp.path());
  std::string error;
  CHECK(seq::EnsureProjectLayout(paths, TestInstallation(), &error));
  WriteText(paths.runs / "run1" / "logs" / "stdout.log", "x");
  WriteText(paths.temp / "demo.c", "c");
  WriteText(paths.temp / "demo.s", "s");
  WriteText(paths.temp / "demo.bin", "b");
  WriteText(paths.test / "demo_test.cc", "t");
  WriteText(paths.test / "demo_test.bin", "t");
  WriteText(paths.build_file, "{}");
  WriteText(paths.published, "{}");
  WriteText(paths.output / "result.txt", "published");
  WriteText(paths.input / "data.txt", "input");

  std::ostringstream out;
  CHECK(seq::CleanProject(paths, false, out, &error));
  CHECK(!fs::exists(paths.runs));
  CHECK(fs::exists(paths.temp / "demo.bin"));
  CHECK(fs::exists(paths.build_file));

  CHECK(seq::CleanProject(paths, true, out, &error));
  CHECK(!fs::exists(paths.temp / "demo.c"));
  CHECK(!fs::exists(paths.temp / "demo.s"));
  CHECK(!fs::exists(paths.temp / "demo.bin"));
  CHECK(!fs::exists(paths.test / "demo_test.cc"));
  CHECK(!fs::exists(paths.test / "demo_test.bin"));
  CHECK(!fs::exists(paths.build_file));
  CHECK(fs::exists(paths.temp / ".gitignore"));
  CHECK(fs::exists(paths.temp / "Makefile"));
  CHECK(fs::exists(paths.published));
  CHECK_EQ(ReadText(paths.output / "result.txt"), std::string("published"));
  CHECK_EQ(ReadText(paths.input / "data.txt"), std::string("input"));
}

// --- Model -------------------------------------------------------------------

SEQ_TEST(Model_ResolvesReferenceModelToPinnedArtifact) {
  seq::ModelRef model;
  CHECK(seq::ParseModelUrl(seq::kReferenceModelUrl, &model));
  seq::ModelLock lock;
  std::string error;
  CHECK(
      seq::ResolveModelArtifact(model, seq::Config::Defaults(), &lock, &error));
  CHECK_EQ(lock.repo, std::string("Qwen/Qwen2.5-Coder-1.5B-Instruct-GGUF"));
  CHECK_EQ(lock.file, std::string("qwen2.5-coder-1.5b-instruct-q4_k_m.gguf"));
  CHECK_EQ(lock.quantization, std::string("q4_k_m"));
  CHECK_EQ(lock.revision.size(), std::size_t{40});
  CHECK_EQ(lock.sha256.size(), std::size_t{64});
  CHECK_EQ(lock.declared_url, std::string(seq::kReferenceModelUrl));
}

SEQ_TEST(Model_UnknownModelIsAnErrorNotASubstitution) {
  seq::ModelRef model;
  CHECK(
      seq::ParseModelUrl("https://huggingface.co/someone/other-model", &model));
  seq::ModelLock lock;
  std::string error;
  CHECK(!seq::ResolveModelArtifact(model, seq::Config::Defaults(), &lock,
                                   &error));
  CHECK(error.find("no runtime artifact is known") != std::string::npos);

  // A complete user mapping resolves it; an incomplete one does not.
  seq::Config config = seq::Config::Defaults();
  config.Set("model.gguf_repo", "someone/other-model-GGUF");
  config.Set("model.gguf_file", "other.gguf");
  CHECK(!seq::ResolveModelArtifact(model, config, &lock, &error));
  config.Set("model.gguf_revision", "abc123");
  config.Set("model.gguf_sha256", std::string(64, 'a'));
  CHECK(seq::ResolveModelArtifact(model, config, &lock, &error));
  CHECK_EQ(lock.file, std::string("other.gguf"));
  config.Set("model.gguf_file", "../escape.gguf");
  CHECK(!seq::ResolveModelArtifact(model, config, &lock, &error));
}

SEQ_TEST(Model_LockRoundTrip) {
  const TempDir temp;
  seq::ModelRef model;
  CHECK(seq::ParseModelUrl(seq::kReferenceModelUrl, &model));
  seq::ModelLock lock;
  std::string error;
  CHECK(
      seq::ResolveModelArtifact(model, seq::Config::Defaults(), &lock, &error));
  CHECK(seq::WriteModelLock(temp.path() / "seq.lock", lock, &error));
  seq::ModelLock loaded;
  CHECK(seq::ReadModelLock(temp.path() / "seq.lock", &loaded, &error));
  CHECK_EQ(loaded.ToJson().Dump(), lock.ToJson().Dump());
  CHECK(loaded.grammar_version > 0);
  CHECK(loaded.prompt_version > 0);

  WriteText(temp.path() / "bad.lock", "{\"lock_version\": 1}");
  CHECK(!seq::ReadModelLock(temp.path() / "bad.lock", &loaded, &error));
  WriteText(temp.path() / "junk.lock", "not json");
  CHECK(!seq::ReadModelLock(temp.path() / "junk.lock", &loaded, &error));
}

// --- Output parsers ----------------------------------------------------------

SEQ_TEST(Execution_ParsesStepRecords) {
  const auto steps = seq::ParseStepRecords(
      "begin 1 load\nok 1 load\nbegin 2 report\n"
      "message 2 cannot open data.txt: no such file\nfail 2 report 3\n");
  CHECK_EQ(steps.size(), std::size_t{2});
  CHECK_EQ(steps[0].status, std::string("ok"));
  CHECK_EQ(steps[1].name, std::string("report"));
  CHECK_EQ(steps[1].status, std::string("failed"));
  CHECK_EQ(steps[1].code, 3);
  CHECK_EQ(steps[1].message, std::string("cannot open data.txt: no such file"));

  // A step that began and never finished was interrupted.
  const auto interrupted = seq::ParseStepRecords("begin 1 spin\n");
  CHECK_EQ(interrupted.size(), std::size_t{1});
  CHECK_EQ(interrupted[0].status, std::string("running"));
  CHECK(seq::ParseStepRecords("garbage\n\nok\n").empty());
}

SEQ_TEST(Execution_ParsesGoogleTestOutput) {
  const auto cases = seq::ParseGoogleTestOutput(
      "[==========] Running 3 tests from 1 test suite.\n"
      "[ RUN      ] Demo.Passes\n"
      "[       OK ] Demo.Passes (0 ms)\n"
      "[ RUN      ] Demo.Fails\n"
      "x.cc:3: Failure\n"
      "[  FAILED  ] Demo.Fails (1 ms)\n"
      "[ RUN      ] Demo.AlsoPasses\n"
      "[       OK ] Demo.AlsoPasses (0 ms)\n"
      "[  PASSED  ] 2 tests.\n"
      "[  FAILED  ] 1 test, listed below:\n"
      "[  FAILED  ] Demo.Fails\n");
  CHECK_EQ(cases.size(), std::size_t{3});
  CHECK_EQ(cases[0].name, std::string("Demo.Passes"));
  CHECK(cases[0].passed);
  CHECK_EQ(cases[1].name, std::string("Demo.Fails"));
  CHECK(!cases[1].passed);
  CHECK(cases[2].passed);
}
