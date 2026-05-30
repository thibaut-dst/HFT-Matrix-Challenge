#!/usr/bin/env bash
set -euo pipefail

HOST=${1:-127.0.0.1}
PORT=${2:-12345}
NUM_CLIENTS=${3:-8}
CLIENT_BIN=${4:-./build/bin/hftclient2026}
LOG_DIR=${5:-./logs}

if [ ! -x "$CLIENT_BIN" ]; then
  echo "Client binary not found or not executable: $CLIENT_BIN"
  echo "Run ./build.sh first or provide a valid path as the 4th argument."
  exit 1
fi

# Ensure log directory exists
mkdir -p "$LOG_DIR"

echo "Using client binary: $CLIENT_BIN"
echo "Writing logs to: $LOG_DIR"
echo "Launching $NUM_CLIENTS clients to $HOST:$PORT"

PIDFILE="client_pids.txt"
rm -f "$PIDFILE"

for i in $(seq 1 "$NUM_CLIENTS"); do
  LOG="${LOG_DIR}/client_${i}.log"
  "$CLIENT_BIN" "$HOST" "$PORT" "Team${i}" > "$LOG" 2>&1 &
  pid=$!
  echo "$pid" >> "$PIDFILE"
  echo "Launched client $i (PID $pid), log: $LOG"
  sleep 0.05
done

echo "All clients launched. PIDs written to $PIDFILE"

