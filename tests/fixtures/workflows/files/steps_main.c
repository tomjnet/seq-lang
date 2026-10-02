int seq_step_1(seq_ctx *ctx) {
  FILE *out = seq_output_open("numbers.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create numbers.txt");
  }
  for (int i = 1; i <= 5; ++i) {
    fprintf(out, "%d\n", i);
  }
  if (fclose(out) != 0) {
    return seq_fail(ctx, "cannot write numbers.txt");
  }
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  FILE *in = seq_output_open("numbers.txt", "r");
  if (in == NULL) {
    return seq_fail(ctx, "cannot open numbers.txt");
  }
  long sum = 0;
  long value = 0;
  while (fscanf(in, "%ld", &value) == 1) {
    sum += value;
  }
  fclose(in);

  FILE *out = seq_output_open("total.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create total.txt");
  }
  fprintf(out, "%ld\n", sum);
  if (fclose(out) != 0) {
    return seq_fail(ctx, "cannot write total.txt");
  }
  printf("sum=%ld\n", sum);
  return 0;
}

int main(void) {
  return 0;
}
