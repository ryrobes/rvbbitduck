#!/usr/bin/env bash
# Optional live call. CI does not set OPENROUTER_API_KEY, so this is a no-op there.
set -euo pipefail
if [ -z "${OPENROUTER_API_KEY:-}" ]; then
  echo "OPENROUTER_API_KEY is unset; skipping live smoke."
  exit 0
fi
DUCKDB="${DUCKDB:-./build/release/duckdb}"
if [ ! -x "$DUCKDB" ]; then
  echo "build the extension first ($DUCKDB is missing)" >&2
  exit 1
fi
"$DUCKDB" -unsigned -c "
LOAD rvbbit;
SET rvbbit_ignore_env = false;
SELECT rvbbit.key_source() AS source;
SELECT rvbbit.truth('Please cancel my appointment', 'The customer explicitly asks to cancel') AS wants_cancel;
"
