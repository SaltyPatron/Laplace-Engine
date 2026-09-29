# Laplace-Engine

Laplace itself: one program, `laplace`, built on [Laplace-Native](https://github.com/SaltyPatron/Laplace-Native) and working against a PostgreSQL database extended by [Laplace-postgres](https://github.com/SaltyPatron/Laplace-postgres). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki).

## Commands

| Command | What it does |
| --- | --- |
| `laplace tier0` | generates tier 0 from the Unicode data and prints its fingerprint |
| `laplace deploy` | makes a database a Laplace database: extensions, content schema, semantics, its tier 0 |
| `laplace ingest file...` | files through their recipes: decompose on every core, deduplicate trunk to leaf, record, attest |
| `laplace index` | builds the container (GIN) and 4D (GiST) indexes after a bulk load |
| `laplace text text` | a text's ID, tier, coordinate and constituents, computed here without the database |
| `laplace hop text` | everything attested about an entity, by how hard each strand tugs back, and the content that holds it |
| `laplace pull from [to]` | fans out from an entity over rated claims, or finds the chain between two |
| `laplace fills phrase` | what follows a phrase, counted across every source |
| `laplace status` | what a database holds, and whether its tier 0 is this engine's |
| `laplace tree file` | a file's syntax tree as its recipe's grammar reads it, for writing recipes |
| `laplace bench` | every native operation measured on this machine |
| `laplace model dir` | a transformer checkpoint read as "b beats c given a" (needs MKL) |

`laplace ingest --plan` shows which recipe takes which file; `--claims` prints what the recipes attest, as text, and loads nothing.

## Build

```sh
source laplace.env        # the one definition of where everything is
./build.sh                # the extension and the engine, each with Laplace-Native built as part of it
./build.sh install        # also installs the extension into PostgreSQL's directories
```

The three repositories sit side by side under one source root. Build trees go to `$LAPLACE_BUILD`.

## Recipes

A recipe says how a kind of file decomposes and what it attests. `recipes/*.recipe`; the format is described at the top of `src/recipe.c`. A recipe with a query is a curated source: what is recorded is the claims it attests and the entities they relate.

Queries run inside the parts of a file's syntax tree no larger than the recipe's `unit`, in reading order. A pattern should name a record's parts by position (anchors), not pair any two siblings: tree-sitter checks text predicates after it has matched structure, so pairing siblings costs the square of their number. When a unit holds more partial matches than a query keeps, ingestion says so.
