#define MAX_COMPANIES 16

typedef struct {
  char name[32];
  double total;
} company_total;

static company_total totals[MAX_COMPANIES];
static int total_count = 0;

int seq_step_1(seq_ctx *ctx) {
  FILE *out = seq_output_open("company.txt", "w");
  if (out == NULL) {
    return seq_fail(ctx, "cannot create company.txt");
  }
  fprintf(out, "ACME,10,25.50\n");
  fprintf(out, "Globex,4,120.00\n");
  fprintf(out, "Initech,30,9.75\n");
  fprintf(out, "ACME,5,26.00\n");
  fprintf(out, "Umbrella,2,40.00\n");
  if (fclose(out) != 0) {
    return seq_fail(ctx, "cannot write company.txt");
  }
  return 0;
}

int seq_step_2(seq_ctx *ctx) {
  FILE *in = seq_output_open("company.txt", "r");
  if (in == NULL) {
    return seq_fail(ctx, "cannot open company.txt");
  }
  char name[32];
  double quantity = 0.0;
  double price = 0.0;
  total_count = 0;
  while (fscanf(in, " %31[^,],%lf,%lf", name, &quantity, &price) == 3) {
    int index = -1;
    for (int i = 0; i < total_count; ++i) {
      if (strcmp(totals[i].name, name) == 0) {
        index = i;
      }
    }
    if (index < 0) {
      if (total_count == MAX_COMPANIES) {
        fclose(in);
        return seq_fail(ctx, "too many companies");
      }
      index = total_count++;
      strcpy(totals[index].name, name);
      totals[index].total = 0.0;
    }
    totals[index].total += quantity * price;
  }
  fclose(in);
  for (int i = 0; i < total_count; ++i) {
    printf("%s %.2f\n", totals[i].name, totals[i].total);
  }
  return 0;
}

int seq_step_3(seq_ctx *ctx) {
  /* Selection sort, largest first; ties keep their first-seen order. */
  for (int i = 0; i < total_count; ++i) {
    int best = i;
    for (int j = i + 1; j < total_count; ++j) {
      if (totals[j].total > totals[best].total) {
        best = j;
      }
    }
    company_total moved = totals[best];
    for (int j = best; j > i; --j) {
      totals[j] = totals[j - 1];
    }
    totals[i] = moved;
  }
  int shown = total_count < 3 ? total_count : 3;
  const char *labels[3];
  double values[3];
  for (int i = 0; i < shown; ++i) {
    labels[i] = totals[i].name;
    values[i] = totals[i].total;
  }
  if (seq_chart_bar_png("top3.png", "Top 3 companies by sales total", labels,
                        values, shown) != 0) {
    return seq_fail(ctx, "cannot draw top3.png");
  }
  return 0;
}
