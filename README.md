# rvbbitduck

Semantic SQL for DuckDB 2.0 and newer. Decisions, lazy decision trees, and a small set of LLM operators, all as ordinary expressions. One OpenRouter key. No Clover or Hutch gateway.

This is a port of the decision core and the main text operators from [rvbbit](https://github.com/ryrobes/calliope) 1.2.3 (Ryan Robitaille's Postgres extension). The Postgres parser could not host real words like `MEANS` or a `CREATE DECISION TREE` statement. DuckDB 2.0 can, and a vector of up to 2048 rows is already a batch.

The extension you `LOAD` is named `rvbbit`. Functions live in the `rvbbit` schema (`rvbbit.truth`, `rvbbit.decision_tree`) so they do not collide with other extensions. `apart` already owns an unqualified `decision_tree`.

## What works in this version

- `CREATE SECRET (TYPE openrouter, KEY '...')`, plus `OPENROUTER_API_KEY` if no secret is present. Optional `BASE_URL`, `DECISION_MODEL`, and `CHAT_MODEL` on the secret. Settings of the same names override those.
- HTTP inside the extension. Each vector dedupes identical bodies. A process-wide cap (`rvbbit_max_concurrency`, default 8) bounds in-flight calls. 429 and 5xx retry with backoff, honoring `Retry-After` up to 5 seconds. Failures are not cached and do not become `false`.
- An in-memory LRU (`rvbbit_cache_size`, default 10000) and, when `rvbbit_cache_path` is set, a DuckDB file with a `judgments` table and a `trees` table.
- Decisions, matched to Calliope's request shape: `POST {base}/api/alpha/decisions` with model `typesafe/jev-1.13` and contract `clover-decisions-v1`. The evidence boundary is prefixed on every non-box question.
- `truth`, `truth_score`, `choose`, `decide`, `truth_question`, `choice_question`.
- `leaf`, `branch`, `choice_branch`, `decision_tree`, `decision_tree_batch`. The tree is validated before any call. Only the path taken is judged. Statuses are `complete`, `unavailable`, `budget_exhausted`, `context_limit`, and `missing_input`.
- `semantic_case` for a first-match list of predicates, evaluated as that same lazy tree.
- Named trees: `save_tree`, `tree`, `run_tree`.
- Chat operators on `{base}/api/v1/chat/completions`, default model `openai/gpt-6-luna`: `means`, `about`, `extract`, `prompt`, `summarize`, `classify`. Prompts are the seeded Calliope strings.
- Opt-in grammar: `col MEANS '...'`, `col ABOUT '...'`, `ABOVE`, `SEMANTIC CASE`, `CREATE DECISION TREE`, `DECIDE name FOR state`. See [docs/SYNTAX.md](docs/SYNTAX.md).

`rvbbit_base_url` defaults to `https://openrouter.ai`. Point it at Hutch or another gateway only if that gateway serves `/api/alpha/decisions` and `/api/v1/chat/completions`.

## What is stubbed

- No editable operator catalog, no retry wards, no multi-take voting, no receipt ledger, no cost reconciliation. Those are the Postgres operator engine (P2).
- No embeddings, `semantic_case` over vectors, `knn`, HNSW, or `NEAREST` (P3). The `semantic_case` that ships here is a Jev tree, not the embedding nearest-branch from Postgres.
- No `image_part`, `locate`, or `box_question`. The validator accepts a `box` or `score` question on `decide`, and box questions skip the evidence boundary, but there is no helper and the mock transport does not invent boxes.
- No aggregate `classify_collection`, no dimension operators (`tags`, `themes`), no `prewarm`.
- Cache keys are MD5 of the request body. Calliope uses blake3 for judgment keys. The identity fields match; the digest does not.
- `tree_id` is MD5 of canonical compact JSON, not Postgres `md5(tree::jsonb::text)`.

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

From the linked `./build/release/duckdb` shell, `LOAD rvbbit` is enough.

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

It is not enough, without replacing a core expression rule, to drop a new infix into the comparison precedence cleanly. `MEANS` / `ABOUT` are a dedicated expression alternative, not a change to `LIKE` or `ComparisonOperator`. Details, including the forms that did not fit, are in [docs/SYNTAX.md](docs/SYNTAX.md).

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
| `rvbbit_ignore_env` | `false` | Ignore `OPENROUTER_API_KEY`. |

`rvbbit.cache_stats()`, `rvbbit.cache_purge()`, `rvbbit.cache_clear_memory()`, and `rvbbit.http_log()` inspect the engine. `mock_reset`, `mock_fail_next`, and `mock_noul` exist so tests can force a score or a 429 without a socket.

## Next

1. **P2 operator catalog.** A `rvbbit.operators` table, `create_operator` that installs a macro, the rest of the seeded prompts, parsers, and the retry / ward / takes loop. Receipts can sit in the same cache file.
2. **P3 embeddings.** `embed` to `FLOAT[1024]`, `array_cosine_similarity`, a `knn_text` macro, and notes for `NEAREST` and the experimental vss HNSW index. The embedding `semantic_case` from Postgres belongs here, next to the Jev one, under a different name so the two are not confused.
3. **Question fusion.** `truth(x, a)` and `choose(x, ...)` on the same `x` in one vector can share a single `/decisions` request. The API allows many questions; Calliope's service capped a batch at 8 states, not 8 questions.
4. **Images and `score`.** `image_part` plus Jev 1.14 `locate`, and `truth_rank` for the ordered score question type Calliope has not shipped yet.
5. **Grammar follow-ups, only if the API grows.** A real precedence slot for word operators, and a `WHEN` arm that can hold a comparison. Until then the function form stays the contract.

## License

MIT. Copyright 2026 Ryan Robitaille.
