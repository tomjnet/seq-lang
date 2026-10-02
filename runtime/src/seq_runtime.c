/* Step driver, path helpers, and error reporting of the Seq runtime. */

#define _POSIX_C_SOURCE 200809L

#include "seq_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "seq_internal.h"

/* Whether descriptor 3 was a pipe when the program started. Checked once,
 * before any step can open a file that might be given the same number. */
static int g_report_enabled = 0;
/* Directory descriptor for input/, or -1 to resolve inputs by path. */
static int g_input_fd = -1;
static int g_initialized = 0;

static void seq_init(void) {
  struct stat info;

  if (g_initialized) {
    return;
  }
  g_initialized = 1;
  if (fstat(SEQ_REPORT_FD, &info) == 0 && S_ISFIFO(info.st_mode)) {
    g_report_enabled = 1;
  }
  if (fstat(SEQ_INPUT_FD, &info) == 0 && S_ISDIR(info.st_mode)) {
    g_input_fd = SEQ_INPUT_FD;
  }
}

static void seq_report(const char* line) {
  size_t length;
  size_t written = 0;

  if (!g_report_enabled) {
    return;
  }
  length = strlen(line);
  while (written < length) {
    ssize_t n = write(SEQ_REPORT_FD, line + written, length - written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    written += (size_t)n;
  }
}

int seq_path_is_safe(const char* relpath) {
  const char* p;
  const char* component;

  if (relpath == NULL || relpath[0] == '\0' || relpath[0] == '/') {
    return 0;
  }
  if (strlen(relpath) > 255) {
    return 0;
  }
  component = relpath;
  for (p = relpath;; ++p) {
    if (*p == '\\') {
      return 0;
    }
    if (*p == '/' || *p == '\0') {
      size_t length = (size_t)(p - component);
      if (length == 0) {
        return 0;
      }
      if (length == 1 && component[0] == '.') {
        return 0;
      }
      if (length == 2 && component[0] == '.' && component[1] == '.') {
        return 0;
      }
      if (*p == '\0') {
        break;
      }
      component = p + 1;
    }
  }
  return 1;
}

/* Creates the parent directories of relpath, which must be safe. */
static int seq_make_parents(const char* relpath) {
  char buffer[256];
  size_t length = strlen(relpath);
  size_t i;

  if (length >= sizeof(buffer)) {
    return -1;
  }
  memcpy(buffer, relpath, length + 1);
  for (i = 1; i < length; ++i) {
    if (buffer[i] != '/') {
      continue;
    }
    buffer[i] = '\0';
    if (mkdir(buffer, 0755) != 0 && errno != EEXIST) {
      return -1;
    }
    buffer[i] = '/';
  }
  return 0;
}

FILE* seq_output_open(const char* relpath, const char* mode) {
  seq_init();
  if (mode == NULL || !seq_path_is_safe(relpath)) {
    errno = EINVAL;
    return NULL;
  }
  if (mode[0] == 'w' || mode[0] == 'a') {
    if (seq_make_parents(relpath) != 0) {
      return NULL;
    }
  }
  return fopen(relpath, mode);
}

FILE* seq_input_open(const char* relpath) {
  int fd;
  FILE* file;

  seq_init();
  if (!seq_path_is_safe(relpath)) {
    errno = EINVAL;
    return NULL;
  }
  if (g_input_fd >= 0) {
    fd = openat(g_input_fd, relpath, O_RDONLY | O_CLOEXEC);
  } else {
    /* Run by hand, outside seqc: inputs come from SEQ_INPUT_DIR or ./input. */
    char path[1024];
    const char* base = getenv("SEQ_INPUT_DIR");
    if (base == NULL || base[0] == '\0') {
      base = "input";
    }
    if (snprintf(path, sizeof(path), "%s/%s", base, relpath) >=
        (int)sizeof(path)) {
      errno = ENAMETOOLONG;
      return NULL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    return NULL;
  }
  file = fdopen(fd, "rb");
  if (file == NULL) {
    close(fd);
  }
  return file;
}

int seq_fail(seq_ctx* ctx, const char* fmt, ...) {
  va_list args;

  if (ctx == NULL || fmt == NULL) {
    return 1;
  }
  va_start(args, fmt);
  vsnprintf(ctx->message, sizeof(ctx->message), fmt, args);
  va_end(args);
  return 1;
}

void seq_ctx_init(seq_ctx* ctx) {
  seq_init();
  if (ctx == NULL) {
    return;
  }
  ctx->step_index = 0;
  ctx->step_name = "";
  ctx->message[0] = '\0';
}

int seq_run(const seq_step_entry* steps, int count) {
  char line[SEQ_MESSAGE_MAX + 128];
  seq_ctx ctx;
  int i;

  seq_init();
  for (i = 0; i < count; ++i) {
    int rc;
    char* p;

    seq_ctx_init(&ctx);
    ctx.step_index = i + 1;
    ctx.step_name = steps[i].name;

    snprintf(line, sizeof(line), "begin %d %s\n", ctx.step_index,
             ctx.step_name);
    seq_report(line);

    rc = steps[i].fn(&ctx);
    fflush(stdout);
    fflush(stderr);

    if (rc == 0) {
      snprintf(line, sizeof(line), "ok %d %s\n", ctx.step_index, ctx.step_name);
      seq_report(line);
      continue;
    }

    /* One record per line: a message must not contain a newline. */
    for (p = ctx.message; *p != '\0'; ++p) {
      if (*p == '\n' || *p == '\r') {
        *p = ' ';
      }
    }
    if (ctx.message[0] != '\0') {
      snprintf(line, sizeof(line), "message %d %s\n", ctx.step_index,
               ctx.message);
      seq_report(line);
    }
    snprintf(line, sizeof(line), "fail %d %s %d\n", ctx.step_index,
             ctx.step_name, rc);
    seq_report(line);
    if (!g_report_enabled) {
      fprintf(stderr, "seq: step %d (%s) failed with code %d%s%s\n",
              ctx.step_index, ctx.step_name, rc,
              ctx.message[0] != '\0' ? ": " : "", ctx.message);
    }
    return SEQ_EXIT_STEP_FAILED;
  }
  return 0;
}
