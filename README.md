# Laplace-Engine

Laplace itself: one program, `laplace`, built on [Laplace-Native](https://github.com/SaltyPatron/Laplace-Native) and working against a PostgreSQL database extended by [Laplace-postgres](https://github.com/SaltyPatron/Laplace-postgres). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>; each source under `recipes/` has its page under the wiki's Corpora.

## Closed parallel publication

Creation composes leaves upward; recorded-subtree pruning still runs trunks
downward. Publication uses the actual newly admitted DAG, independently of
semantic tier. Native `lp_dag_levels` computes its dependency barriers before
any writes. A cross-lane dependency requires an earlier committed wave;
dependencies inside one lane can share its atomic entity/path transaction.
Cycles fail before publication. Runs remain compressed during scheduling.

Each bounded libpq lane owns its staging pair and publishes through the same
`laplace_merge_stage` owner. Canonical ID prefixes assign disjoint destination
leaves; rows within each lane remain Hilbert ordered. A useful transport window
uses COPY's existing 4 MiB grain to size active concurrency. Small windows use
one writer; this affects execution grain, never admission or identity.

Every child wave finishes before a parent wave begins. Interruption may retain
complete branches, while a later replay still descends through an absent trunk.
No two-phase commit is needed. Observation totals commit in private UNLOGGED
tables with each working window, preserving wins directly rather than recovering
them from rounded means. The client then releases its observation map. Streaming
holds no public consensus bucket locks and no source-wide transaction.

Final registration publishes the canonical witness and authoritative standings
in one read-committed transaction. Each window already recorded its changed
claim IDs; the Native indexed upward provenance operator resolves those claims
against all registered witnesses, including concurrently completed sources.
Only pruned files need stored claim discovery. Newly ingested source contents
are never walked downward again for publication. `tests/observation-windows.py`
observes committed windows through a second connection, checks lock availability,
rejects a repeated downward walk and compares final standings against complete
Native evidence after concurrent publication and partial replay.

The ingest phase report separates Native preparation, staging COPY and paired
publication. `tests/publication-waves.py` uses real PostgreSQL databases to
compare serial and parallel stored state, observe subtree closure during writes,
kill and resume a writer, and verify unchanged replay. Native's DAG tests also
exercise shared/repeated edges, owner-local cohorts, cycles and a 200,000-node
dependency chain without C recursion.

Recipe references compile once when the registry loads. Native
`lp_graph_components` groups logical cycles and orders external providers before
their consumers. Each recipe keeps its resulting dependency level. Source-file
listing and ingestion use that same level; ingestion then orders by stated
witness trust and original file position with one sort. The existing compiled
continuation solver resolves declarations inside a cyclic group without
reopening source bytes. `tests/recipe-order.py` checks a 40-recipe chain, a cyclic
pair with external providers, file-start order, and all 43 resolved claims.

`src/pg_plan.c` owns prepared SQL by connection lifetime and copied SQL contents.
libpq reset/destruction events release its Native map. A reused connection
address or mutable query buffer cannot select another session's statement.
The driver leaves custom/generic planning and parallel-append policy to the
declared database settings; connection setup does not override them. The live
`pg_plan_lifecycle` acceptance covers reset, multiple connections, query-buffer
reuse and retention of the caller's planner policy.

The same connection owner discovers Native and PostGIS namespaces and binary
type OIDs from `pg_catalog`. SQL templates explicitly name their installed
tables, functions, types and operators through `@extschema@` and `@postgis@`;
no search-path change is needed. Binary ID parameters bind when sent, so one
argument set can be reused against separate databases with different OIDs.
Reset reconnects and rebinds; finish releases bindings and rendered statements.
Read scopes use the same connection owner to export or import a read-only,
repeatable-read snapshot. Pool scope completion releases transactions while
retaining connections, namespace bindings and plans for the next read.
`tests/db-binding.py` installs both extensions in separate named schemas with
conflicting public objects, then exercises ingestion, replay, reads, indexing
and source removal through the production Engine.

`src/read_ids.c` owns client projection of exact-ID sets for the Engine and
MCP. Tiers, coordinates and ordered physicalities use the shared PostgreSQL
`laplace_entities` / `laplace_physicalities` readers, on the caller's connection
and snapshot. Request ordinals preserve duplicate occurrences; absent IDs stay
absent. Tier-0 coordinates and tiers come directly from the mapped cache.
Transfer windows bound binary transport without limiting the accepted set.
The same native primary-key kernel serves presence, DAG realization and the
monorepo's single/batch physicality adapter. Clients do not route exact IDs to
named partitions or open another connection pool for these reads.

`laplace deploy` installs or upgrades the extensions without adopting existing
tables or changing database planner, memory or cache configuration. Host
installers explicitly use `deploy --configure-server-cache` to persist their
declared server-local `LAPLACE_TIER0`, `LAPLACE_FLAGS` and `LAPLACE_HIGHWAY`
paths together. A remote client must leave that flag out. `laplace index`
uses configured maintenance memory and analyzes only the Native relations.
For installation beside existing application objects, `laplace deploy --schema
laplace` creates Native objects in that named schema. Existing Native and
PostGIS namespaces are discovered automatically. A conflicting `--schema` on
an existing installation fails without moving its objects.

Claim readers keep complete Native-decoded tuples in one allocation, with no
fixed operand count. Sorting and filtering move descriptors while their operand
arena stays owned by the returned result. Chain traversal returns an explicit
standing summary and reads fallback subjects as a set. Positional provenance
selectors also retain every supplied operand. First, last and any-part lookups
share that reader, including translation. `tests/claim-width.py` reads a real
80-operand PostgreSQL claim and follows the true endpoint of a 5,003-operand relation.
Observation counts retain PostgreSQL's signed 64-bit representation through
held rows, claim results, traversal summaries and CLI formatting.
Parsed value lists and compound identities share an arena composer sized from
their actual arity. `tests/record-values-width.py` checks complete 64-, 65- and
512-member records with one and four workers.

`src/ingest.c` finds recorded file trunks from the OS metadata before opening
source bytes. Each level reads distinct metadata IDs as a set through the path
index, while retaining every selected file occurrence associated with each ID.
The frontier read reuses its connection-owned prepared statement.
Traversal tracks `(metadata ID, file occurrence)` pairs, so converging ancestry
and repeated paths are read once without losing ownership or imposing a depth
cap. The transport window bounds request bytes, not the admitted ancestry.
`tests/file-precheck.py` exercises three real files, two of them hard links,
unchanged replay, and 32 levels of canonical recorded metadata. It checks exact
selected trunks and complete stored subtrees in a private database.

`src/db.c` owns one metadata DAG reader for recorded-file context checks and
compiled-key restoration. It fetches complete ID frontiers through Native's
indexed path API, reads shared metadata nodes once, and stops at context/key
entries before following their referenced product. Root visits preserve each
file's scope. Context validation starts at the selected file metadata, avoiding
a database-wide upward walk from the common tag. Key fields retain their stored
order and runs. `tests/metadata-replay.py` uses actual Tatoeba recipes to compare
unchanged replay, cached-provider restoration and changed-provider context
invalidation against cold database readings.

`src/read_text.c` owns selected-ID CLI text and parts. One append-only child
arena retains ordered runs for every fetched node; traversal uses an explicit
occurrence stack. Only newly discovered IDs form another database frontier.
Metadata reads, selected text frontiers and COPY share the declared 4 MiB
transport grain, with windows over complete inputs. Display byte limits unwind
active branches safely for later reads. Claim and voice annotations retain their
existing role outside the CLI's content text. `tests/reader-depth.py` qualifies
4,096 genuine nested tiers, repeated reads without another fetch, bounded
projection of a 100,000-occurrence run, display limits, Unicode and cycle rejection.

Native's shared Windows process manifest preserves UTF-8 before CRT argument
conversion. `tests/utf8-input.py` compares scalar/grapheme argument identities
against raw UTF-8 bytes and verifies byte-exact parsing through a Unicode path.

## Commands

| Command | What it does |
| --- | --- |
| `laplace tier0` | generates tier 0 from the Unicode data and prints its fingerprint |
| `laplace highway` | generates the highway: the types the resources list (parts of speech, relations, concepts, classes, frames, rolesets) and the mappings between them, a perf-cache beside tier 0 |
| `laplace deploy` | creates the database if absent; installs or upgrades Native/PostGIS objects and indexes in their installed namespaces |
| `laplace ingest --catalog` | all available sources, short aliases, content paths, recipes, grammars, filename patterns and prerequisites as JSON; no database, cache or corpus required |
| `laplace ingest --list-sources` | every canonical source name from the same recipe estate |
| `laplace sources --catalog` | the same complete available catalog |
| `laplace ingest` | every source, in the order `recipes/order` gives, each with its own log; what is recorded already is passed over |
| `laplace ingest source \| file...` | a source by its name, or files, through their recipes: decompose on every core, deduplicate trunk to leaf, record, attest |
| `laplace index` | the indexes again, if one was dropped; `laplace deploy` makes them all from the start |
| `laplace text text` | a text's ID, tier, coordinate and constituents, computed here without the database |
| `laplace pull prompt` | the forward pass: the prompt broken down, and the segments and strands its firmware takes |
| `laplace firmware` | inspect the compiled query relation IDs without opening a database |
| `laplace hop text` | everything attested about an entity, by how hard each strand tugs back, and the content that holds it |
| `laplace hop subject predicate object` | claims whose first, middle and last parts match, across tuple arities; `?` leaves a selector open |
| `laplace translate word from to...` | a word up to its concepts and down into other languages, two lookups |
| `laplace degrees from [to]` | how far one entity is from another over rated claims, or what is nearest one |
| `laplace forget witness` | what one witness attested, taken back out, and whatever nothing holds any more with it |
| `laplace sweep` | whatever nothing holds, removed |
| `laplace fills phrase` | what follows a phrase, counted across every source |
| `laplace status` | what a database holds, and whether its tier 0 is this engine's |
| `laplace tree file` | a file's syntax tree as its recipe's grammar reads it, for writing recipes |
| `laplace bench` | every native operation measured on this machine |
| `laplace model dir` | a transformer checkpoint read as "b beats c given a" (needs MKL) |

Use catalog aliases such as `ucd`, `iso639`, `ud`, `oewn`, and `omw` wherever a source name is accepted. `uca` is another alias for `unicode`, rather than a separate scheduled ingestion. The recipe estate owns the full alias list.

`laplace ingest --plan` shows which recipe takes which file; `--claims` prints what the recipes attest, as text, and loads nothing.

Ingestion requires Laplace-postgres 1.37 or later. Native tier descent classifies the
formed working set once; binary COPY carries those classifications into private
`UNLOGGED` staging. New IDs carry their canonical payload. Known IDs carry changed
metadata and any path annotations, without resending coordinates or Hilbert values.
Metadata already acknowledged in this loader is omitted when unchanged; repeated
observations still contribute their testimony and games. The shared Native merge
verifies concurrent overlap only for claimed-new IDs before plain inserts and
addressed metadata updates. Batch logs report complete payloads, existing metadata
IDs, annotation overlays, tier rounds, SQL commands and COPY streams. Command counts
exclude startup and source registration and are not measurements of network round
trips.

The read commands (`hop`, `translate`, `degrees`, `pull`) plan trips. The road classes are the identifier systems, the highway's type lists: words in each language, a wordnet's synsets, the ILI, ISO 639 languages, FrameNet frames, frame elements and lexical units, VerbNet classes and roles, PropBank rolesets, VerbAtlas frames, parts of speech, dependency relations, lexicographer files. The interchanges are the mapping strands where a route changes class: lexicalizations, CILI's maps, SemLink, the Predicate Matrix, MapNet, VerbAtlas, WordFrameNet, FrameBase. A road's speed is its confidence, each hop pays a turn penalty, and a node holding more claims than the firmware's fan (NOUN, `eng`) is a fan-limited node, reached and not crossed. A* plans a trip that changes class at interchanges, with highway nodes as the landmarks that give its heuristic, and the highway ROM (`laplace highway`: the type lists and the mapping edges) is the precomputed freeway network. As built, the reads run Dijkstra, best-first with an estimate of 0, over claims read from the database, and never walk the highway ROM's edges. The acceptance trip is `dog` → its OEWN synset → `i46360` → `[犬, jpn]`, printed leg by leg.

## The machine, the deployment, the build

Setting up the machine, building, deploying from an empty server, and the repositories' agents are Laplace-Operations
(`setup.sh`, `build.sh`, `deploy.sh`, `agents.sh`, and `laplace.env`, the one definition of where everything is). The
engine alone builds with CMake as any of the three code repositories does:

```sh
source ../Laplace-Operations/laplace.env
cmake -S . -B "$LAPLACE_BUILD/Laplace-Engine/icx-release" -G Ninja -DCMAKE_C_COMPILER=icx -DCMAKE_CXX_COMPILER=icpx -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$LAPLACE_BUILD/Laplace-Engine/icx-release"
```

## Generation, deployment and admission

`laplace tier0` and `laplace highway` generate the two client-side memory-mapped perf-caches (`tier0.bin` and `highway.bin`). Every highway node (`ili`, `Synset`, `fnframe`, `fnfe`, `fnlu`, `vnclass`, `pbroleset`, `vaframe`, ISO 639 language code) is the canonical Tier-0 composition of its own identifier string (`i46360`, `oewn-00001740-n`, `abandon.01`, `va:0001f`, `leave-51.2`, `eng`), deterministic from `tier0.bin` alone, with fixed-width bit-banger masks (`upos`, `lexfile`, `deprel`, `vnrole`, `kind`) and crosswalk interchanges baked into `highway.bin`. Generating or using `highway.bin` does not require raw corpus definitions on disk or a `.nodes` sidecar dump.

CILI's published WordNet 3.0, WordNet 3.1 and Open Dutch WordNet 1.3 maps resolve release-specific pointers through the `wordnet-3.0`, `wordnet-3.1` and `odwn-1.3` key namespaces to existing ILI records. These keys add neither entities nor mask slots. `highway --refresh` reads the maps through their recipes; ordinary `highway` reconstructs keys from Native's frozen `manifest/slots/keys.tsv.gz`. That metadata accepts `namespace/key/target-list/target-slot` tab-separated rows alongside the original `list/key/slot` rows. Cross-list keys select the self-contained `LPHW0003` envelope, requiring Native's namespace-aware reader; caches with only original rows retain `LPHW0002`.

A source pointer may name several concepts: CILI's WordNet 3.1 map contains 47 such pointers. Every distinct target is retained and can be enumerated through Native. A recipe's singular `type` lookup reports an ambiguous pointer as an unresolved obligation and makes no selection; provenance commands require an explicit canonical target. Neither row order nor a composed surrogate chooses a concept.

`laplace deploy` creates or updates extensions, schema and indexes. Cache selection belongs to the host installer through `--configure-server-cache`; deployment does not ingest content. Tier-0 and Highway identities remain available through their memory-mapped Native caches.


## Firmware

How a pull reads the records is not in the program and not in the records: it is a firmware, a file of decisions, one for each human being. `firmware/program.firmware` is the program's own and says what a firmware can decide; `$LAPLACE_FIRMWARE`, or `--firmware FILE` on `pull`, `hop`, `translate` and `degrees`, names another. The same records pulled under another firmware give another selection, and no standing changes.

## Recipes

A recipe says how a kind of file decomposes and what it attests. `recipes/*.recipe`; the format is described at the top of `src/recipe.c`. A recipe that says what a file's parts are is a curated source: what is recorded is the file's tree as its recipe reads it, over its metadata tree, and the claims it attests. A source's identifiers are of two kinds. A highway node (an ILI, a PropBank roleset, a VerbNet class, a FrameNet frame, frame element or lexical unit, a VerbAtlas frame, a language code) is content: its own text is the node, the same entity in every source that cites it; the highway's lists are the perf-cache that resolves highway nodes to their slots, and what a node is is what a source attests. A pointer (a WordNet offset, a synset or sense id, a FrameNet numeric id, a token number or `sent_id`, a row or line number, a source's own key for a highway node such as `mcr:ili-30-01976841-v`) is how a source addresses its own records or another release's: `key` and `refer` resolve it to the ID of what it points at, `type` resolves a source's key for a highway node to that node, and none of them records anything of it; the facts its notation carries (a WordNet offset's part of speech, a sense key's lexicographer file) are decomposed by that notation and attested. Where a record stands in its file is occurrence, never identity. A sense key is WordNet's pointer to a lexicalization: decomposed for its facts, resolved through the highway, not recorded. The witness is the source's trunk, `[source record, its files' trunks]` (`src/file.c`), whose record holds the source's name as content: one corpus is one trunk and one witness, its treebanks, lexicons and data sets are files under it, and the annotators, workers, speakers and members it names are content, and what it reports of them are relations it attests. Provenance is containment (`LP_SAID_RECORD` and `LP_SAID_CLAIM` vertices under each file's content tree, walked by `src/walk.c` and `laplace_attested` / `laplace_attribution` in `Laplace-postgres`), with standing stored on every claim in `consensus`; the retired `attestation` table is gone. For ordinary files without an extension Match (`laplace ingest file`), `detect_structure_recipe` in `src/recipe.c` inspects the file's structure and selects the matching stock recipe (`xml`, `json`, `jsonl`, `tsv`, `csv`, `markdown`, `turtle`, `text`) so structured files are decomposed by their structural layout and only leaf text passes through UAX #29. A claim a witness asserts n times is one attestation with n games and a score, a series the client solves as one update, the rating where the claim's prior standing and the series' score agree, never Glicko-2's single linearized period step, which is wrong for a long series (450 against 3400 over 1000 games scoring 0.15: game by game 3100, solved 3092, one period step 105,823); each game's outcome is pulled toward a draw by the witness's trust, so no repetition passes its trust's ceiling, and witnesses sharing a dependence root or trust class count as one capped root. A relation is what the source means, never a field's, column's, attribute's or layer's name, as built `[dog, BNC, NN1]` ([recipes/BRIEF.md](recipes/BRIEF.md)). Seeding is extraction: a recipe takes the content and what the source states out of the packaging, a claim attaches once at the tier the source asserts it, and a mapping row is one interchange strand, never a claim per column.

Queries run inside the parts of a file's syntax tree no larger than the recipe's `unit`, in reading order. A pattern should name a record's parts by position (anchors), not pair any two siblings: tree-sitter checks text predicates after it has matched structure, so pairing siblings costs the square of their number. When a unit holds more partial matches than a query keeps, ingestion says so.

## Packaging vs. Product, Chained Tree-sitter Grammars & Assembly-Line ETL

1. **Chained Tree-sitter $\rightarrow$ Laplace Grammars (Bidirectional Ingest & Export)**:
   - Every file is **packaging** opened by a compiled Tree-sitter grammar (`tree-sitter-xml`, `tree-sitter-json`, `tree-sitter-turtle`, `tree-sitter-csv`/`tsv`/`psv`, `tree-sitter-conllu`, `tree-sitter-wndb`, `tree-sitter-pgn`, plus 300+ code grammars) into a `TSTree` / `TSNode` of byte spans with integer `TSSymbol` and `TSFieldId` IDs.
   - Field spans with internal sub-syntax (CoNLL-U `FEATS`, `DEPS`, `MISC`, JSON/delimited cells inside TSV, or structured identifiers like `drop%2:38:00::` / `mcr:ili-30-01976841-v`) chain directly into child Tree-sitter grammars via `ts_parser_set_included_ranges`, down to UAX #29 text leaves and the `tier0.bin` / `highway.bin` perf-caches.
   - `.format` and `.recipe` files compile once at load time into $O(1)$ integer `TSSymbol` / `TSFieldId` jump tables. Because Laplace's Merkle DAG *is* an AST, ingest (`Bytes -> Chained Tree-sitter AST -> Laplace Merkle DAG + Claims`) and export/realization (`Laplace Merkle DAG -> AST -> Formatted Bytes`) are exact inverses.
2. **True Assembly-Line Record Streaming ETL**:
   - Stream files in a single pass on outer grammar record boundaries $\rightarrow$ parallel worker threads parse records via reusable thread-local `TSParser` + compiled integer jump tables + thread-local arenas (`0` `malloc`/`free` calls per record) $\rightarrow$ compose bottom-up from `tier0.bin` + `highway.bin` into lock-free `NODE[]`/`VTX[]` while accumulating per-witness `(claim, games, score)` series in-stream $\rightarrow$ prune known subtrees top-down (`Trunk -> Leaf`) against PostgreSQL $\rightarrow$ stream novel rows and series updates continuously via binary `COPY` into `UNLOGGED` `stage.*` tables and update standings in PostgreSQL via `laplace_attest_series`.
3. **Deprecated Legacy Code (Slated for Removal — Do Not Extend)**:
   - `STree` / `SNode` intermediate heap DOM (`g_node`, `s_grammar_split`, `copy_around`, `graft`, `copy_rec` in `src/structure.c`).
   - Bespoke hand-rolled byte scanners (`s_decompose`, `tier_read`, `part_end`, `mini_json`, `parts_of` in `src/structure.c`), the `csv`/`tsv`/`psv` Tree-sitter block in `src/recipe.c:322`, and duplicate hardcoded C walkers (`attest_json_node`, `attest_xml_node`, `attest_delimited`, `attest_code_node` in `src/recipe.c`).
   - Per-node runtime string building and per-row heap churn in `src/say.c` (`child_path`, `dis_scan`, `owed`, and per-row `records_of` `calloc`/`free` arrays).
   - Two-pass stretch reads (`pass 0` + `table_reset` + `pass 1`), lagged end-of-source `standings_write()` ("consensus fold"), 16-connection Two-Phase Commit (`PREPARE TRANSACTION`) on `UNLOGGED` `stage.*` tables, and client-RAM partition rewrites (`L->rewrite`) in `src/ingest.c` and `src/db.c`.
