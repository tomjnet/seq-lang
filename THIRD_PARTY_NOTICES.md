# Third-party notices

The code in this repository is original work under the MIT license in [LICENSE](LICENSE). That includes the compiler, the runtime library with its PNG encoder and bitmap font, the templates, and the `minigtest` stand-in used by the test suite.

`seqc` contains and links no third-party source code. It uses the following separately at run time. Each keeps its own license, and the MIT license of this project does not relicense any of them.

| Component | Used for | License |
| --- | --- | --- |
| GCC and GNU binutils | Compiling and linking generated programs | GPL-3.0-or-later, with the GCC Runtime Library Exception for its runtime libraries |
| GNU C Library (glibc) | Linked statically into generated programs | LGPL-2.1-or-later |
| Google Test | Linked into generated test binaries only | BSD-3-Clause |
| llama.cpp | Inference runtime started by `seqc` | MIT |
| Qwen2.5-Coder-1.5B-Instruct and its GGUF conversion | The reference model weights, downloaded by `seqc model pull` | Apache-2.0, as stated on the model card; check the card of the model you use |
| curl | Downloading model weights | curl license (MIT-style) |

## Generated programs

A generated executable statically links glibc and the seqc runtime. Distributing such an executable is subject to the LGPL terms of glibc. The seqc runtime itself is MIT.

This project does not settle who holds rights in model-generated source code or in the outputs of generated programs.

## Documentation

`docs/Google_Cpp_Style_Guide_20260925.md` is a Markdown copy of the Google C++ Style Guide, copyright Google, kept for offline reading. Its license and attribution are stated at <https://github.com/google/styleguide> (Creative Commons Attribution 3.0). It is not covered by this project's MIT license. The C and Rust style guides beside it were written for this project and draw on that guide and on the Rust Style Guide. None of these documents is code that `seqc` contains or installs.
