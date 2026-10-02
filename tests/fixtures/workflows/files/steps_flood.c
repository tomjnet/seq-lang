/* Step 2 prints without end. */
int seq_step_1(seq_ctx *ctx) {
  (void)ctx;
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  (void)ctx;
  for (;;) {
    printf("flooding the console with output, line after line after line\n");
  }
  return 0;
}
