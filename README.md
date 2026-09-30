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

`deploy.sh` runs the five steps of Deployment in order: build and install, tier 0 and its flags, the grammars, `laplace deploy`, `laplace ingest`, then `laplace status` and `laplace bench`. It can be run again: what is built is not rebuilt, what is recorded is passed over by its bytes, and a run that was cut off is taken up where it stopped. Each step's output is kept under `$LAPLACE_WORK/logs/deploy`, and each source's under `$LAPLACE_WORK/logs/ingest`.

A source is not begun when the database's volume has not the room the source was measured to take (`room` in its `source` file); that is said, and the run goes on to the next.

## The machine

```sh
sudo ./setup.sh                                    # once per machine: volumes, packages, kernel, dependencies, cluster, settings, access
sudo ./setup.sh settings                           # one part again, after changing its declaration
```

`setup.sh` is the one step that needs root. It is not a sequence of patches: the top of the file declares the machine as sets — the volumes Laplace keeps apart (heap, WAL, temporary files, repositories, data), the packages, the dependencies built from `$LAPLACE_SRC`, the server's settings from Operations: Database, who may connect and how (the operator over the socket; `laplace` from `$LAPLACE_LAN` with scram over TLS; nothing else) — and each part compares its set with the machine and makes only what is missing. Run again on a finished machine it changes nothing and reports. What it did is kept under `$LAPLACE_WORK/logs/root`; the role's password is in the operator's `~/.pgpass`. Then `./deploy.sh`.

Every name of the machine's is a variable of `laplace.env` with this installation's value as its default: the paths (`LAPLACE_PREFIX`, `LAPLACE_PG_DIR`, `LAPLACE_PGDATA`, `LAPLACE_PGWAL`, `LAPLACE_PGTEMP`), the users and groups (`LAPLACE_PGUSER`, `LAPLACE_ROLE`, `LAPLACE_GROUP`, `LAPLACE_PG_GROUP`), the service (`LAPLACE_PGSERVICE`), the volumes (`LAPLACE_VOLUMES`), the network (`LAPLACE_LAN`) and the agent. Another machine sets its own before running `setup.sh`.

### The agents

```sh
sudo ./agents.sh                                   # one GitHub Actions runner per repository of $LAPLACE_REPOS, as laplace-runner
sudo ./agents.sh status                            # each, as this machine and as GitHub see it
sudo ./agents.sh remove Laplace-Wiki               # unregister and remove one (or all)
```

`agents.sh` gives every repository its own runner on this machine, all as `$LAPLACE_AGENT_USER` (default `laplace-runner`) under `$LAPLACE_AGENT_HOME/<repo>`: its own directory, environment (`runner/.env`: its builds and work under its directory; the machine's dependencies, data and database), registration (named `<host>-<repo>`, labelled `laplace,<repo>`), service, and rights — the Engine's runner alone may run `setup.sh` of its checkout as root and restart the server; the others have nothing beyond the shared group. Registration takes a token per repository: `gh` logged in as an administrator, or `LAPLACE_AGENT_TOKEN_<REPO>`. Laplace-postgres and Laplace-Engine check out Laplace-Native (and Laplace-postgres) beside themselves; they are private, so those repositories need the secret `LAPLACE_CHECKOUT`, which `agents.sh` sets from `LAPLACE_CHECKOUT_TOKEN` when given. Each repository's `.github/workflows/laplace.yml` is its job on its runner: Native builds with icx and gcc and runs its tests under both; postgres builds, installs into the extension directories by rename (no root: the group's right) and updates the extension in the database; Engine runs `setup.sh` and `deploy.sh`; Wiki builds the site. The operator and the agent share `$LAPLACE_GROUP`: every shared tree is that group's, set-group-id and group-writable, so neither meets a permission.

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
