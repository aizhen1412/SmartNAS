#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

export SMARTNAS_MODEL_PATH="${SMARTNAS_MODEL_PATH:-$ROOT_DIR/models/llm/qwen2.5-7b-instruct}"
export SMARTNAS_CORE_API="${SMARTNAS_CORE_API:-http://127.0.0.1:8080}"

if [[ ! -f "$ROOT_DIR/build/Makefile" ]]; then
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"
fi

if [[ ! -x "$ROOT_DIR/build/bin/SmartNAS" ]]; then
    cmake --build "$ROOT_DIR/build"
fi

(
    cd "$ROOT_DIR/build/bin"
    ./SmartNAS
) &
CORE_PID=$!

python3 "$ROOT_DIR/scripts/agent_service.py" &
AGENT_PID=$!

cleanup() {
    kill "$CORE_PID" "$AGENT_PID" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

sleep 1
if ! kill -0 "$CORE_PID" 2>/dev/null; then
    echo "SmartNAS Core failed to start."
    kill "$AGENT_PID" 2>/dev/null || true
    exit 1
fi

if ! kill -0 "$AGENT_PID" 2>/dev/null; then
    echo "SmartNAS Agent failed to start."
    kill "$CORE_PID" 2>/dev/null || true
    exit 1
fi

echo "SmartNAS Core:  http://127.0.0.1:8080"
echo "SmartNAS Agent: http://127.0.0.1:8081"
echo "LLM backend:    transformers"
echo "Model path:     $SMARTNAS_MODEL_PATH"
echo "Press Ctrl+C to stop both services."

wait
