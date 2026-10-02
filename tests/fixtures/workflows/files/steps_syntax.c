/* Does not compile: step 2 assigns to a variable that was never declared. */
int seq_step_1(seq_ctx *ctx) {
  (void)ctx;
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  (void)ctx;
  undeclared_variable = 3;
  return 0;
}
