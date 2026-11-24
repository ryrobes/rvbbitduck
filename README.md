# rvbbitduck

Semantic SQL for DuckDB 2.0 and newer. Decisions, lazy decision trees, and a small set of LLM operators, all as ordinary expressions. One OpenRouter key. No Clover or Hutch gateway.

This is a port of the decision core and the main text operators from [rvbbit](https://github.com/ryrobes/calliope) 1.2.3 (Ryan Robitaille's Postgres extension). The Postgres parser could not host real words like `MEANS` or a `CREATE DECISION TREE` statement. DuckDB 2.0 can, and a vector of up to 2048 rows is already a batch.

The extension you `LOAD` is named `rvbbit`. Functions live in the `rvbbit` schema (`rvbbit.truth`, `rvbbit.decision_tree`) so they do not collide with other extensions. `apart` already owns an unqualified `decision_tree`.

## What works in this version

- `CREATE SECRET (TYPE openrouter, KEY '...')`, plus `OPENROUTER_API_KEY` if no secret is present. Optional `BASE_URL`, `DECISION_MODEL`, `CHAT_MODEL`, `EMBED_MODEL`, and `EMBED_DIM` on the secret. Settings of the same names override those. `rvbbit_embed_dim` of 0 keeps the secret or the default 1024.
- HTTP inside the extension. Each vector dedupes identical bodies. A process-wide cap (`rvbbit_max_concurrency`, default 8) bounds in-flight calls. 429 and 5xx retry with backoff, honoring `Retry-After` up to 5 seconds. Failures are not cached and do not become `false`.
- An in-memory LRU (`rvbbit_cache_size`, default 10000) and, when `rvbbit_cache_path` is set, a DuckDB file with a `judgments` table and a `trees` table.
- Decisions, matched to Calliope's request shape: `POST {base}/api/alpha/decisions` with model `typesafe/jev-1.13` and contract `clover-decisions-v1`. The evidence boundary is prefixed on every non-box question.
- `truth`, `truth_score`, `choose`, `decide`, `truth_question`, `choice_question`.
- `leaf`, `branch`, `choice_branch`, `decision_tree`, `decision_tree_batch`. The tree is validated before any call. Only the path taken is judged. Statuses are `complete`, `unavailable`, `budget_exhausted`, `context_limit`, and `missing_input`.
- `semantic_case` for a first-match list of predicates, evaluated as that same lazy tree.
- Named trees: `save_tree`, `tree`, `run_tree`.
- Fused decisions. Inside one chunk, questions about the same state share a single `POST /api/alpha/decisions` (up to 8). A rejected fused call is sent again as separate requests. Different states stay separate.
- The `rvbbit.operators()` catalog, seeded from Calliope 1.2.3: `means`, `about`, `summarize`, `classify`, `extract`, `condense`, `sentiment`, `contradicts`, `supports`, `implies`, `clean_year` (retry), `redact` (blocking post-ward), `headline` (3 takes and an evaluator), `safe_classify` (LLM step plus `validate_one_of`), and dimension lists `themes`, `tags`, `aspects`, `entities`, `keyphrases`. `prompt` stays free-form.
- `create_operator`, `set_operator_retry`, `set_operator_wards`, and `set_operator_takes`. A custom operator becomes a macro in `main` (`ping(...)`) and is always callable as `rvbbit.exec('ping', ...)`.
- Opt-in grammar: `MEANS`, `ABOUT`, `NEAR`, `ABOVE`, `SUPPORTS`, `CONTRADICTS`, `IMPLIES`, `SEMANTIC CASE`, `CREATE DECISION TREE`, `DECIDE name FOR state`. See [docs/SYNTAX.md](docs/SYNTAX.md).
- Embeddings: `embed` returns `FLOAT[]` at `rvbbit_embed_dim` (default 1024, model `voyageai/voyage-4`) via `POST {base}/api/v1/embeddings`. A chunk is deduped and sent in batches of 64. nomic, e5, and bge get Calliope's query/document prefixes before the cache key. `similarity`, `knn_text`, `nearest_label`, `topics`, `dedupe_groups`, and `diff` sit on that cache. `CAST(rvbbit.embed(text) AS FLOAT[1024])` works with DuckDB `NEAREST` and `array_cosine_similarity`.
- `image_part` (data URL or PNG/JPEG bytes), `box_question`, and `locate`. `locate` pins `typesafe/jev-1.14`, skips the evidence boundary, and returns `{top, bottom, left, right, confidence}`. `truth_rank` asks a score question and returns one of the supplied levels.

`rvbbit_base_url` defaults to `https://openrouter.ai`. Point it at Hutch or another gateway only if that gateway serves `/api/alpha/decisions`, `/api/v1/chat/completions`, and `/api/v1/embeddings`.

## What is stubbed

- `classify_collection` and any `aggregate`, `rowset`, or `query` shape are listed in the catalog and raise when called. Dimension operators return `VARCHAR[]` rather than Postgres `SETOF`.
- SQL validators understand `btrim($output) ~ 'regex'`, `$output !~ 'regex'`, `$output =` / `<>`, `length($inputs->>'field')` comparisons, and `AND` of those. Anything outside that subset is treated as passing, which is how Calliope treats a broken validator. Function-valued validators are stored and not run.
- Heterogeneous `takes.nodes` are stored. Execution covers homogeneous `factor` takes with `vote`, `first_valid`, `evaluator`, and `consensus`.
- The vss HNSW index is not linked. Build a `FLOAT[N]` column with `embed(...)::FLOAT[N]` (N must equal `rvbbit_embed_dim`, 1024 by default) and `INSTALL vss; LOAD vss;` yourself. CI covers brute-force `NEAREST`, not HNSW.
- `knn_text`, `topics`, `dedupe_groups`, and `diff` take `VARCHAR[]`, not a Postgres `regclass` plus a column name. Lists are capped at 2000 texts. `dedupe_groups` is pairwise cosine, so it is O(N²). `topics` runs a small deterministic k-means and caps k at 16. `nearest_label` is the embedding nearest-branch; `semantic_case` stays the Jev tree.
- `image_part` checks the data-URL prefix or the PNG/JPEG signature. Pixel bounds (each side 256–1568) are enforced by Jev, not here. Remote URLs are rejected.
- `truth_rank` sends `{type: score, criteria: {level: level}}` because Calliope 1.2.3 has no score-question SQL. The answer is the level name the model returns. That wire shape is the one this extension already validated for `decide`.
- No receipt or cost ledger, and no `prewarm`. Cache keys are MD5, not blake3. `tree_id` is MD5 of canonical compact JSON, not Postgres `md5(tree::jsonb::text)`.

## Install and build

Pinned to DuckDB **2.0.1** (`v2.0.1`, `8d78114e90a53823803660bf821f6073ac87da8c`) via the official C++ extension template. `extension-ci-tools` is the `v2.0-cyanoptera` branch. HTTP goes through DuckDB's own client, so there is no vcpkg dependency.

```sh
git clone --recurse-submodules https://github.com/ryrobes/rvbbitduck.git
cd rvbbitduck
sudo apt-get install -y ninja-build build-essential python3   # once
GEN=ninja make release
```

Binaries:

- `./build/release/duckdb` loads the extension
- `./build/release/test/unittest` runs SQLLogicTests
- `./build/release/extension/rvbbit/rvbbit.duckdb_extension` is the loadable binary

```sh
GEN=ninja make test
```

CI is `.github/workflows/ci.yml`. It builds DuckDB 2.0.1 and runs the SQLLogicTests. It does not call OpenRouter. `scripts/live_smoke.sh` exits immediately unless `OPENROUTER_API_KEY` is set, and the workflow fails if that variable is present so a secret cannot be spent by accident.

Load a built binary:

```sql
LOAD 'build/release/extension/rvbbit/rvbbit.duckdb_extension';
```

The `./build/release/duckdb` shell links rvbbit, so `LOAD rvbbit` works there. A stock DuckDB binary loads the shared library by path:

```sql
LOAD 'build/release/extension/rvbbit/rvbbit.duckdb_extension';
```

## Quickstart

```sql
LOAD rvbbit;

CREATE SECRET rvbbit (TYPE openrouter, KEY 'sk-or-...');
-- or export OPENROUTER_API_KEY=sk-or-...

SELECT id
FROM tickets
WHERE rvbbit.truth(body, 'The customer explicitly asks to cancel');

SELECT rvbbit.choose(body, ['billing', 'technical problem', 'cancellation', 'other'])
FROM tickets;

SELECT rvbbit.save_tree('churn', rvbbit.branch(
  'Does this customer show meaningful churn risk?',
  yes := rvbbit.branch(
    'Is an unresolved service problem the main concern?',
    yes := rvbbit.leaf('service intervention'),
    no := rvbbit.leaf('relationship review'),
    threshold := 0.7),
  no := rvbbit.leaf('routine follow-up')));

SELECT customer_id, (rvbbit.run_tree('churn', recent_messages)).label
FROM customer_contexts;
```

Sugar, off by default:

```sql
SET active_grammar_extensions = ['rvbbit'];

SELECT * FROM tickets WHERE body MEANS 'customer wants to cancel' ABOVE 0.8;

CREATE DECISION TREE churn AS
  BRANCH 'Does this customer show meaningful churn risk?'
    YES 'retention'
    NO 'routine follow-up';

SELECT (DECIDE churn FOR recent_messages).label FROM customer_contexts;

-- Custom operator. ping() is a macro in main; rvbbit.exec is the qualified form.
SELECT rvbbit.create_operator(
  'ping', ['text'], 'text',
  'Reply briefly.',
  'TEXT: {{ text }}\n\nReply:');

SELECT rvbbit.nearest_label(
  body, ['cancel the subscription', 'thanks for the help'], ['billing', 'praise'], 'other')
FROM tickets;

SELECT d.text
FROM (SELECT rvbbit.embed('cancel my subscription')::FLOAT[1024] AS embedding) q
JOIN docs d
NEAREST 1 BY SIMILARITY array_cosine_similarity(q.embedding, d.embedding);

SELECT (rvbbit.locate('[' || rvbbit.image_part(photo) || ']', 'the visible damage')).top
FROM listings;
```

Tests set `rvbbit_transport` to `mock`. That transport never leaves the process. `rvbbit_ignore_env` stops a developer key from satisfying a missing-secret test.

## How this differs from the other Jev extensions

| Extension | What it is | What it does not do |
|---|---|---|
| `decide` | `decision`, `decision_prob`, `decision_choice`, `decision_score` over OpenAI Decisions / Jev. Small batches, low concurrency. | No trees, no grammar, no shared chat operators. |
| `jev` | `jev`, `jev_prob`, `jev_choice`, `jev_score`, `jev_eval`. | Scalar judgments only. |
| `querio_jev` | OpenRouter, a disk cache, dedupe, httpfs. | No lazy tree, no path trace. |
| `typed_ai` | `typed_is`, `typed_pick`, `typed_score`, `typed_box`, budgets. | No tree walk. |
| `anofox_decide` | Many backends, including ONNX, plus calibration metrics. | No OpenRouter tree language. |
| `dc` | A fast path and a slow path. | No grammar and no decision tree. |
| `ai` | A wide LLM suite, including `ai_jev` / `ai_decide`, many providers. | Not a port of rvbbit's tree contract. |
| `flock` | `llm_complete`, filter, reduce, embeddings, and MODEL/PROMPT objects. | Objects are prompts, not a lazy judged tree. |
| `apart` | A numeric affine `decision_tree`. | Name collision only. Different function, different schema. |

What you get here that those do not: a lazy tree with prior-judgment context, evidence binding, an audited path, and wave batching across the rows of a vector; SQL that composes in `WHERE` / `JOIN` / `GROUP BY`; and opt-in words (`MEANS`, `SEMANTIC CASE`, `CREATE DECISION TREE`) that desugar to those functions.

## Grammar experiment

The 2.0.1 `GrammarExtension` API is enough to add rules, prepend expression alternatives, and add a statement, as long as the transform returns a normal DuckDB AST node. That is how the sugar is implemented: no new physical operator.

It is not enough, without replacing a core expression rule, to drop a new infix into the comparison precedence cleanly. `MEANS`, `ABOUT`, and `NEAR` are a dedicated expression alternative, not a change to `LIKE` or `ComparisonOperator`. Details, including the forms that did not fit, are in [docs/SYNTAX.md](docs/SYNTAX.md).

## Settings

| Setting | Default | Role |
|---|---|---|
| `rvbbit_transport` | `live` | `mock` answers locally. Tests use this. |
| `rvbbit_max_concurrency` | `8` | Process-wide in-flight HTTP cap. |
| `rvbbit_max_retries` | `3` | Extra attempts after 429 or 5xx. |
| `rvbbit_retry_base_ms` | `200` | First backoff. Later attempts double it, capped at 2s. |
| `rvbbit_cache_size` | `10000` | LRU entries. |
| `rvbbit_cache_path` | empty | DuckDB file for `judgments` and `trees`. |
| `rvbbit_base_url` | empty | Overrides the secret origin. |
| `rvbbit_decision_model` | empty | Overrides the Jev model. |
| `rvbbit_chat_model` | empty | Overrides the chat model. |
| `rvbbit_embed_model` | empty | Overrides the embeddings model. Empty uses the secret or `voyageai/voyage-4`. |
| `rvbbit_embed_dim` | `0` | `0` uses the secret or 1024. Any other value is the list length `embed` returns and the `dimensions` field sent to OpenRouter. |
| `rvbbit_ignore_env` | `false` | Ignore `OPENROUTER_API_KEY`. |

`rvbbit.cache_stats()`, `rvbbit.cache_purge()`, `rvbbit.cache_clear_memory()`, and `rvbbit.http_log()` inspect the engine. `mock_reset`, `mock_fail_next`, and `mock_noul` exist so tests can force a score or a 429 without a socket.

## Next

1. **Aggregate operators** such as `classify_collection`, heterogeneous `takes.nodes`, and a receipt or cost ledger beside the judgment cache.
2. **HNSW as an optional install.** vss stays out of this build. A later helper can create the `FLOAT[N]` column and the index when the user has `LOAD vss`. Brute-force `NEAREST` is what CI checks.
3. **Grammar follow-ups, only if the API grows.** A real precedence slot for word operators, and a `WHEN` arm that can hold a comparison. Until then the function form stays the contract.
4. **Live checks, still outside CI.** Embeddings and `locate` are mocked here. A keyed smoke can confirm that `voyageai/voyage-4` honors `dimensions: 1024` and that Jev 1.14 returns a box. Pixel-size checks stay on that service.

## License

MIT. Copyright 2026 Ryan Robitaille.
