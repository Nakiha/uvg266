#!/bin/sh
set -eu
if [ "$#" != 2 ]; then echo "Usage: $0 input.i420 NEW_RUN_DIRECTORY" >&2; exit 2; fi
root=$(CDPATH='' cd "$(dirname "$0")/.." && pwd)
mkdir "$2"
"$root/build/live266" "$1" "$2/output.vvc" "$2/frames.csv" 2>"$2/encoder.log"
python3 "$root/scripts/summary.py" "$2/frames.csv" > "$2/summary.json"
cat "$2/summary.json"
