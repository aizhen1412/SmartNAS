#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

export SMARTNAS_CONFIG="${SMARTNAS_CONFIG:-$ROOT_DIR/config/config.json}"
mapfile -t CONFIG_VALUES < <(
    python3 -c 'import json,os; c=json.load(open(os.environ["SMARTNAS_CONFIG"])); [print(c[k]) for k in ("core_port","agent_port","deepseek_api_base","deepseek_model","data_dir","database_path","deepseek_api_key")]'
)
CORE_PORT="${CONFIG_VALUES[0]}"
AGENT_PORT="${CONFIG_VALUES[1]}"
DEEPSEEK_API="${CONFIG_VALUES[2]}"
DEEPSEEK_MODEL="${CONFIG_VALUES[3]}"
DATA_DIR="${CONFIG_VALUES[4]}"
DATABASE_PATH="${CONFIG_VALUES[5]}"
CONFIG_DEEPSEEK_KEY="${CONFIG_VALUES[6]}"
[[ "$DATA_DIR" = /* ]] || DATA_DIR="$ROOT_DIR/$DATA_DIR"
[[ "$DATABASE_PATH" = /* ]] || DATABASE_PATH="$ROOT_DIR/$DATABASE_PATH"

mkdir -p "$DATA_DIR" "$(dirname "$DATABASE_PATH")"
if [[ ! -w "$DATA_DIR" || ! -w "$(dirname "$DATABASE_PATH")" ]]; then
    echo "SmartNAS data directories are not writable: $DATA_DIR or $(dirname "$DATABASE_PATH")" >&2
    exit 1
fi

if [[ ! -f "$ROOT_DIR/build/Makefile" ]]; then
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build"
fi

cmake --build "$ROOT_DIR/build"

"$ROOT_DIR/build/bin/SmartNAS" "$SMARTNAS_CONFIG" &
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

echo "SmartNAS Core:  http://127.0.0.1:$CORE_PORT"
echo "SmartNAS Agent: http://127.0.0.1:$AGENT_PORT"
echo "LLM provider:   DeepSeek"
echo "DeepSeek API:   $DEEPSEEK_API"
echo "DeepSeek model: $DEEPSEEK_MODEL"
if [[ -n "${SMARTNAS_DEEPSEEK_API_KEY:-${DEEPSEEK_API_KEY:-$CONFIG_DEEPSEEK_KEY}}" ]]; then
    echo "DeepSeek key:   configured"
else
    echo "DeepSeek key:   missing (set SMARTNAS_DEEPSEEK_API_KEY or DEEPSEEK_API_KEY)"
fi
echo "Press Ctrl+C to stop both services."

wait
