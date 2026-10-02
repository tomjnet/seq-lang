#ifndef SEQ_BACKEND_HPP_
#define SEQ_BACKEND_HPP_

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "seq/ast.hpp"

namespace seq {

// One hygiene-rule violation in model-written source. These checks keep
// generated code inside the runtime contract; they are not a security
// boundary. The sandbox is.
struct PolicyViolation {
  // 1-based line within the model-written text, or 0 when the violation is
  // about the text as a whole.
  std::size_t line = 0;
  std::string message;
};

// A generated-language backend. It consumes the validated workflow and the
// model-written parts; parsing and validation never depend on it.
class Backend {
 public:
  virtual ~Backend() = default;

  // Name shown to the user, such as "C".
  virtual std::string Name() const = 0;

  // The rules and runtime API the model must follow when writing step
  // functions. Part of the synthesis prompt.
  virtual std::string StepContract(const Workflow& workflow) const = 0;
  // The rules for writing tests of the step functions.
  virtual std::string TestContract(const Workflow& workflow) const = 0;

  // Dependency names a synthesis plan may list.
  virtual std::vector<std::string> AllowedDependencies() const = 0;

  // Wraps model-written step functions in the compiler-owned prelude and
  // main().
  virtual std::string ComposeUnit(const Workflow& workflow,
                                  const std::string& step_functions) const = 0;
  virtual std::vector<PolicyViolation> CheckStepFunctions(
      const Workflow& workflow, const std::string& step_functions) const = 0;

  // Wraps model-written test cases in the compiler-owned test prelude.
  virtual std::string ComposeTest(const Workflow& workflow,
                                  const std::string& test_cases) const = 0;
  virtual std::vector<PolicyViolation> CheckTestCases(
      const std::string& test_cases) const = 0;
};

// Creates the backend for a validated workflow. `runtime_header` is the text
// of the installed seq_runtime.h, which is the single source of the runtime
// API shown to the model.
std::unique_ptr<Backend> CreateBackend(BackendKind kind,
                                       std::string runtime_header);

// Formats violations for the console and for repair prompts.
std::string DescribeViolations(const std::vector<PolicyViolation>& violations,
                               const std::string& source);

}  // namespace seq

#endif  // SEQ_BACKEND_HPP_
