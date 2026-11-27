# Syntax

rvbbitduck is built so semantic judgment reads as SQL. The function form is the source of truth. A grammar extension, off until you turn it on, rewrites a few statements and predicates into those same functions.

Calliope 1.2.3 is the behavior reference. Where Postgres could not express something (a real `MEANS` word, a `CREATE DECISION TREE` statement, a struct you can project with `.label`), this page shows the DuckDB form beside it.

## Three ways to write the same judgment

| | rvbbit on Postgres | rvbbitduck, plain SQL | rvbbitduck, grammar on |
|---|---|---|---|
| Yes / no | `rvbbit.truth(body, 'wants to cancel')` | `rvbbit.truth(body, 'wants to cancel')` | `body MEANS 'wants to cancel'` |
| Yes / no with a threshold | `rvbbit.truth(body, 'wants to cancel', 0.8)` | `rvbbit.truth(body, 'wants to cancel', 0.8)` | `body MEANS 'wants to cancel' ABOVE 0.8` |
| Relevance | `rvbbit.about(body, 'billing')` | `rvbbit.about(body, 'billing')` | `body ABOUT 'billing'` |
| Relevance gate | `rvbbit.about(body, 'billing') > 0.5` | `rvbbit.about(body, 'billing') > 0.5` | `body ABOUT 'billing' ABOVE 0.5` |
| Pick a label | `rvbbit.choose(body, ARRAY['billing','other'])` | `rvbbit.choose(body, ['billing','other'])` | same function; no infix |
| One-sentence summary | `rvbbit.summarize(body)` | `rvbbit.summarize(body)` | same function |
| Pull a value out | `rvbbit.extract(body, 'the order number')` | `rvbbit.extract(body, 'the order number')` | same function |
| Relation | `rvbbit.supports(a, b)` | `rvbbit.supports(a, b)` | `a SUPPORTS 'b'` or `a SUPPORTS b` |
| Free-form instruction | a catalog operator | `rvbbit.prompt(body, 'reply with the intent')` | same function |

Turn the sugar on for the session:

```sql
LOAD rvbbit;
SET active_grammar_extensions = ['rvbbit'];
```

`SET active_grammar_extensions = []` turns it back off. Nothing in the function API changes.

`MEANS`, `ABOUT`, `SUPPORTS`, `CONTRADICTS`, and `IMPLIES` stay identifiers, not reserved words, so `rvbbit.means(...)` still parses while the grammar is on. `DECIDE`, `SEMANTIC`, `ABOVE`, `DECISION`, and `TREE` are added as unreserved keywords only while the grammar is active. They can still be column names. A function literally named `decide` would not resolve through the generic function rule; the plain name for a stored tree is `rvbbit.run_tree`.

## Decisions

A failed provider call is NULL for `truth`, `truth_score`, `choose`, and `decide`. It is never `false` and never a fallback label. `truth` is true only when the score is strictly greater than the threshold (default 0.5). NULL inputs skip the call.

```sql
SELECT id
FROM tickets
WHERE rvbbit.truth(body, 'The customer explicitly asks to cancel');

SELECT rvbbit.truth_score(body, 'The customer explicitly asks to cancel') AS score
FROM tickets;

SELECT rvbbit.choose(body, ['billing', 'technical problem', 'cancellation', 'other'])
FROM tickets;

-- Label descriptions, same cache entry as the array form when the definitions match.
SELECT rvbbit.choose(body, '{"billing":"Invoices, charges, or payments","other":"Anything else"}')
FROM tickets;
```

`rvbbit.decide(state, questions)` is the `clover_decide` equivalent. `questions` is a JSON object of named `noul` or `choice` questions. The extension adds the same evidence boundary Calliope adds, unless the question type is `box` (accepted by the validator, not given a helper in this version). The result is JSON with `answers`, `model`, `requested_model`, `contract` (`clover-decisions-v1`), and numeric `usage`.

State is text, or text that parses as a JSON object or array. A JSON object is sent as an object, not as a string wrapped in another string.

```sql
SELECT rvbbit.decide(
  '{"subject":"Invoice 204","body":"Please explain this charge"}',
  rvbbit.truth_question('Does this message ask for a response?')
);
```

`truth_question` and `choice_question` only build JSON. They do not call the model, which makes them safe to store.

## Decision trees

Constructors are pure. Only `decision_tree` / `run_tree` / `semantic_case` call the model, and only for nodes on the path that was taken.

```sql
SELECT rvbbit.branch(
  'Does this customer show meaningful churn risk?',
  yes := rvbbit.branch(
    'Is an unresolved service problem the main concern?',
    yes := rvbbit.leaf('service intervention'),
    no := rvbbit.leaf('relationship review'),
    threshold := 0.7),
  no := rvbbit.leaf('routine follow-up'));
```

Named arguments use `:=` or `=>`. The result is a STRUCT, so the label is a normal field:

```sql
SELECT customer_id,
       (rvbbit.decision_tree(
          to_json({'recent_messages': recent_messages, 'account': account_summary})::VARCHAR,
          policy.tree)).label AS route
FROM customer_contexts, policies
WHERE policies.name = 'churn';
```

The struct fields are `version`, `tree_id`, `status`, `label`, `steps`, `path`, `error`, `stopped_at`, and `missing_inputs`. `version` is `clover-tree-v1`. `status` is `complete`, `unavailable`, `budget_exhausted`, `context_limit`, or `missing_input`. `tree_id` is the MD5 of the canonical JSON of the tree. Postgres hashes `jsonb::text`, which inserts spaces and orders keys its own way, so the hex digest will not match a Postgres `md5` of the same tree. It is stable inside DuckDB.

`inputs` limits what a node sees, and what goes into its cache key:

```sql
rvbbit.branch(
  'Is the account overdue?',
  yes := rvbbit.leaf('collections'),
  no := rvbbit.leaf('ok'),
  inputs := ['account'])
```

If a visited node names a field the evidence object does not have, the walk stops with `missing_input` and does not call the model. `max_steps` defaults to 16 and must be 1..16. A leaf that is never reached is never judged. Prior judgments are passed along as interpretations, not as new evidence, using Calliope's wording.

`decision_tree_batch(states, tree)` evaluates a list in one wave schedule. A normal `SELECT rvbbit.decision_tree(...)` already batches the rows of each vector, so the batch function is there for the Postgres call shape, not because the scalar is row-at-a-time.

### A tree with a name

```sql
SELECT rvbbit.save_tree('churn', rvbbit.branch(
  'Does this customer show meaningful churn risk?',
  yes := rvbbit.leaf('retention'),
  no := rvbbit.leaf('routine follow-up')));

SELECT (rvbbit.run_tree('churn', recent_messages)).label
FROM customer_contexts;
```

With the grammar on, the same definition is a statement and the call looks like an operator. Both desugar to `save_tree` and `run_tree`:

```sql
CREATE DECISION TREE churn AS
  BRANCH 'Does this customer show meaningful churn risk?'
    YES 'retention'
    NO 'routine follow-up';

SELECT (DECIDE churn FOR recent_messages).label
FROM customer_contexts;
```

`BRANCH` / `YES` / `NO` in that statement are matched as words. The grammar tree only builds boolean branches and string leaves. `choice_branch` stays a function.

### SEMANTIC CASE

First match, top to bottom. It compiles to a right-deep tree and evaluates only until a predicate hits, which is the lazy behavior `CASE` cannot give you if every branch is an independent function call the planner evaluates freely. In the function form you pass the predicates yourself:

```sql
SELECT rvbbit.semantic_case(
  body,
  ['The customer explicitly asks to cancel', 'The request is about billing'],
  ['retention', 'finance'],
  [0.8, 0.5],
  'triage')
FROM tickets;
```

With the grammar on, predicates are string literals. A `WHEN` that is itself a `choose(...)` comparison is not parsed; write that in the function form or as a normal `CASE` around `rvbbit.choose`.

```sql
SELECT SEMANTIC CASE ON body
         WHEN 'The customer explicitly asks to cancel' ABOVE 0.8 THEN 'retention'
         WHEN 'The request is about billing' THEN 'finance'
         ELSE 'triage'
       END
FROM tickets;
```

## Operator catalog

`SELECT * FROM rvbbit.operators()` lists the seeded Calliope 1.2.3 operators. `means`, `about`, and the other scalar names call whatever prompt is stored there, so an edit is visible on the next call. Dimension operators (`themes`, `tags`, `aspects`, `entities`, `keyphrases`) return `VARCHAR[]`; unnest them when you want one row per label.

```sql
SELECT rvbbit.set_operator_retry(
  'clean_year',
  '{"until":{"sql":"btrim($output) ~ ''^((1[6-9]|20)[0-9]{2}|unknown)$''"},"max_attempts":3,"instructions":"Reply with a 4-digit year or unknown"}');

SELECT rvbbit.set_operator_wards(
  'redact',
  '{"post":[{"validator":{"sql":"$output !~ ''[A-Za-z0-9._%+-]+@[A-Za-z0-9._-]+''"},"mode":"blocking"}]}');

SELECT rvbbit.set_operator_takes(
  'headline',
  '{"factor":3,"reduce":"evaluator","evaluator":{"instructions":"Reply with only the number of the best headline."}}');
```

Order of a call: pre-wards, then takes (or one chat), then retry, then post-wards. A blocking pre-ward raises. A blocking post-ward returns NULL and is not cached. Retry does not repeat a transport failure. `vote`, `first_valid`, `evaluator`, and `consensus` are the reducers. `max_attempts` includes the first try.

The `rvbbit` schema cannot hold new macros. `create_operator` writes a macro in `main` and always leaves `rvbbit.exec('name', ...)` working:

```sql
SELECT rvbbit.create_operator(
  'ping', ['text'], 'text',
  'Reply briefly.',
  'TEXT: {{ text }}\n\nReply:');

SELECT ping('hello');
SELECT rvbbit.exec('ping', 'hello');
```

`{{ arg }}`, `{{ inputs.arg }}`, and `{{ steps.name.output }}` are the template fields. `safe_classify` is the seeded two-step operator: an LLM call, then the `validate_one_of` code step.

## Grouped aggregates

Calliope 1.2.3 declares `classify_collection` as an aggregate and does not execute it. Here it is a real aggregate, along with three collection forms that use the same chat path. They sit in `GROUP BY` like any other aggregate. Per-row `themes(text)`, `tags(text)`, and the other dimension lists stay one value per row.

```sql
SELECT region,
       rvbbit.classify_collection(body, 'billing,shipping,other') AS desk,
       rvbbit.summarize_collection(body) AS summary,
       rvbbit.consensus(body) AS agreed,
       rvbbit.collection_themes(body) AS themes
FROM tickets
GROUP BY region;
```

A group keeps at most 48 distinct texts. Each text is cut at 400 characters. Duplicate texts in the group are one input. When a group has more than 12 texts, each chunk of 12 is judged first and those answers are judged again. The category list passed to `classify_collection` should be constant inside the group; the first non-null value is the one that is sent.

`SELECT rvbbit.classify_collection(body, 'billing,other') FROM tickets` with no `GROUP BY` is one collection: the whole table.

## Usage ledger

Every OpenRouter POST and every cache hit is a row. The API key is not stored. A cache hit has `cost` 0, `cache_hit` true, and `endpoint` `cache`. Token counts and `cost` come from the provider `usage` object when it is present (`prompt_tokens` or `input_tokens`, `completion_tokens` or `output_tokens`, `cost`).

```sql
SELECT kind, model, prompt_tokens, completion_tokens, cost, cache_hit, status
FROM rvbbit.ledger();

SELECT http_requests, cache_hits, prompt_tokens, completion_tokens, cost
FROM rvbbit.spend();

SELECT rvbbit.ledger_reset();
```

Rows stay in memory for the process (the newest 10000). If `rvbbit_cache_path` is set, they are also appended to a `ledger` table in that DuckDB file and reloaded the next time the ledger is read.

## Embeddings

`rvbbit.embed(text)` returns `FLOAT[]`. The length is `rvbbit_embed_dim` when that setting is at least 2, otherwise the secret's `EMBED_DIM`, otherwise 1024. The default model is `mongodb/voyage-4`. `voyageai/voyage-4` is accepted and rewritten to that id before the request and the cache key. The request is OpenAI-shaped:

```json
{"model":"mongodb/voyage-4","input":["..."],"dimensions":1024}
```

`POST {base}/api/v1/embeddings`. Identical texts in one chunk share one slot. Misses go out in batches of 64. The cache key is the model, the dimension, and the text after Calliope's mode prefix. `embed(text, 'query')` prefixes nomic (`search_query:` / `search_document:`), e5 (`query:` / `passage:`), and bge (the query instruction). An empty mode, and every other model, sends the raw text. `knn_text` and the composites embed raw, the same way Calliope's generic path does, so they share one vector space.

| Calliope | Here | Returns |
|---|---|---|
| `rvbbit.embed` | `rvbbit.embed(text [, mode])` | `FLOAT[]` |
| `rvbbit.similarity` | `rvbbit.similarity(a, b)` | cosine |
| `rvbbit.knn_text(rel, col, query, k)` | `rvbbit.knn_text(query, texts, k)` | `value`, `score` |
| embedding `semantic_case` | `rvbbit.nearest_label(text, conditions, results, otherwise, min_score)` | the winning label, or `otherwise` below `min_score` |
| `rvbbit.topics` | `rvbbit.topics(texts, k)` | `cluster_id`, `size`, `exemplar` |
| `rvbbit.dedupe_groups` | `rvbbit.dedupe_groups(texts [, threshold])` | `group_id`, `representative`, `size`, `members` |
| `rvbbit.diff` | `rvbbit.diff(a, b, k)` | `text`, `novelty` (`1 - max cosine`) |

`semantic_case` remains the Jev tree. `nearest_label` is the cosine argmax.

DuckDB already has the join. The query row is the left side; `NEAREST k` keeps k rows of the right side:

```sql
SELECT d.text
FROM (SELECT rvbbit.embed('cancel my subscription')::FLOAT[1024] AS embedding) q
JOIN docs d
NEAREST 1 BY SIMILARITY array_cosine_similarity(q.embedding, d.embedding);
```

`array_cosine_similarity` wants `FLOAT[N]`, and so does an HNSW index. Cast when the list length is N:

```sql
ALTER TABLE docs ADD COLUMN embedding FLOAT[1024];
UPDATE docs SET embedding = rvbbit.embed(text)::FLOAT[1024];

INSTALL vss;
LOAD vss;
CREATE INDEX docs_hnsw ON docs USING HNSW (embedding) WITH (metric = 'cosine');
```

vss is not part of this extension. CI checks the cast and the brute-force `NEAREST` join with the mock embedder. It does not load vss.

With the grammar on, `a NEAR 'b'` is `rvbbit.similarity(a, 'b')`, and `a NEAR 'b' ABOVE 0.8` is that cosine compared with `0.8`. The function form does not need the grammar.

## Images and ranks

`image_part` wraps a picture the way Calliope does. It does not call the network.

```sql
SELECT rvbbit.image_part(photo);  -- BLOB whose signature is PNG or JPEG
SELECT rvbbit.box_question('the visible damage');
```

`locate` builds that box question and sends it to `typesafe/jev-1.14`. Pass a model only when you want a different one. Box instructions are not prefixed with the evidence boundary. The result is a struct of fractions of the image. A box comes back even when the target is absent; pair it with `truth` if you need a yes or no.

```sql
SELECT (rvbbit.locate(
          '[' || rvbbit.image_part(photo) || ']',
          'the visible damage')).top
FROM listings;
```

`truth_rank(state, question, levels)` sends a score question whose `criteria` is the level array, in order. Jev answers with a numeric `score` (an index), a `legend` of index to label, and `probabilities` keyed by index. The function returns the legend label with the highest probability. Calliope 1.2.3 does not ship this question. The evidence boundary is applied, unlike a box.

```sql
SELECT rvbbit.truth_rank(body, 'how urgent is this', ['low', 'medium', 'high'])
FROM tickets;
```

## Fused decisions

A single `truth` or `decision_tree` call sees up to 2048 rows. Rows that share a state and differ only in the question are one `POST /api/alpha/decisions`. The cap is 8 questions per request. Two different states stay two requests. If the fused response is rejected, those questions are sent again one by one.

```sql
SELECT rvbbit.truth(state, predicate)
FROM (VALUES
  ('please cancel the appointment', 'the customer wants to cancel'),
  ('please cancel the appointment', 'this is about billing')) v(state, predicate);
-- one HTTP request, two answers
```

Separate expressions in the same SELECT (`truth(...)` next to `choose(...)`) are separate function calls. They still share the cache when the request body matches. Put both questions in `rvbbit.decide(state, questions)` when you want them fused on purpose.

## Composition

These are scalar expressions. They sit in `WHERE`, `CASE`, `JOIN`, and `GROUP BY` without a special scan.

```sql
SELECT rvbbit.choose(body, ['billing', 'technical', 'other']) AS desk,
       count(*)
FROM tickets
WHERE rvbbit.truth(body, 'a person is asking for something')
GROUP BY 1;

SELECT t.id
FROM tickets t
JOIN desks d
  ON rvbbit.choose(t.body, ['billing', 'technical', 'other']) = d.name;
```

Put cheap predicates first in `WHERE`. The functions are volatile, so the planner will not constant-fold them into one call for the whole query, and it will not treat two different texts as the same value. Within one vector (up to 2048 rows) identical inputs are deduped, and a tree walks every active row of that vector before it moves to the next node.

## What the grammar API allowed

DuckDB 2.0.1's `GrammarExtension` is opt-in (`active_grammar_extensions`) and C++ only. The changes that worked here:

- `AddChoice` on `UnreservedKeyword`, so `SEMANTIC`, `ABOVE`, `DECISION`, `TREE`, and `DECIDE` tokenize as keywords only while the extension is active.
- `AddRule` plus a transform that returns a normal `FunctionExpression`, `ComparisonExpression`, or `SelectStatement`.
- `PrependChoice` on `SingleExpression` for `MEANS` / `ABOUT`, `SEMANTIC CASE`, and `DECIDE name FOR state`.
- `AddChoice` on `Statement` for `CREATE DECISION TREE`.

`MEANS`, `ABOUT`, and `NEAR` are not keywords. The expression is `atom identifier string-literal`, and the transform accepts `means`, `about`, `near`, `supports`, `contradicts`, and `implies`. That keeps `rvbbit.means(...)` working, because the function-name rule still wants an identifier. The left side has to be a column, literal, function call, parenthesized expression, struct, or `CASE`. `lower(body) MEANS 'x'` works. `a || b MEANS 'x'` does not; parenthesize the left side. `SUPPORTS`, `CONTRADICTS`, and `IMPLIES` use the same shape. `ABOVE` applies to `MEANS`, `ABOUT`, and `NEAR`.

`ABOVE` is a keyword, so it can follow the string. `MEANS ... ABOVE n` becomes `rvbbit.truth(left, right, n)`. `ABOUT ... ABOVE n` becomes `rvbbit.about(left, right) > n`. `NEAR ... ABOVE n` becomes `rvbbit.similarity(left, right) > n`.

The API did not give a way to insert a new precedence level into `ComparisonExpression` without replacing that rule and reimplementing every comparison. Reusing `LIKE`'s existing transform was also a poor fit: its generated trampoline only knows the operators it was compiled with, and `LIKE` is not repeatable, so `MEANS` and `ABOVE` could not both be `LIKE`-class operators. A full `WHEN choose(...) = 'billing'` arm inside `SEMANTIC CASE` would need a second expression grammar that overlaps `CASE`. That was left as a function.

Grammar registration does not change parse behavior until the setting names `rvbbit`. If a future 2.0.x build rejects one of these rules, `LOAD rvbbit` still installs the functions; only `SET active_grammar_extensions` would fail. The functions do not depend on the grammar.

## Cache identity

A decision is cached by MD5 of the exact request body (model, state, questions with the evidence boundary already applied). The threshold is not part of that body, so `truth` and `truth_score` share a row. A tree node is cached by the evidence it actually sent plus its question and the prior judgments, not by the whole tree. Failed calls are not cached. Chat operators use the same rule with the chat request body. An embedding is cached by MD5 of `embed`, the model, the dimension, and the text after the mode prefix. A query embedding and a document embedding of the same sentence do not share a row.

Calliope hashes judgment keys with blake3. The fields that go into the key match. The digest here is MD5 because it is already in DuckDB and the cache is local to this extension.
