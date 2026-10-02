/* Tests of the Seq runtime library, written directly in C with no model
 * involved.
 *
 *   runtime_test supervised <workdir>   behaves like a program started by
 *                                       seqc: a pipe on descriptor 3 and the
 *                                       input directory on descriptor 4
 *   runtime_test manual <workdir>       behaves like a program run by hand:
 *                                       neither descriptor is provided
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "seq_runtime.h"

static int g_failures = 0;

#define CHECK(condition)                                           \
  do {                                                             \
    if (!(condition)) {                                            \
      ++g_failures;                                                \
      fprintf(stderr, "%s:%d: expected: %s\n", __FILE__, __LINE__, \
              #condition);                                         \
    }                                                              \
  } while (0)

static int g_calls[4];

static int step_first(seq_ctx* ctx) {
  g_calls[0]++;
  return ctx->step_index == 1 && strcmp(ctx->step_name, "first") == 0 ? 0 : 9;
}

static int step_second(seq_ctx* ctx) {
  (void)ctx;
  g_calls[1]++;
  return 0;
}

static int step_third(seq_ctx* ctx) {
  g_calls[2]++;
  /* The newline must not split the record. */
  return seq_fail(ctx, "bad thing %d\nsecond line", 7);
}

static int step_never(seq_ctx* ctx) {
  (void)ctx;
  g_calls[3]++;
  return 0;
}

static void write_file(const char* path, const char* text) {
  FILE* f = fopen(path, "wb");
  if (f == NULL) {
    perror(path);
    exit(2);
  }
  fputs(text, f);
  fclose(f);
}

static void read_all(FILE* f, char* buffer, size_t size) {
  size_t n = fread(buffer, 1, size - 1, f);
  buffer[n] = '\0';
}

static void test_path_helpers(void) {
  static const char* const kBad[] = {"/absolute", "../up", "a/../b", "",
                                     "a//b",      "./a",   "a/.",    "a\\b",
                                     "dir/",      ".."};
  FILE* f;
  char text[64];
  size_t i;

  for (i = 0; i < sizeof(kBad) / sizeof(kBad[0]); ++i) {
    errno = 0;
    CHECK(seq_output_open(kBad[i], "w") == NULL);
    CHECK(seq_input_open(kBad[i]) == NULL);
  }
  CHECK(seq_output_open(NULL, "w") == NULL);
  CHECK(seq_output_open("x.txt", NULL) == NULL);
  CHECK(seq_input_open(NULL) == NULL);

  /* Missing parent directories are created for writing. */
  f = seq_output_open("sub/dir/file.txt", "w");
  CHECK(f != NULL);
  if (f != NULL) {
    fputs("written", f);
    fclose(f);
  }
  f = seq_output_open("sub/dir/file.txt", "r");
  CHECK(f != NULL);
  if (f != NULL) {
    read_all(f, text, sizeof(text));
    fclose(f);
    CHECK(strcmp(text, "written") == 0);
  }
  /* Reading a missing file does not create anything. */
  CHECK(seq_output_open("nowhere/missing.txt", "r") == NULL);
  CHECK(access("nowhere", F_OK) != 0);

  /* Inputs open read-only. */
  f = seq_input_open("data/numbers.txt");
  CHECK(f != NULL);
  if (f != NULL) {
    read_all(f, text, sizeof(text));
    CHECK(strcmp(text, "1 2 3\n") == 0);
    CHECK(fputs("x", f) == EOF || fflush(f) == EOF);
    fclose(f);
  }
  CHECK(seq_input_open("missing.txt") == NULL);
}

static void test_fail_helper(void) {
  seq_ctx ctx;
  char longer[2 * SEQ_MESSAGE_MAX];

  seq_ctx_init(&ctx);
  CHECK(ctx.message[0] == '\0');
  CHECK(seq_fail(&ctx, "value %d and %s", 42, "text") == 1);
  CHECK(strcmp(ctx.message, "value 42 and text") == 0);

  memset(longer, 'x', sizeof(longer) - 1);
  longer[sizeof(longer) - 1] = '\0';
  CHECK(seq_fail(&ctx, "%s", longer) == 1);
  CHECK(strlen(ctx.message) == SEQ_MESSAGE_MAX - 1);
  CHECK(seq_fail(NULL, "ignored") == 1);
}

static uint32_t read_be32(const unsigned char* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t crc32_of(const unsigned char* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFU;
  size_t i;
  int k;

  for (i = 0; i < size; ++i) {
    crc ^= data[i];
    for (k = 0; k < 8; ++k) {
      crc = (crc & 1U) ? 0xEDB88320U ^ (crc >> 1) : crc >> 1;
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

/* Checks the PNG container: signature, chunk order, chunk checksums, and the
 * header fields. The compressed data is checked by check_png.py. */
static void check_png_structure(const char* path) {
  static const unsigned char kSignature[8] = {0x89, 'P',  'N',  'G',
                                              '\r', '\n', 0x1A, '\n'};
  unsigned char* data;
  long size;
  size_t pos = 8;
  int chunk = 0;
  int saw_plte = 0;
  int saw_idat = 0;
  int saw_iend = 0;
  FILE* f = fopen(path, "rb");

  CHECK(f != NULL);
  if (f == NULL) {
    return;
  }
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  data = (unsigned char*)malloc((size_t)size);
  CHECK(data != NULL && fread(data, 1, (size_t)size, f) == (size_t)size);
  fclose(f);

  CHECK(size > 64 && memcmp(data, kSignature, 8) == 0);
  /* A chart is mostly flat color, so the file must be far smaller than the
   * 384 KiB of raw pixels. */
  CHECK(size < 64 * 1024);
  while (pos + 12 <= (size_t)size) {
    uint32_t length = read_be32(data + pos);
    const unsigned char* type = data + pos + 4;
    if (pos + 12 + length > (size_t)size) {
      CHECK(!"chunk runs past the end of the file");
      break;
    }
    CHECK(crc32_of(type, 4 + (size_t)length) ==
          read_be32(data + pos + 8 + length));
    if (chunk == 0) {
      CHECK(memcmp(type, "IHDR", 4) == 0 && length == 13);
      CHECK(read_be32(type + 4) == 800);
      CHECK(read_be32(type + 8) == 480);
      CHECK(type[12] == 8); /* bit depth */
      CHECK(type[13] == 3); /* palette color */
      CHECK(type[14] == 0 && type[15] == 0 && type[16] == 0);
    } else if (memcmp(type, "PLTE", 4) == 0) {
      CHECK(!saw_idat && length % 3 == 0 && length >= 3 * 5);
      saw_plte = 1;
    } else if (memcmp(type, "IDAT", 4) == 0) {
      CHECK(saw_plte && length > 6);
      saw_idat = 1;
    } else if (memcmp(type, "IEND", 4) == 0) {
      CHECK(length == 0);
      saw_iend = 1;
      CHECK(pos + 12 == (size_t)size);
    }
    pos += 12 + (size_t)length;
    ++chunk;
  }
  CHECK(saw_plte && saw_idat && saw_iend);
  free(data);
}

static void test_chart(void) {
  static const char* const kLabels[] = {"ACME", "Globex", "Initech"};
  static const double kValues[] = {1500.5, 980.0, 2210.25};
  static const double kMixed[] = {-40.0, 0.0, 125.0};
  static const double kNotFinite[] = {1.0, NAN, 3.0};
  static const char* const kLong[] = {
      "a very long company name that cannot possibly fit", "b", "caf\xC3\xA9"};
  double many[33];
  const char* many_labels[33];
  int i;

  CHECK(seq_chart_bar_png("charts/top.png", "Top 3 companies by revenue",
                          kLabels, kValues, 3) == 0);
  check_png_structure("charts/top.png");

  /* Negative values, a single bar, long and non-ASCII labels, no title. */
  CHECK(seq_chart_bar_png("mixed.png", NULL, kLabels, kMixed, 3) == 0);
  check_png_structure("mixed.png");
  CHECK(seq_chart_bar_png("one.png", "", kLabels, kValues, 1) == 0);
  check_png_structure("one.png");
  CHECK(seq_chart_bar_png("long.png", "t", kLong, kValues, 3) == 0);
  check_png_structure("long.png");

  for (i = 0; i < 33; ++i) {
    many[i] = (double)(i + 1);
    many_labels[i] = "x";
  }
  CHECK(seq_chart_bar_png("many.png", "32 bars", many_labels, many, 32) == 0);
  check_png_structure("many.png");

  /* Rejected requests leave no file behind. */
  CHECK(seq_chart_bar_png("bad1.png", "t", many_labels, many, 33) != 0);
  CHECK(seq_chart_bar_png("bad2.png", "t", kLabels, kValues, 0) != 0);
  CHECK(seq_chart_bar_png("bad3.png", "t", kLabels, kNotFinite, 3) != 0);
  CHECK(seq_chart_bar_png("bad4.png", "t", NULL, kValues, 3) != 0);
  CHECK(seq_chart_bar_png("bad5.png", "t", kLabels, NULL, 3) != 0);
  CHECK(seq_chart_bar_png("../escape.png", "t", kLabels, kValues, 3) != 0);
  CHECK(seq_chart_bar_png("/tmp/abs.png", "t", kLabels, kValues, 3) != 0);
  CHECK(access("bad1.png", F_OK) != 0);
  CHECK(access("bad3.png", F_OK) != 0);
}

int main(int argc, char* argv[]) {
  static const seq_step_entry kSteps[] = {
      {"first", step_first},
      {"second", step_second},
      {"third", step_third},
      {"never", step_never},
  };
  int supervised;
  int report_read = -1;
  char input_dir[1024];
  char report[1024];

  if (argc != 3 ||
      (strcmp(argv[1], "supervised") != 0 && strcmp(argv[1], "manual") != 0)) {
    fprintf(stderr, "usage: runtime_test supervised|manual <workdir>\n");
    return 2;
  }
  supervised = strcmp(argv[1], "supervised") == 0;

  if (mkdir(argv[2], 0700) != 0 && errno != EEXIST) {
    perror(argv[2]);
    return 2;
  }
  if (chdir(argv[2]) != 0) {
    perror(argv[2]);
    return 2;
  }
  mkdir("input-root", 0700);
  mkdir("input-root/data", 0700);
  write_file("input-root/data/numbers.txt", "1 2 3\n");
  mkdir("out", 0700);
  if (getcwd(input_dir, sizeof(input_dir) - 16) == NULL) {
    return 2;
  }
  strcat(input_dir, "/input-root");

  /* Descriptors 3 and 4 must be arranged before the first runtime call: the
   * runtime looks at them once. */
  close(3);
  close(4);
  if (supervised) {
    int ends[2];
    int dir;
    if (pipe(ends) != 0) {
      return 2;
    }
    report_read = fcntl(ends[0], F_DUPFD, 10);
    dir = open(input_dir, O_RDONLY | O_DIRECTORY);
    dir = fcntl(dir, F_DUPFD, 11);
    if (dup2(ends[1], 20) < 0) {
      return 2;
    }
    close(3);
    close(4);
    close(ends[0]);
    close(ends[1]);
    dup2(20, 3);
    close(20);
    dup2(dir, 4);
    close(dir);
    fcntl(report_read, F_SETFL, O_NONBLOCK);
  } else {
    setenv("SEQ_INPUT_DIR", input_dir, 1);
  }
  if (chdir("out") != 0) {
    return 2;
  }

  /* Step ordering and stop-on-failure. */
  CHECK(seq_run(kSteps, 4) == SEQ_EXIT_STEP_FAILED);
  CHECK(g_calls[0] == 1 && g_calls[1] == 1 && g_calls[2] == 1);
  CHECK(g_calls[3] == 0);
  CHECK(seq_run(kSteps, 2) == 0);
  CHECK(seq_run(kSteps, 0) == 0);

  if (supervised) {
    ssize_t n = read(report_read, report, sizeof(report) - 1);
    report[n > 0 ? n : 0] = '\0';
    CHECK(strcmp(report,
                 "begin 1 first\nok 1 first\n"
                 "begin 2 second\nok 2 second\n"
                 "begin 3 third\n"
                 "message 3 bad thing 7 second line\n"
                 "fail 3 third 1\n"
                 "begin 1 first\nok 1 first\n"
                 "begin 2 second\nok 2 second\n") == 0);
    if (g_failures != 0) {
      fprintf(stderr, "report was:\n%s", report);
    }
  }

  test_path_helpers();
  test_fail_helper();
  test_chart();

  if (!supervised) {
    /* Run by hand, descriptor 3 was closed at startup, so a file the program
     * opens can be given that number. The runtime must not write step
     * records into it. */
    struct stat info;
    int fd;
    close(3);
    fd = open("first-file.txt", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd == 3);
    CHECK(seq_run(kSteps, 2) == 0);
    CHECK(fstat(fd, &info) == 0 && info.st_size == 0);
    close(fd);
  }

  if (g_failures != 0) {
    fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  printf("runtime tests passed (%s)\n", argv[1]);
  return 0;
}
