/* Step 2 never returns. */
int seq_step_1(seq_ctx *ctx) {
  (void)ctx;
  printf("before the loop\n");
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  (void)ctx;
  volatile unsigned long counter = 0;
  for (;;) {
    ++counter;
  }
  return 0;
}
