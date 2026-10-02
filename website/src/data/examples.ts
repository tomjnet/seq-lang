// The sessions that "Try Seq" replays. seqc needs GCC and a local model, so
// it cannot run in a browser. The transcripts follow seqc's real console
// format (src/cli/build.cpp); timings, sizes and identifiers are illustrative.

export interface Line {
  text: string;
  // Pause before this line appears, in milliseconds.
  wait?: number;
}

export interface Example {
  id: string;
  label: string;
  blurb: string;
  source: string;
  // The first run, and the run that follows when nothing changed.
  run: Line[];
  rerun?: Line[];
  exit: number;
}

const MODEL = 'model("https://huggingface.co/Qwen/Qwen2.5-Coder-1.5B-Instruct")';
const ISOLATED = '(isolated: no network, no processes, writes only to staging)';

const lines = (text: string): Line[] => text.split('\n').map((line) => ({ text: line }));
const slow = (text: string, wait: number): Line => ({ text, wait });

const artifacts = (name: string, run: string): Line[] =>
  lines(`
Build artifacts:
  output/temp/${name}.c
  output/temp/${name}.s
  output/temp/${name}.bin
  output/temp/test/${name}_test.cc
  output/temp/test/${name}_test.bin
Run records:
  output/temp/runs/${run}/`);

const helloOutput = lines(`---- program output ----
Hello from Seq
---- end of program output ----
[run]      step 1 (step1): ok

Results: none (the workflow printed its result to the console)`);

const chartOutput = lines(`---- program output ----
ACME 385.00
Globex 480.00
Initech 292.50
Umbrella 80.00
---- end of program output ----
[run]      step 1 (step1): ok
[run]      step 2 (step2): ok
[run]      step 3 (step3): ok

Results:
  output/company.txt  (76 bytes)
  output/top3Company.png  (4.1 KiB)  final`);

export const EXAMPLES: Example[] = [
  {
    id: 'hello',
    label: 'Hello, Seq!',
    blurb: 'One step, one request. The workflow that seqc new writes.',
    source: `${MODEL}
backend.C()
name = "hello"

step step1():
    ask("print Hello from Seq")
`,
    run: [
      ...lines(`Seq Compiler

[check]    src/main.seq: 1 step(s), 1 request(s), backend C
[inputs]   0 file(s), 0 bytes
[cache]    building: no accepted build yet
[plan]     requesting a plan for 1 step(s)
[model]    starting llama-server`),
      slow('[plan]     accepted: 1 step(s), 0 declared output(s), 0 final', 700),
      { text: '[generate] attempt 1 of 3' },
      slow('[policy]   ok', 800),
      slow('[compile]  ok (0.3 s, -O3)', 250),
      { text: '[link]     ok (built from the emitted assembly)' },
      { text: '[test]     generating tests, attempt 1 of 3' },
      slow('[test]     built hello_test.bin (1.8 s)', 700),
      ...lines(`[test]     Hello.StepSucceeds: passed
[test]     1 passed, 0 failed (model-written tests are reported, not enforced)
[build]    accepted
[run]      output/temp/hello.bin ${ISOLATED}`),
      ...helloOutput,
      ...artifacts('hello', '20261001T091542.118Z-3fa9c1'),
    ],
    rerun: [
      ...lines(`Seq Compiler

[check]    src/main.seq: 1 step(s), 1 request(s), backend C
[inputs]   0 file(s), 0 bytes
[cache]    reusing the accepted build 5d02b7c9e41a (no inference, no compilation)
[test]     recorded results: 1 passed, 0 failed
[run]      output/temp/hello.bin ${ISOLATED}`),
      ...helloOutput,
      ...artifacts('hello', '20261001T091611.402Z-b81e07'),
    ],
    exit: 0,
  },
  {
    id: 'top3',
    label: 'Top 3 companies',
    blurb: 'Three steps: write a data file, total it per company, chart the top three.',
    source: `${MODEL}
backend.C()
name = "top3Company"

step step1():
    ask("create a database file company.txt with exactly these 5 stock transactions, one per line in the format company,quantity,price")
    ask("the lines are: ACME,10,25.50 then Globex,4,120.00 then Initech,30,9.75 then ACME,5,26.00 then Umbrella,2,40.00")

step step2():
    ask("read company.txt and for each company compute its sales total, the sum of quantity times price over its transactions")
    ask("print one line per company in the order they first appear, as: company total with two decimals")

step step3():
    ask("create an image chart top3Company.png showing the 3 companies with the highest sales total, highest first")
    ask("if two companies have the same total, the one that appears first in company.txt comes first")
`,
    run: [
      ...lines(`Seq Compiler

[check]    src/main.seq: 3 step(s), 6 request(s), backend C
[inputs]   0 file(s), 0 bytes
[cache]    building: no accepted build yet
[plan]     requesting a plan for 3 step(s)
[model]    starting llama-server`),
      slow('[plan]     accepted: 3 step(s), 2 declared output(s), 1 final', 800),
      { text: '[generate] attempt 1 of 3' },
      slow('[policy]   ok', 1000),
      slow('[compile]  ok (0.6 s, -O3)', 300),
      { text: '[link]     ok (built from the emitted assembly)' },
      { text: '[test]     generating tests, attempt 1 of 3' },
      slow('[test]     built top3Company_test.bin (2.1 s)', 800),
      ...lines(`[test]     Chart.AllStepsSucceed: passed
[test]     Chart.WritesFiveTransactions: passed
[test]     Chart.ProducesPng: passed
[test]     3 passed, 0 failed (model-written tests are reported, not enforced)
[build]    accepted
[run]      output/temp/top3Company.bin ${ISOLATED}`),
      ...chartOutput,
      ...artifacts('top3Company', '20261001T091727.560Z-c40d2e'),
    ],
    rerun: [
      ...lines(`Seq Compiler

[check]    src/main.seq: 3 step(s), 6 request(s), backend C
[inputs]   0 file(s), 0 bytes
[cache]    reusing the accepted build 9c41e07ab35d (no inference, no compilation)
[test]     recorded results: 3 passed, 0 failed
[run]      output/temp/top3Company.bin ${ISOLATED}`),
      ...chartOutput,
      ...artifacts('top3Company', '20261001T091803.227Z-17aa5b'),
    ],
    exit: 0,
  },
  {
    id: 'mistake',
    label: 'A mistake, caught',
    blurb: 'Every error is reported with its line and column, before any model access.',
    source: `${MODEL}
backend.C()
name = "hello"

step step1():
    ask "print Hello from Seq"
`,
    run: lines(`Seq Compiler

src/main.seq:6:9: error[E0204]: ask requires parentheses
        ask "print Hello from Seq"
            ^
    hint: write ask("...")
seqc: 1 error(s) in src/main.seq; nothing was built`),
    exit: 3,
  },
];

// The workflow on the hero, in the short form docs/language.md opens with.
export const HERO_SOURCE = `${MODEL}
backend.C()
name = "top3Company"

step step1():
    ask("create a database file company.txt with 5 sample stock transactions")

step step2():
    ask("for each transaction created give me the total revenue of the company")

step step3():
    ask("create an image chart showing the 3 companies with the highest revenue")
`;

// An abridged build of it: the stages that fit a small window, in order.
export const HERO_SESSION: Line[] = [
  { text: '[check]    src/main.seq: 3 step(s), 3 request(s), backend C', wait: 500 },
  { text: '[plan]     accepted: 3 step(s), 2 declared output(s), 1 final', wait: 900 },
  { text: '[compile]  ok (0.6 s, -O3)', wait: 900 },
  { text: '[test]     3 passed, 0 failed', wait: 700 },
  { text: '[build]    accepted', wait: 300 },
  { text: '[run]      step 3 (step3): ok', wait: 600 },
  { text: '', wait: 200 },
  { text: 'Results:', wait: 0 },
  { text: '  output/company.txt  (212 bytes)', wait: 0 },
  { text: '  output/top3Company.png  (4.1 KiB)  final', wait: 0 },
];
