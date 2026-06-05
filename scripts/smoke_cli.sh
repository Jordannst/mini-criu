#!/usr/bin/env bash
set -euo pipefail

APP="${1:-./build/mini-criu}"

if [[ ! -x "$APP" ]]; then
  echo "error: mini-criu binary is not executable: $APP" >&2
  exit 1
fi

run_check() {
  local name="$1"
  shift

  echo "==> $name"
  "$@"
}

run_check "help command" "$APP" help
run_check "status command" "$APP" status
run_check "list command" "$APP" list

echo "CLI smoke checks passed."
