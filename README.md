# Laplace-Engine

Laplace itself: one program, `laplace`, built on [Laplace-Native](https://github.com/SaltyPatron/Laplace-Native) and working against a PostgreSQL database extended by [Laplace-postgres](https://github.com/SaltyPatron/Laplace-postgres). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>; each source under `recipes/` has its page under the wiki's Corpora.

## Commands

| Command | What it does |
| --- | --- |
| `laplace tier0` | generates tier 0 from the Unicode data and prints its fingerprint |
| `laplace deploy` | makes a database a Laplace database: the database itself if the server does not have it, extensions, content schema, semantics, its tier 0 |
| `laplace ingest` | every source, in the order `recipes/order` gives, each with its own log; what is recorded already is passed over |
| `laplace ingest source \| file...` | a source by its name, or files, through their recipes: decompose on every core, deduplicate trunk to leaf, record, attest |
| `laplace index` | the indexes again, if one was dropped; `laplace deploy` makes them all from the start |
| `laplace text text` | a text's ID, tier, coordinate and constituents, computed here without the database |
| `laplace pull prompt` | the forward pass: the prompt broken down, and the segments and strands its firmware takes |
| `laplace hop text` | everything attested about an entity, by how hard each strand tugs back, and the content that holds it |
| `laplace hop subject predicate object` | the claims that hold the given parts in their places, with `?` for a part left open |
| `laplace translate word from to...` | a word up to its concepts and down into other languages, two lookups |
| `laplace degrees from [to]` | how far one entity is from another over rated claims, or what is nearest one |
| `laplace forget witness` | what one witness attested, taken back out, and whatever nothing holds any more with it |
| `laplace sweep` | whatever nothing holds, removed |
| `laplace fills phrase` | what follows a phrase, counted across every source |
| `laplace status` | what a database holds, and whether its tier 0 is this engine's |
| `laplace tree file` | a file's syntax tree as its recipe's grammar reads it, for writing recipes |
| `laplace bench` | every native operation measured on this machine |
| `laplace model dir` | a transformer checkpoint read as "b beats c given a" (needs MKL) |

`laplace ingest --plan` shows which recipe takes which file; `--claims` prints what the recipes attest, as text, and loads nothing.

## From an empty server to loaded content

```sh
./deploy.sh                                        # the database laplace.env names
LAPLACE_CONNINFO="host=/tmp port=5432 user=laplace dbname=NAME" ./deploy.sh
```

`deploy.sh` runs the five steps of Deployment in order: build and install, tier 0 and its flags, `laplace deploy`, `laplace ingest`, then `laplace status` and `laplace bench`. It can be run again: what is built is not rebuilt, what is recorded is passed over by its bytes, and a run that was cut off is taken up where it stopped. Each step's output is kept under `$LAPLACE_WORK/logs/deploy`, and each source's under `$LAPLACE_WORK/logs/ingest`.

A source is not begun when the database's volume has not the room the source was measured to take (`room` in its `source` file); that is said, and the run goes on to the next.

## On the local network

```sh
sudo ./lan.sh                                      # once: the server reachable from the LAN as laplace, scram over TLS
```

`lan.sh` is the one step that needs root, because `pg_hba.conf` and the certificate are in postgres's data directory: it adds the rule for `$LAPLACE_LAN` (default 192.168.1.0/24), makes the certificate, turns `ssl` on, restarts the server so `listen_addresses = '*'` takes effect, and then connects over TCP with the password the way a client on the network would. Its output is kept under `$LAPLACE_WORK/logs/root`. The role's password is in the operator's `~/.pgpass`.

## Firmware

How a pull reads the records is not in the program and not in the records: it is a firmware, a file of decisions, one for each human being. `firmware/program.firmware` is the program's own and says what a firmware can decide; `$LAPLACE_FIRMWARE`, or `--firmware FILE` on `pull`, `hop`, `translate` and `degrees`, names another. The same records pulled under another firmware give another selection, and no standing changes.

## Build

```sh
source laplace.env        # the one definition of where everything is
./build.sh                # the extension and the engine, each with Laplace-Native built as part of it
./build.sh install        # also installs the extension into PostgreSQL's directories
```

The three repositories sit side by side under one source root. Build trees go to `$LAPLACE_BUILD`.

## Recipes

A recipe says how a kind of file decomposes and what it attests. `recipes/*.recipe`; the format is described at the top of `src/recipe.c`. A recipe with a query is a curated source: what is recorded is the claims it attests and the entities they are about, never the file's own identifiers. A `map` binds the source's identifiers to what they stand for, and each kind of claim says the stock default it enters at.

Queries run inside the parts of a file's syntax tree no larger than the recipe's `unit`, in reading order. A pattern should name a record's parts by position (anchors), not pair any two siblings: tree-sitter checks text predicates after it has matched structure, so pairing siblings costs the square of their number. When a unit holds more partial matches than a query keeps, ingestion says so.
