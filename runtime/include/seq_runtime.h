/* Seq runtime contract, version 1.
 *
 * A generated program is one C17 translation unit. It defines one function
 * per workflow step, named by 1-based source order:
 *
 *   int seq_step_1(seq_ctx *ctx);   returns 0 on success, nonzero on failure
 *
 * The compiler supplies main(); the program must not define it. The program
 * runs with the output staging directory as its working directory, with no
 * arguments, no network, and no ability to start other processes. Steps
 * share data through file-scope static variables or through files.
 */
#ifndef SEQ_RUNTIME_H_
#define SEQ_RUNTIME_H_

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SEQ_RUNTIME_VERSION 1

/* Exit status of the program when a step fails. */
#define SEQ_EXIT_STEP_FAILED 70

#define SEQ_MESSAGE_MAX 256

/* Per-step context. Step functions pass it to seq_fail(). */
typedef struct seq_ctx {
  int step_index;        /* 1-based index of the running step */
  const char* step_name; /* step name from the workflow source */
  char message[SEQ_MESSAGE_MAX];
} seq_ctx;

/* Opens an output file. relpath is relative to the output directory; an
 * absolute path or a ".." component is rejected. Missing parent directories
 * are created. mode is an fopen() mode. Returns NULL on failure. */
FILE* seq_output_open(const char* relpath, const char* mode);

/* Opens a file from the project's input/ directory for reading. Inputs are
 * read-only. Returns NULL on failure. */
FILE* seq_input_open(const char* relpath);

/* Records a printf-style error message for the current step and returns 1,
 * so a step can fail with:  return seq_fail(ctx, "cannot open %s", name); */
int seq_fail(seq_ctx* ctx, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Renders a labelled bar chart as a PNG image at relpath (same path rules as
 * seq_output_open). labels[i] names values[i]; count is 1 to 32. Returns 0 on
 * success and nonzero on failure. */
int seq_chart_bar_png(const char* relpath, const char* title,
                      const char* const* labels, const double* values,
                      int count);

/* ---- Used by compiler-owned code and by tests, not by step functions. ---- */

typedef int (*seq_step_fn)(seq_ctx* ctx);

typedef struct seq_step_entry {
  const char* name;
  seq_step_fn fn;
} seq_step_entry;

/* Prepares a context for calling a step function directly, as tests do. */
void seq_ctx_init(seq_ctx* ctx);

/* Runs the steps in order, reports each one, and stops at the first failure.
 * Returns the process exit status. */
int seq_run(const seq_step_entry* steps, int count);

#ifdef __cplusplus
}
#endif

#endif /* SEQ_RUNTIME_H_ */
