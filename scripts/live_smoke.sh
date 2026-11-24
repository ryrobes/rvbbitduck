#!/usr/bin/env bash
# Optional live OpenRouter matrix. CI does not set OPENROUTER_API_KEY, so this is a no-op there.
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
# A 1x1 PNG. Jev may still reject the pixel size; the call must be a real HTTP
# exchange either way, and a NULL with status 0 is a transport failure.
PNG='data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='
OUT="$("$DUCKDB" -unsigned -no-agent -c "
LOAD rvbbit;
SET rvbbit_ignore_env = false;
SELECT rvbbit.key_source() AS source;
SELECT rvbbit.truth('Please cancel my appointment', 'The customer explicitly asks to cancel') AS wants_cancel;
SELECT rvbbit.choose('My invoice shows a double charge', ['billing', 'technical', 'other']) AS desk;
SELECT rvbbit.sentiment('My invoice shows a double charge for October.') AS tone;
SELECT len(rvbbit.embed('Please cancel my appointment')) AS embed_len;
SELECT (rvbbit.locate('[\"Please cancel\", \"${PNG}\"]', 'the words cancel')).top AS box_top;
SELECT rvbbit.truth_rank('The food was okay but service was slow', 'How satisfied is the customer?', ['low', 'medium', 'high']) AS rank;
SELECT (rvbbit.decision_tree(
    'thanks for the help today',
    rvbbit.branch('the customer wants to cancel',
      yes := rvbbit.leaf('retention'),
      no := rvbbit.branch('this is about billing', yes := rvbbit.leaf('finance'), no := rvbbit.leaf('routine'))))).label AS nested_no;
SELECT count(*) FILTER (WHERE status = 0) AS transport_failures FROM rvbbit.http_log();
")"
echo "$OUT"
echo "$OUT" | grep -q "true" || { echo "live truth did not return true" >&2; exit 1; }
echo "$OUT" | grep -q "1024" || { echo "embed did not return 1024 dimensions" >&2; exit 1; }
# A status-0 count prints as its own result. Refuse that without confusing it with 1024.
if echo "$OUT" | grep -E -q 'transport_failures.*[^0]|│ +[1-9]'; then
  echo "live transport recorded a failed connection" >&2
  exit 1
fi
echo "live smoke finished"
