/* Tries to reach outside its sandbox with plain C library calls, which the
 * hygiene checks do not and cannot catch. Every attempt must be denied by
 * the kernel. The program reports what happened and still succeeds, so the
 * test can read the report. */
static void try_read(const char *path) {
  FILE *f = fopen(path, "r");
  printf("read %s: %s\n", path, f != NULL ? "ALLOWED" : "denied");
  if (f != NULL) {
    fclose(f);
  }
}

static void try_write(const char *path) {
  FILE *f = fopen(path, "a");
  printf("write %s: %s\n", path, f != NULL ? "ALLOWED" : "denied");
  if (f != NULL) {
    fclose(f);
  }
}

int seq_step_1(seq_ctx *ctx) {
  (void)ctx;
  /* Unrelated host files. */
  try_read("/etc/passwd");
  try_read("/proc/self/environ");
  /* The project's own source and the compiler's artifacts. The working
   * directory is output/temp/runs/<run>/staging. */
  try_read("../../../../../src/main.seq");
  try_read("../../../escape.c");
  try_write("../../../escape.c");
  try_write("../../../../planted.txt");
  try_write("/tmp/seq-integration-escape.txt");
  /* Inputs are readable but not writable. */
  try_write("../../../../../input/data.txt");
  try_write("../../../../../input/planted.txt");
  /* The path helpers refuse these before the kernel is even asked. */
  printf("helper ../x: %s\n",
         seq_output_open("../x", "w") != NULL ? "ALLOWED" : "denied");
  printf("helper /abs: %s\n",
         seq_output_open("/tmp/abs", "w") != NULL ? "ALLOWED" : "denied");

  FILE *out = seq_output_open("numbers.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create numbers.txt");
  }
  fprintf(out, "1\n");
  fclose(out);
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  /* The input itself is still readable through the runtime. */
  FILE *in = seq_input_open("data.txt");
  char line[64] = "";
  if (in == NULL || fgets(line, sizeof(line), in) == NULL) {
    return seq_fail(ctx, "cannot read the input file");
  }
  fclose(in);
  printf("input: %s", line);

  FILE *out = seq_output_open("total.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create total.txt");
  }
  fprintf(out, "1\n");
  fclose(out);
  return 0;
}
