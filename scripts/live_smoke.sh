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
# Jev rejects images smaller than 256px on a side. A solid 256x256 PNG is enough
# for locate to be a real call rather than a 400.
PNG="$(python3 - <<'PY'
import base64, struct, zlib
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
w = h = 256
raw = b"".join(b"\x00" + b"\x00\x00\x00" * w for _ in range(h))
png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
       chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))
print("data:image/png;base64," + base64.b64encode(png).decode(), end="")
PY
)"
# One CSV row, no header. Each field is checked as a whole token.
ROW="$("$DUCKDB" -unsigned -no-agent -csv -noheader -c "
LOAD rvbbit;
SET rvbbit_ignore_env = false;
SELECT
  rvbbit.key_source(),
  rvbbit.truth('Please cancel my appointment', 'The customer explicitly asks to cancel'),
  rvbbit.choose('My invoice shows a double charge', ['billing', 'technical', 'other']) IS NOT NULL,
  rvbbit.sentiment('My invoice shows a double charge for October.') IS NOT NULL,
  len(rvbbit.embed('Please cancel my appointment')),
  (rvbbit.locate('[\"Please cancel\", \"${PNG}\"]', 'the words cancel')).top IS NOT NULL,
  rvbbit.truth_rank('The food was okay but service was slow', 'How satisfied is the customer?', ['low', 'medium', 'high']) IS NOT NULL,
  (rvbbit.decision_tree(
      'thanks for the help today',
      rvbbit.branch('the customer wants to cancel',
        yes := rvbbit.leaf('retention'),
        no := rvbbit.branch('this is about billing', yes := rvbbit.leaf('finance'), no := rvbbit.leaf('routine'))))).label IS NOT NULL,
  (SELECT count(*) FILTER (WHERE status = 0) FROM rvbbit.http_log());
")"
echo "$ROW"
# key_source, truth, choose, sentiment, embed_len, locate, rank, nested_no, transport_failures
IFS=',' read -r SOURCE TRUTH CHOOSE TONE EMBED_LEN LOCATE RANK NESTED FAILURES <<<"$ROW"
[ "$SOURCE" = "secret" ] || [ "$SOURCE" = "env" ] || { echo "unexpected key source: [$SOURCE]" >&2; exit 1; }
[ "$TRUTH" = "true" ] || { echo "truth was [$TRUTH], expected true" >&2; exit 1; }
[ "$CHOOSE" = "true" ] || { echo "choose was null" >&2; exit 1; }
[ "$TONE" = "true" ] || { echo "sentiment was null" >&2; exit 1; }
[ "$EMBED_LEN" = "1024" ] || { echo "embed length was [$EMBED_LEN], expected 1024" >&2; exit 1; }
[ "$LOCATE" = "true" ] || { echo "locate did not return a box" >&2; exit 1; }
[ "$RANK" = "true" ] || { echo "truth_rank was null" >&2; exit 1; }
[ "$NESTED" = "true" ] || { echo "nested no-branch did not finish" >&2; exit 1; }
[ "$FAILURES" = "0" ] || { echo "transport failures were [$FAILURES], expected 0" >&2; exit 1; }
echo "live smoke finished"
