# Laplace-Engine

Laplace itself: one program, `laplace`, built on [Laplace-Native](https://github.com/SaltyPatron/Laplace-Native) and working against a PostgreSQL database extended by [Laplace-postgres](https://github.com/SaltyPatron/Laplace-postgres). The documentation is [Laplace-Wiki](https://github.com/SaltyPatron/Laplace-Wiki), published at <https://saltypatron.github.io/Laplace-Wiki/>; each source under `recipes/` has its page under the wiki's Corpora.

## Commands

| Command | What it does |
| --- | --- |
| `laplace tier0` | generates tier 0 from the Unicode data and prints its fingerprint |
| `laplace highway` | generates the highway: the types the resources list (parts of speech, relations, concepts, classes, frames, rolesets) and the mappings between them, a perf-cache beside tier 0 |
| `laplace deploy` | makes a database a Laplace database: the database itself if the server does not have it, extensions, content schema, semantics, its tier 0 |
| `laplace ingest` | every source, in the order `recipes/order` gives, each with its own log; what is recorded already is passed over |
| `laplace ingest source \| file...` | a source by its name, or files, through their recipes: decompose on every core, deduplicate trunk to leaf, record, attest |
| `laplace index` | the indexes again, if one was dropped; `laplace deploy` makes them all from the start |
| `laplace text text` | a text's ID, tier, coordinate and constituents, computed here without the database |
| `laplace pull prompt` | the forward pass: the prompt broken down, and the segments and strands its firmware takes |
| `laplace hop text` | everything attested about an entity, by how hard each strand tugs back, and the content that holds it |
| `laplace hop subject predicate object` | the claims that hold the given parts in their places, with `?` for a part left open; a claim is a tuple or longer path of any arity, and this asks for those with three parts |
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

## Firmware

How a pull reads the records is not in the program and not in the records: it is a firmware, a file of decisions, one for each human being. `firmware/program.firmware` is the program's own and says what a firmware can decide; `$LAPLACE_FIRMWARE`, or `--firmware FILE` on `pull`, `hop`, `translate` and `degrees`, names another. The same records pulled under another firmware give another selection, and no standing changes.

## Recipes

A recipe says how a kind of file decomposes and what it attests. `recipes/*.recipe`; the format is described at the top of `src/recipe.c`. A recipe that says what a file's parts are is a curated source: what is recorded is the file's tree as its recipe reads it, over its metadata tree, and the claims it attests. A source's identifiers are of two kinds. A highway node (an ILI, a PropBank roleset, a VerbNet class, a FrameNet frame, frame element or lexical unit, a VerbAtlas frame, a language code) is content: its own text is the node, the same entity in every source that cites it, and `type` reads a source's way of writing one as that node; the highway's lists are the perf-cache that resolves highway nodes to their slots, and what a node is is what a source attests. A pointer (a WordNet offset, a synset or sense id, a FrameNet numeric id, a token number or `sent_id`, a row or line number) is how a source addresses its own records: `key` and `refer` resolve it to the ID of what it points at and record nothing of it, and the facts its notation carries (a WordNet offset's part of speech, a sense key's lexicographer file) are decomposed by that notation and attested. Where a record stands in its file is occurrence, never identity. A sense key is WordNet's pointer to a lexicalization: decomposed for its facts, resolved through the highway, not recorded. The witness is the source's trunk, `[source record, its files' trunks]` (`src/file.c`), whose record holds the source's name as content: one corpus is one trunk and one witness, its treebanks, lexicons and data sets are files under it, and the annotators, workers, speakers and members it names are content, and what it reports of them are relations it attests. As built, the engine cites the name's text as the witness (`named_for`, `src/recipe.c`), can name a witness per file or directory (`{dir}`, `{first NAME}`) and per annotator (`own`, `by`, `voices`), and keeps provenance in attestation rows, one per claim and witness, with its games, score and position. The target is provenance by containment, a record a path over the claims it asserts under its file's trunk, with standing stored on every claim (#22); the attestation rows stay until a working prototype on real data shows containment answers everything they answer with nothing lost. A claim a witness asserts n times is one strand with run length n: the witness plays one matchup per claim per ingestion, with n as the certainty of its assertion, never n Glicko-2 games, and the run count is kept beside the claim; as built, each record's attestation plays as its own matchup and the attestation row counts its games. Seeding is extraction: a recipe takes the content and what the source states out of the packaging, a claim attaches once at the tier the source asserts it, and a mapping row is one interchange strand, never a claim per column.

Queries run inside the parts of a file's syntax tree no larger than the recipe's `unit`, in reading order. A pattern should name a record's parts by position (anchors), not pair any two siblings: tree-sitter checks text predicates after it has matched structure, so pairing siblings costs the square of their number. When a unit holds more partial matches than a query keeps, ingestion says so.
