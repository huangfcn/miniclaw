#!/usr/bin/env bash
# Desktop E2E scenario runner for the mobile C ABI (MSYS2/Windows).
#
# usage: run_e2e_scenario.sh "<command the mock LLM should issue>"
#
# Prereqs:
#   - backend built with -DMC_E2E_TEST=ON and `mc_e2e` target built
#   - mock LLM running:  python backend/test/mock_llm.py
#
# Creates a fresh workspace (session state persists between runs, which would
# make the mock skip issuing a new tool call), points it at the mock, and
# runs the driver. Prints engine events; tool output is multi-line.
set -u
CMD="${1:-ls | sort}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
WS="C:/tmp/mc_e2e_ws"

printf '%s' "$CMD" > /c/tmp/mc_mock_cmd.txt

rm -rf /c/tmp/mc_e2e_ws && mkdir -p /c/tmp/mc_e2e_ws && cd /c/tmp/mc_e2e_ws
printf 'hello world\nhello miniclaw\nfoo bar baz\n' > notes.txt
echo "alpha" > a.txt; echo "beta" > b.txt
cat > config.yaml <<'YAML'
conversation:
  provider: openai
  model: mock-model
  endpoint: "http://127.0.0.1:9876/v1/chat/completions"
  api_key: dummy
embedding:
  endpoint: "http://127.0.0.1:9876/v1/embeddings"
  model: mock-embed
  dimension: 1536
YAML

DRIVER="$ROOT/backend/build/mc_e2e.exe"
[ -f "$DRIVER" ] || DRIVER="$ROOT/frontend/src-tauri/binaries/mc_e2e.exe"
# Run from a directory with the runtime DLLs (the Tauri binaries dir has them).
cd "$ROOT/frontend/src-tauri/binaries" 2>/dev/null || cd "$(dirname "$DRIVER")"
# NOTE: don't filter with grep -E "^\[event\]" — tool output is multi-line
# and only its first line carries the [event] prefix.
"$DRIVER" "$WS" "test message" 2>&1 | grep -vE "^\[[0-9]{4}-"
