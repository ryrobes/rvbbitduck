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

`MEANS` and `ABOUT` are not keywords. The expression is `atom identifier string-literal`, and the transform accepts only those two words. That keeps `rvbbit.means(...)` working, because the function-name rule still wants an identifier. The left side has to be a column, literal, function call, parenthesized expression, struct, or `CASE`. `lower(body) MEANS 'x'` works. `a || b MEANS 'x'` does not; parenthesize the left side. `SUPPORTS`, `CONTRADICTS`, and `IMPLIES` use the same shape. `ABOVE` applies to `MEANS` and `ABOUT` only.

`ABOVE` is a keyword, so it can follow the string. `MEANS ... ABOVE n` becomes `rvbbit.truth(left, right, n)`. `ABOUT ... ABOVE n` becomes `rvbbit.about(left, right) > n`.

The API did not give a way to insert a new precedence level into `ComparisonExpression` without replacing that rule and reimplementing every comparison. Reusing `LIKE`'s existing transform was also a poor fit: its generated trampoline only knows the operators it was compiled with, and `LIKE` is not repeatable, so `MEANS` and `ABOVE` could not both be `LIKE`-class operators. A full `WHEN choose(...) = 'billing'` arm inside `SEMANTIC CASE` would need a second expression grammar that overlaps `CASE`. That was left as a function.

Grammar registration does not change parse behavior until the setting names `rvbbit`. If a future 2.0.x build rejects one of these rules, `LOAD rvbbit` still installs the functions; only `SET active_grammar_extensions` would fail. The functions do not depend on the grammar.

## Cache identity

A decision is cached by MD5 of the exact request body (model, state, questions with the evidence boundary already applied). The threshold is not part of that body, so `truth` and `truth_score` share a row. A tree node is cached by the evidence it actually sent plus its question and the prior judgments, not by the whole tree. Failed calls are not cached. Chat operators use the same rule with the chat request body.

Calliope hashes judgment keys with blake3. The fields that go into the key match. The digest here is MD5 because it is already in DuckDB and the cache is local to this extension.
