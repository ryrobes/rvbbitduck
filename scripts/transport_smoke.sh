#!/usr/bin/env bash
# Real libcurl transport against a local HTTP and HTTPS server. No OpenRouter key.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUCKDB="${DUCKDB:-$ROOT/build/release/duckdb}"
if [ ! -x "$DUCKDB" ]; then
  echo "build the extension first ($DUCKDB is missing)" >&2
  exit 1
fi
CERT_DIR="$(mktemp -d)"
SERVER_PID=""
trap 'rm -rf "$CERT_DIR"; [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true' EXIT
openssl req -x509 -newkey rsa:2048 -keyout "$CERT_DIR/key.pem" -out "$CERT_DIR/cert.pem" -days 1 -nodes \
  -subj "/CN=127.0.0.1" >/dev/null 2>&1
HTTP_PORT=8765
HTTPS_PORT=8766
python3 "$ROOT/scripts/local_openrouter.py" \
  --http-port "$HTTP_PORT" --https-port "$HTTPS_PORT" \
  --cert "$CERT_DIR/cert.pem" --key "$CERT_DIR/key.pem" &
SERVER_PID=$!
for _ in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
  if curl -sf "http://127.0.0.1:${HTTP_PORT}/health" >/dev/null; then
    break
  fi
  sleep 0.1
done
curl -sf "http://127.0.0.1:${HTTP_PORT}/health" >/dev/null

run_sql() {
  "$DUCKDB" -unsigned -no-agent -c "$1"
}

echo "HTTPS decisions, chat, embeddings, boxes, and score ranks"
HTTPS_OUT="$(run_sql "
LOAD rvbbit;
SET rvbbit_transport='live';
SET rvbbit_ignore_env=true;
SET rvbbit_tls_verify=false;
SET rvbbit_retry_base_ms=1;
SET rvbbit_base_url='https://127.0.0.1:${HTTPS_PORT}';
SET rvbbit_embed_dim=8;
CREATE SECRET local_or (TYPE openrouter, KEY 'local-test');
SELECT rvbbit.http_log_reset();
SELECT rvbbit.truth('Please cancel my appointment', 'The customer explicitly asks to cancel') AS wants_cancel;
SELECT rvbbit.sentiment('thanks for the help') AS tone;
SELECT len(rvbbit.embed('please cancel')) AS embed_len;
SELECT (rvbbit.locate('[\"please cancel\"]', 'cancel button')).top AS box_top;
SELECT rvbbit.truth_rank('billing problem', 'which queue', ['low', 'medium', 'high']) AS rank;
SELECT count(*) FILTER (WHERE status = 429) AS retried FROM rvbbit.http_log();
")"
echo "$HTTPS_OUT"
echo "$HTTPS_OUT" | grep -q $'wants_cancel\ntrue\|true' || true
python3 - "$HTTPS_OUT" <<'PY'
import sys
text = sys.argv[1]
def need(label, token):
    if token not in text:
        raise SystemExit(f"missing {label}: {token!r}\n{text}")
need("truth", "true")
need("sentiment", "positive")
need("embed length", "8")
need("box", "0.2")
need("rank", "medium")
need("retry", "1")
print("https matrix ok")
PY

echo "plain HTTP"
HTTP_OUT="$(run_sql "
LOAD rvbbit;
SET rvbbit_transport='live';
SET rvbbit_ignore_env=true;
SET rvbbit_base_url='http://127.0.0.1:${HTTP_PORT}';
CREATE SECRET local_http (TYPE openrouter, KEY 'local-test');
SELECT rvbbit.truth('Please cancel', 'wants to cancel') AS wants_cancel;
")"
echo "$HTTP_OUT"
echo "$HTTP_OUT" | grep -q "true"

echo "connection refused is an error, not NULL"
set +e
REFUSED="$(run_sql "
LOAD rvbbit;
SET rvbbit_transport='live';
SET rvbbit_ignore_env=true;
SET rvbbit_max_retries=0;
SET rvbbit_base_url='http://127.0.0.1:9';
CREATE SECRET nowhere (TYPE openrouter, KEY 'local-test');
SELECT rvbbit.truth('Please cancel', 'wants to cancel');
" 2>&1)"
STATUS=$?
set -e
if [ "$STATUS" -eq 0 ]; then
  echo "expected a connection error, got success:" >&2
  echo "$REFUSED" >&2
  exit 1
fi
echo "$REFUSED" | grep -q "could not connect" || {
  echo "connection failure was not a clear transport error:" >&2
  echo "$REFUSED" >&2
  exit 1
}

echo "empty chat content is an error"
set +e
EMPTY="$(run_sql "
LOAD rvbbit;
SET rvbbit_transport='live';
SET rvbbit_ignore_env=true;
SET rvbbit_tls_verify=false;
SET rvbbit_max_retries=0;
SET rvbbit_base_url='https://127.0.0.1:${HTTPS_PORT}';
CREATE SECRET local_empty (TYPE openrouter, KEY 'local-test');
SELECT rvbbit.sentiment('EMPTY_CONTENT please');
" 2>&1)"
STATUS=$?
set -e
if [ "$STATUS" -eq 0 ]; then
  echo "empty content was accepted:" >&2
  echo "$EMPTY" >&2
  exit 1
fi
echo "$EMPTY" | grep -q "content was empty" || {
  echo "empty content did not raise:" >&2
  echo "$EMPTY" >&2
  exit 1
}

echo "transport smoke passed"
