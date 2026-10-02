int seq_step_1(seq_ctx *ctx) {
  if (printf("Hello from Seq\n") < 0) {
    return seq_fail(ctx, "cannot write to standard output");
  }
  return 0;
}
