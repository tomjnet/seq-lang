# SeqAtom Coder 1.5B Instruct

## Complete Build Log: From Zero to First Hugging Face Upload

Date documented: 2026-10-04

Model repository:

`tomjnet/SeqAtom-Coder-1.5B-Instruct`

Base model:

`Qwen/Qwen2.5-Coder-1.5B-Instruct`

Primary goal:

Build an experimental coding model specialized for the Seq programming language, train it with LoRA, evaluate it, merge the adapter into the base model, package the merged checkpoint, and upload the resulting model to Hugging Face.

This document records the workflow developed in the SeqAtom Model project from the initial idea through the first successful upload of the merged model.

## 1. Model Naming and Scope

The model name selected for the first Seq focused coding model was:

`SeqAtom-Coder-1.5B-Instruct`

The Hugging Face target repository was:

`tomjnet/SeqAtom-Coder-1.5B-Instruct`

The base model selected for fine tuning was:

`Qwen/Qwen2.5-Coder-1.5B-Instruct`

The purpose of the model is to understand and generate Seq programming language code, including concepts such as:

`step`

`ask()`

`backend.C`

`backend.Rust`

Seq workflows

Seq code correction

Seq code explanation

`seqc` usage

## 2. Development Environment

The work was performed in Linux under WSL.

The working directory used later in the project was:

```bash
~/seqatom-training
```

A Python virtual environment was created:

```bash
python3 -m venv .venv
```

The environment was activated with:

```bash
source .venv/bin/activate
```

When active, the shell prompt showed:

```text
(.venv)
```

The project was therefore isolated from the global Python installation.

## 3. Python Dependencies

The original dependency set discussed for the training environment was:

```bash
pip install torch transformers datasets peft trl accelerate bitsandbytes huggingface_hub
```

These packages serve the following purposes.

| Package | Purpose |
| --- | --- |
| `torch` | PyTorch runtime and tensor operations |
| `transformers` | Qwen model and tokenizer loading |
| `datasets` | Dataset loading and processing |
| `peft` | LoRA and parameter efficient fine tuning |
| `trl` | Supervised fine tuning utilities |
| `accelerate` | Device management and training acceleration |
| `bitsandbytes` | Reduced precision and memory efficient model support |
| `huggingface_hub` | Authentication, repository access, and model upload |

The final project directory also contained:

```text
requirements.txt
requirements.lock.txt
```

The lock file was added so the environment used for the experiment could be reproduced more reliably.

## 4. Initial Project Structure

The first proposed structure was conceptually similar to:

```text
seqatom/
    data/
        train.jsonl
        validation.jsonl
        test.jsonl
    train.py
    merge.py
    test.py
    requirements.txt
```

As the experiment evolved, the actual working directory gained additional scripts for evaluation, merging, packaging, and reporting.

The final directory visible immediately after the upload contained:

```text
README.md
__pycache__/
evaluate_seqatom.py
merge_seqatom.py
output/
package_seqatom.py
report_seqatom.py
requirements.txt
requirements.lock.txt
test.py
train.py
train.py.before-lora
train.py.before-token-count-fix
```

The backup files are important because they show that the training script evolved during the experiment.

## 5. Dataset Creation

The first SeqAtom dataset was intentionally small. Its purpose was to prove that the complete training pipeline worked before investing time in a much larger dataset.

Three JSONL splits were created:

```text
data/train.jsonl
data/validation.jsonl
data/test.jsonl
```

The initial dataset contained:

```text
10 training examples
10 validation examples
10 test examples
```

The examples covered several Seq language behaviors, including:

1. Creating Seq programs with ordered `step` blocks.
2. Using `ask()` for AI assisted tasks.
3. Selecting `backend.C`.
4. Selecting `backend.Rust`.
5. Explaining Seq syntax.
6. Correcting invalid Seq code.
7. Generating Seq workflows.
8. Working with the `seqc` compiler workflow.
9. Producing coding oriented instructions and responses.
10. Teaching the intended structure of Seq programs.

The dataset used conversational instruction style examples so the instruct model could learn how to respond to Seq related requests.

A commit message used for the initial examples was:

```text
Add initial SeqAtom training examples
```

The early recommendation was to validate the pipeline with the small dataset first, then expand to approximately 100 examples, followed by 500 or more examples once the process was stable.

## 6. Initial LoRA Training Design

The first LoRA configuration used:

```text
Base model: Qwen/Qwen2.5-Coder-1.5B-Instruct
LoRA rank: 16
LoRA alpha: 32
LoRA dropout: 0.05
```

The targeted projection modules were:

```text
q_proj
k_proj
v_proj
o_proj
gate_proj
up_proj
down_proj
```

These modules cover the attention projections and the feed forward network projections commonly adapted during LoRA fine tuning of Qwen style models.

An early training output directory was:

```text
./seqcoder-output
```

An early adapter save location was:

```text
./SeqCoder-1.5B-LoRA
```

The project later standardized its naming around SeqAtom.

## 7. Training Script

The main training entry point became:

```text
train.py
```

The training flow was designed around the following sequence:

```text
Load Qwen tokenizer
Load Qwen base model
Load SeqAtom dataset
Format conversational examples
Apply LoRA configuration
Run supervised fine tuning
Save adapter and training artifacts
```

The project retained two historical copies of the script:

```text
train.py.before-lora
train.py.before-token-count-fix
```

These files document two important stages in the development of the training pipeline.

### 7.1 Before LoRA

`train.py.before-lora` preserved the script state before LoRA was fully introduced into the training workflow.

The project then moved toward parameter efficient fine tuning instead of training all model parameters.

That choice was appropriate for an experimental 1.5B parameter model because LoRA reduces training memory requirements and makes repeated experiments faster.

### 7.2 Token Count Fix

A second backup was created:

```text
train.py.before-token-count-fix
```

This records that the training script later required a correction related to token counting.

The final `train.py` therefore represents the corrected version after both LoRA support and the token count fix were incorporated.

The exact final implementation should be treated as the authoritative source for reproducing that training behavior.

## 8. Running the Training Environment

The normal workflow began by entering the project directory and activating the virtual environment:

```bash
cd ~/seqatom-training
source .venv/bin/activate
```

From that point, the project scripts could use the isolated Python dependencies and model training environment.

The main training command was centered on:

```bash
python train.py
```

The training stage produced SeqAtom model artifacts that were later evaluated and merged.

## 9. Basic Testing

The project contained:

```text
test.py
```

This script was used as a simple test entry point for checking the model or training workflow.

The purpose of this stage was to detect obvious failures before performing more formal evaluation or publishing the model.

A useful validation sequence for the project is:

```bash
python test.py
```

followed by the dedicated evaluation step.

## 10. Dedicated Evaluation

A dedicated evaluation script was later added:

```text
evaluate_seqatom.py
```

This separated evaluation from simple smoke testing.

The evaluation stage exists to measure whether the fine tuned SeqAtom model actually improves behavior on Seq related prompts rather than merely confirming that the model loads.

The intended evaluation flow is:

```text
Load evaluation examples
Load the trained SeqAtom checkpoint or adapter
Generate responses
Compare expected Seq behavior with model output
Record results for review
```

Run it with:

```bash
python evaluate_seqatom.py
```

The upload commit message later explicitly referenced evaluation, which confirms that evaluation artifacts were part of the model publication stage.

## 11. Merging the LoRA Adapter

Once the experimental adapter had been trained and evaluated, the next step was to merge the LoRA weights into the original Qwen base model.

The project created:

```text
merge_seqatom.py
```

The conceptual merge process is:

```text
Qwen/Qwen2.5-Coder-1.5B-Instruct
        plus
SeqAtom LoRA adapter
        becomes
fully merged SeqAtom model
```

The merge script was run with:

```bash
python merge_seqatom.py
```

The final merged model directory used for publication was:

```text
./output/seqatom-merged
```

This directory is especially important because it was the source directory passed directly to the Hugging Face upload command.

## 12. Packaging the Model

The project created a separate packaging script:

```text
package_seqatom.py
```

This stage helps verify and organize the files expected in a publishable Hugging Face model repository.

Run it with:

```bash
python package_seqatom.py
```

A merged Transformers model repository normally requires the model weights plus tokenizer and configuration files needed to load it independently of the original LoRA adapter.

The final upload reported:

```text
Found 16 files to upload
```

This confirms that the merged package contained a complete set of model repository artifacts rather than only one adapter file.

## 13. Experiment Report

The project also created:

```text
report_seqatom.py
```

This script was introduced so the experiment could produce a report separate from raw training output.

Run it with:

```bash
python report_seqatom.py
```

The purpose of this stage is to preserve information such as model identity, training configuration, evaluation summary, output locations, and experiment status.

Keeping reporting separate from training makes later experiments easier to compare.

## 14. Model Card

The project root contained:

```text
README.md
```

The model card was designed for the Hugging Face repository.

The metadata and documentation discussed for the model included:

```text
License: Apache-2.0
Base model: Qwen/Qwen2.5-Coder-1.5B-Instruct
Task: text-generation
Library: transformers
```

The README describes SeqAtom as a model focused on Seq programming language tasks such as:

1. Seq code generation.
2. Seq code completion.
3. Seq syntax explanation.
4. Seq error correction.
5. `ask()` based workflows.
6. `backend.C` and `backend.Rust`.
7. Interaction with the `seqc` compiler concept.

Because this was an early experiment with a very small dataset, the model should be described as experimental rather than as a production ready model.

## 15. Hugging Face Authentication

The environment required authentication with Hugging Face before model publication.

The original workflow discussed:

```bash
huggingface-cli login
```

The later upload used the newer `hf` command line interface.

The target repository was:

```text
tomjnet/SeqAtom-Coder-1.5B-Instruct
```

## 16. Final Upload Command

The successful model publication was performed from:

```text
~/seqatom-training
```

with the virtual environment activated.

The exact command shown in the final terminal was:

```bash
cd ~/seqatom-training
source .venv/bin/activate
hf upload tomjnet/SeqAtom-Coder-1.5B-Instruct ./output/seqatom-merged . --repo-type model --commit-message "Upload experimental SeqAtom merged checkpoint and evaluation"
```

The command means:

| Argument | Meaning |
| --- | --- |
| `tomjnet/SeqAtom-Coder-1.5B-Instruct` | Destination Hugging Face model repository |
| `./output/seqatom-merged` | Local merged model directory |
| `.` | Upload into the root of the remote repository |
| `--repo-type model` | Treat the destination as a model repository |
| `--commit-message` | Commit message for the uploaded model files |

## 17. Successful Upload Result

The Hugging Face CLI reported:

```text
Found 16 files to upload
Preparing     16 / 16
Uploading      3 / 3 files
Validating    100%
Committing    16 / 16
```

It then reported:

```text
No files have been modified since last commit. Skipping to prevent empty commit.
```

and finally confirmed the upload with:

```text
Uploaded
```

The returned Hugging Face commit URL was:

```text
https://huggingface.co/tomjnet/SeqAtom-Coder-1.5B-Instruct/commit/097ac61148b3ff7304ff046eb8855fdaab2474e4
```

At this point, the first experimental merged SeqAtom checkpoint had reached Hugging Face successfully.

## 18. Final Project State at the Upload Milestone

Immediately after the upload, running:

```bash
ls
```

showed:

```text
README.md
__pycache__
evaluate_seqatom.py
merge_seqatom.py
output
package_seqatom.py
report_seqatom.py
requirements.txt
requirements.lock.txt
test.py
train.py
train.py.before-lora
train.py.before-token-count-fix
```

This is the first important SeqAtom milestone:

```text
Idea
  to
Dataset
  to
LoRA training
  to
Training fixes
  to
Testing
  to
Evaluation
  to
Adapter merge
  to
Packaging
  to
Report
  to
Hugging Face upload
```

## 19. Reproduction Checklist

A clean reproduction should follow this order.

1. Clone or create the `seqatom-training` project.
2. Create `.venv`.
3. Activate `.venv`.
4. Install the dependencies from `requirements.txt` or the exact locked environment from `requirements.lock.txt`.
5. Confirm access to `Qwen/Qwen2.5-Coder-1.5B-Instruct`.
6. Prepare `train.jsonl`, `validation.jsonl`, and `test.jsonl`.
7. Run `train.py`.
8. Run `test.py`.
9. Run `evaluate_seqatom.py`.
10. Review evaluation output.
11. Run `merge_seqatom.py`.
12. Confirm that `output/seqatom-merged` exists and contains the complete merged model.
13. Run `package_seqatom.py`.
14. Run `report_seqatom.py`.
15. Review `README.md`.
16. Authenticate with Hugging Face.
17. Upload `output/seqatom-merged` to `tomjnet/SeqAtom-Coder-1.5B-Instruct`.

The final publication command is:

```bash
hf upload tomjnet/SeqAtom-Coder-1.5B-Instruct ./output/seqatom-merged . --repo-type model --commit-message "Upload experimental SeqAtom merged checkpoint and evaluation"
```

## 20. What Has Been Proven So Far

At this milestone, the project has demonstrated the complete lifecycle of a custom Seq focused coding model:

1. A suitable open coding base model was selected.
2. A Seq specific instruction dataset was created.
3. A LoRA training strategy was defined.
4. The training script evolved through multiple fixes.
5. Dedicated testing and evaluation tooling was created.
6. The LoRA adapter was merged with the base model.
7. A standalone merged checkpoint was produced.
8. Packaging and reporting scripts were added.
9. A Hugging Face model repository was prepared.
10. The merged checkpoint was uploaded successfully.

The most important achievement is not the quality of this first small dataset model. The important achievement is that the entire SeqAtom training and release pipeline now exists and works end to end.

## 21. Recommended Next Stage

The next phase should focus on model quality rather than pipeline construction.

The initial dataset was deliberately tiny. The next training cycle should expand the training corpus substantially while preserving a clean validation and test split.

The next milestone should include:

1. Expand the training dataset from the initial 10 examples to at least 100 carefully reviewed examples.
2. Add broader Seq syntax coverage.
3. Add valid and invalid Seq programs.
4. Add compiler oriented examples.
5. Add `ask()` behavior examples.
6. Add `backend.C` and `backend.Rust` examples.
7. Add multi step Seq programs.
8. Add code explanation and correction tasks.
9. Keep validation and test data separate from training data.
10. Record baseline Qwen results before fine tuning.
11. Compare baseline Qwen with the new SeqAtom checkpoint.
12. Save training metrics and evaluation results with every published version.
13. Version each experimental model release rather than overwriting previous experiments.
14. Only consider the model stable after it performs consistently on a much larger held out test set.

## 22. Important Experimental Status

`SeqAtom-Coder-1.5B-Instruct` at this point is an experimental checkpoint.

The first dataset contained only 10 training examples, so this upload primarily validates the architecture, scripts, merge process, packaging process, and Hugging Face release workflow.

It should not yet be treated as a mature Seq programming model.

The current milestone can be summarized as:

```text
SeqAtom training pipeline: WORKING

LoRA fine tuning pipeline: WORKING

Evaluation tooling: CREATED

Merge pipeline: WORKING

Packaging pipeline: CREATED

Hugging Face upload: SUCCESSFUL

Large scale Seq training corpus: NEXT
```

## 23. Reference Commands

Environment:

```bash
cd ~/seqatom-training
python3 -m venv .venv
source .venv/bin/activate
```

Dependencies:

```bash
pip install torch transformers datasets peft trl accelerate bitsandbytes huggingface_hub
```

Training:

```bash
python train.py
```

Basic test:

```bash
python test.py
```

Evaluation:

```bash
python evaluate_seqatom.py
```

Merge:

```bash
python merge_seqatom.py
```

Package:

```bash
python package_seqatom.py
```

Report:

```bash
python report_seqatom.py
```

Hugging Face authentication:

```bash
huggingface-cli login
```

Final model upload:

```bash
hf upload tomjnet/SeqAtom-Coder-1.5B-Instruct ./output/seqatom-merged . --repo-type model --commit-message "Upload experimental SeqAtom merged checkpoint and evaluation"
```

## 24. Milestone

The first SeqAtom model build reached its first publication milestone on 2026-10-04.

Published model repository:

`tomjnet/SeqAtom-Coder-1.5B-Instruct`

Published checkpoint source:

`./output/seqatom-merged`

Upload status:

`SUCCESS`

Hugging Face commit:

`097ac61148b3ff7304ff046eb8855fdaab2474e4`
