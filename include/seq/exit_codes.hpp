#ifndef SEQ_EXIT_CODES_HPP_
#define SEQ_EXIT_CODES_HPP_

namespace seq {

// Stable exit statuses of seqc. docs/language.md documents the same table.
inline constexpr int kExitOk = 0;
// An unexpected failure inside seqc itself.
inline constexpr int kExitInternal = 1;
// Bad command line.
inline constexpr int kExitUsage = 2;
// The source file is not a valid workflow.
inline constexpr int kExitSource = 3;
// The model could not be resolved or reached, or its response was unusable.
inline constexpr int kExitModel = 4;
// The generated program was rejected, or did not compile within the repair
// limit.
inline constexpr int kExitCompile = 5;
// The generated program failed, hit a limit, or produced invalid outputs.
inline constexpr int kExitExecution = 6;
// A filesystem or security rule stopped the build: bad project layout,
// rejected input, output collision, busy project, or unavailable isolation.
inline constexpr int kExitFilesystem = 7;
// A prerequisite is missing: GCC, g++, Google Test, the runtime, or curl.
inline constexpr int kExitPrerequisite = 8;

}  // namespace seq

#endif  // SEQ_EXIT_CODES_HPP_
