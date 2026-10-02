#!/usr/bin/env python3
"""Builds a script for seqc's fake model adapter from plain source files.

Writing C inside JSON by hand is error-prone, so fixtures keep each scripted
response in its own file and this tool assembles them.

Usage:
  make_script.py OUT.json [--plan FILE ...] [--generate FILE ...] [--test FILE ...]

Each FILE becomes the next response of its family, in order:
  *.json          returned as a JSON document (a plan)
  *.c             wrapped as {"step_functions": <text>}
  *.cc            wrapped as {"test_cases": <text>}
  *.raw           returned verbatim, to script a malformed response
  error:MESSAGE   a provider failure with that message
"""

import json
import sys


def entry(spec: str, family: str):
    if spec.startswith("error:"):
        return {"$error": spec[len("error:"):]}
    with open(spec, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    if spec.endswith(".raw"):
        return text
    if spec.endswith(".json"):
        return json.loads(text)
    field = {"generate": "step_functions", "test": "test_cases"}[family]
    return {field: text}


def main() -> int:
    out = sys.argv[1]
    script = {"plan": [], "generate": [], "test": []}
    family = None
    for arg in sys.argv[2:]:
        if arg in ("--plan", "--generate", "--test"):
            family = arg[2:]
        elif family is None:
            print(f"expected --plan, --generate, or --test before {arg}")
            return 2
        else:
            script[family].append(entry(arg, family))
    with open(out, "w", encoding="utf-8") as f:
        json.dump(script, f, indent=2)
        f.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
