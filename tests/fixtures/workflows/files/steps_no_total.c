/* Succeeds, but never produces the declared total.txt. */
int seq_step_1(seq_ctx *ctx) {
  FILE *out = seq_output_open("numbers.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create numbers.txt");
  }
  fprintf(out, "1\n2\n3\n4\n5\n");
  fclose(out);
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  (void)ctx;
  printf("sum=15\n");
  return 0;
}
