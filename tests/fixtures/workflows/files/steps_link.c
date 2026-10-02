/* Compiles but does not link: it calls a function that exists nowhere. */
int function_that_does_not_exist(void);

int seq_step_1(seq_ctx *ctx) {
  (void)ctx;
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  (void)ctx;
  return function_that_does_not_exist();
}
