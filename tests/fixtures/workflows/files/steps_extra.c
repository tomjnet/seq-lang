/* Produces both declared outputs and one file the plan did not declare. */
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
  FILE *out = seq_output_open("total.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create total.txt");
  }
  fprintf(out, "15\n");
  fclose(out);

  FILE *notes = seq_output_open("scratch/notes.txt", "w");
  if (notes == NULL) {
    return seq_fail(ctx, "cannot create scratch/notes.txt");
  }
  fprintf(notes, "not declared in the plan\n");
  fclose(notes);
  printf("sum=15\n");
  return 0;
}
