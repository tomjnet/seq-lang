# Examples

Each directory is a Seq project. Building one needs the prerequisites that `seqc doctor` checks: GCC, g++, Google Test, llama.cpp, and the pulled model.

```sh
cd examples/hello
seqc model pull
seqc src/main.seq
```

**These examples have not been built with a real model yet.** Their pass rates are unknown; see [../docs/decisions.md](../docs/decisions.md). The test suite builds equivalent workflows with scripted model responses (`tests/fixtures/workflows/`).

## hello

One step that prints `Hello from Seq`. The same workflow `seqc new` writes.

Expected: the line `Hello from Seq` on standard output and `Results: none`.

## top3Company

Three steps: write five transactions, total them per company, and chart the top three.

The request "revenue" of the original sketch is ambiguous: quantity times price is the value of a transaction, not a company's accounting revenue. This example asks for a *sales total* and defines it in the prompts, together with the file format and the data. That gives a fixed oracle, worked out by hand and independent of whatever program the model writes:

| Company | Transactions | Sales total |
| --- | --- | --- |
| ACME | 10 × 25.50 + 5 × 26.00 | 385.00 |
| Globex | 4 × 120.00 | 480.00 |
| Initech | 30 × 9.75 | 292.50 |
| Umbrella | 2 × 40.00 | 80.00 |

Expected:

- `output/company.txt` with the five lines in the given order.
- Standard output, in order of first appearance: `ACME 385.00`, `Globex 480.00`, `Initech 292.50`, `Umbrella 80.00`.
- `output/top3Company.png`, an 800 by 480 PNG with three bars in this order: Globex 480, ACME 385, Initech 292.5. Ties, of which this data has none, keep the order of first appearance.

`tests/runtime/check_png.py output/top3Company.png 800 480 3` checks the image structure. The bar values are printed on the chart.
