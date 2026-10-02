#ifndef SEQ_PLANNER_PLANNER_HPP_
#define SEQ_PLANNER_PLANNER_HPP_

#include <string>
#include <vector>

#include "project/project.hpp"
#include "seq/ast.hpp"
#include "seq/backend.hpp"
#include "seq/model.hpp"
#include "support/json.hpp"

namespace seq {

struct PlanStep {
  int index = 0;
  std::string name;
  std::string summary;
  std::vector<std::string> reads;
  std::vector<std::string> writes;
};

struct PlanOutput {
  // Relative to output/.
  std::string path;
  // "text", "png", or "binary".
  std::string kind;
  // A result the user asked for, as opposed to an intermediate file.
  bool final_artifact = false;
};

// The structured whole-workflow plan (plan.json).
struct Plan {
  std::vector<PlanStep> steps;
  std::vector<PlanOutput> outputs;
  std::vector<std::string> dependencies;

  Json ToJson() const;
};

enum class PlanError {
  kNone,
  // The response is not a plan: malformed JSON, wrong shape, or a step that
  // is missing, duplicated, or out of order.
  kInvalid,
  // The plan asks for a library outside the allowlist. Not repairable.
  kUnsupportedDependency,
};

// Parses and validates a planning response against the workflow.
PlanError ParsePlan(const std::string& content, const Workflow& workflow,
                    const std::vector<std::string>& allowed_dependencies,
                    Plan* plan, std::string* error);

// Reads a plan back from a build manifest. Only shape is checked.
bool PlanFromJson(const Json& json, Plan* plan);

// Requests. Every one carries the entire ordered workflow and the approved
// input context, so later steps can rely on what earlier steps create.
InferenceRequest MakePlanRequest(const Workflow& workflow,
                                 const InputManifest& inputs,
                                 const Backend& backend);
InferenceRequest MakeGenerateRequest(const Workflow& workflow,
                                     const InputManifest& inputs,
                                     const Plan& plan, const Backend& backend);
InferenceRequest MakeRepairRequest(const Workflow& workflow,
                                   const InputManifest& inputs,
                                   const Plan& plan, const Backend& backend,
                                   const std::string& previous_source,
                                   const std::string& problems);
InferenceRequest MakeTestRequest(const Workflow& workflow, const Plan& plan,
                                 const Backend& backend,
                                 const std::string& step_functions);
InferenceRequest MakeTestRepairRequest(const Workflow& workflow,
                                       const Plan& plan, const Backend& backend,
                                       const std::string& step_functions,
                                       const std::string& previous_tests,
                                       const std::string& problems);

// Response fields that carry source code.
inline constexpr char kStepFunctionsField[] = "step_functions";
inline constexpr char kTestCasesField[] = "test_cases";

// Extracts the single string field of a generation response. Fails on
// malformed JSON, a missing or non-string field, or an empty value.
bool ExtractSourceField(const std::string& content, const char* field,
                        std::string* source, std::string* error);

}  // namespace seq

#endif  // SEQ_PLANNER_PLANNER_HPP_
