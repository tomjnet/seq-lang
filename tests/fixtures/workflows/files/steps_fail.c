/* Step 1 writes its file and then fails; step 2 must never run. */
int seq_step_1(seq_ctx *ctx) {
  FILE *out = seq_output_open("numbers.txt", "w");
  if (out != NULL) {
    fprintf(out, "1\n");
    fclose(out);
  }
  printf("step one ran\n");
  return seq_fail(ctx, "disk on fire, code %d", 7);
}

int seq_step_2(seq_ctx *ctx) {
  (void)ctx;
  printf("step two ran\n");
  return 0;
}
