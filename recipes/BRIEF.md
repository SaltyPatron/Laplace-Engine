# Writing recipes for Laplace: the brief

You are writing **recipes** that tell Laplace's engine how to read curated data sets. Read this whole brief first.

## What Laplace records, and the one rule that matters

A source is a **witness**. Laplace records what the witness wrote, exactly as it wrote it. The inventor's words:

> "we record as-is because [the source] is an observation... a witness... why would we make things up and put words in
> their mouth and generate fake placeholder hardcoded records in the universal substrate?"

So:

- **Preserve source notation and state its interpretation explicitly.** `Number=Sing` retains `Number` and `Sing`.
  A field-scoped vocabulary can additionally align a documented category with a shared category; it never rewrites
  arbitrary content words. `type pos upos vocabulary wordnet-pos` maps `n` to `NOUN` and records
  `[[wordnet-pos, n], denotes, NOUN]` under the source. Reusable tables live in `vocabularies/*.tsv`: a `list LIST`
  header followed by unique `SOURCE CANONICAL` pairs. `-` preserves a documented source category without claiming
  equivalence to a shared category. Unknown closed categories are semantic omissions and reject source publication.
- **No predicate comes from the markup**: a header row, an attribute or element name, a JSON key, a `KEY=VALUE` key
  or a name in the file's name says where a value is, never what it means (see "A relation is meaning", below). For a
  table with no header row, column names may come **only** from the source's own documentation (its README, its
  format page, in the set's directory): they name the columns for the recipe's lines, and are in no claim. Quote the
  documentation line in a `#` comment of the recipe. If the source does not name a column anywhere, **do not invent a
  name**: use a pair or a tuple in the row's own order (see `pair`), or report it.
- **Break values down as far as the source's own notation says**, no further: a field of `A=B|C=D` parts is two
  entities `[A, B]` and `[C, D]`, not one string. Stop at words.
- A claim is a **tuple of entities** (`[subject, predicate, object]`, a pair, or a longer path). Every part is content.
- Whatever a source leaves empty (its own empty marker) attests nothing.
- The witness is named **as the source names itself** (its README title, its own `label`), and that name is content
  inside the source's record. The witness is the source's trunk, `[source record, its files' trunks in path order]`
  (`file.c`): content-derived, never a made-up key or a hash of a string, and a new release, having other files, is
  another trunk. The corpus is the trunk: Universal Dependencies, the Open Multilingual Wordnet, the WSD evaluation
  framework and HateCheck are each one source trunk and one witness, and a treebank, a lexicon or a data set under it
  is files, with their paths, under that trunk, never a witness of its own. One organization's corpus set is one
  source: UD's treebanks, its documentation pages (UD's own definitions of each UPOS, relation and feature) and its
  validator data (derived by UD's own system from those pages) are one trunk and one witness; as built,
  `universal-dependencies-tools` and `universal-dependencies-documentation` are sources of their own. ToxiGen is one
  corpus witnessing its records, like a PGN: the generator (model, prompt) and the annotators are participants recorded
  in each record, as White and Black are in a game, and ToxiGen attests that these annotators rated this statement
  so, never that the statement is true. ISO 639 is each authority its own corpus (the Library of Congress's 639-2 with
  the 2-letter codes, SIL's 639-3, IANA's registry, Unicode's CLDR supplemental data, Glottolog), and the 2↔3-letter
  links and macrolanguage relations are relations those corpora attest; as built, the one `iso-639` source names a
  witness per recipe, and since the witness is the source's trunk, those authorities are one witness until the source
  is parted per authority. A claim's standing is stored on every claim, keyed by the claim. Provenance is containment
  (Laplace-Engine#22, proven side by side and the attestation table retired): every part a recipe speaks of is a
  record, a path over the claims it says inside its file's content tree, each claim's vertex carrying its run, outcome
  and position; walking up from a claim finds every source trunk that holds it, and the trunk is the witness, with
  the source's trust and lineage. The `witness` line names the source's record (its trunk's first part).
  Directories, lexicon labels and filenames remain content or packaging under that trunk. A person and the
  judgment, sentence or annotation the source associates with them are content within its records.
- **The corpus is the trunk, from the trunk to its leaves.** Universal Dependencies is one source trunk, `[source
  record, its files' trunks in path order]`, and one witness. Its treebanks (`UD_English-EWT/`) are directories of
  files: a directory's path is a file's metadata, and a treebank is never a witness of its own. Every file is
  `[metadata, content]`, and its README, LICENSE, `stats.xml` and `.txt` files are files under the trunk like the
  `.conllu`. A `.conllu` file's content is its records, one per sentence block, and a record is a path over what the
  corpus says of that sentence: the sentence (its `# text`, down to codepoints); its tokens, as UD segments them; its
  UPOS, XPOS, LEMMA and FEATS layers aligned to the tokens; its dependency tree, each DEPREL by its HEAD; its notes
  (`text_en` and the like) as sentence-tier strands; its speaker and annotators as content the corpus attests
  (`[speaker, sentence]`, `[annotator, sentence]`); and the strands it says of each word (`[forces, NOUN]`,
  `[forces, force]`, `[forces, [Number, Plur]]`). Its pointers (`ID`, `HEAD`, `sent_id`, `newdoc`, `newpar`, the MISC
  ids) resolve into the tree and are not recorded. Token keys are scoped to their sentence. The surface is
  composed from FORM and spacing, including multiword ranges; decimal empty positions participate in the aligned
  layers and enhanced dependencies. The record retains the layers and dependency tree beside its source-attested
  claims. Open Multilingual Wordnet, Open English WordNet and the WSD framework each follow the same single-trunk
  contract; an internal XML lexicon label never registers another witness.
- **Testimony, not bookkeeping.** A witness provides content and attestations about it: that is what a recipe reads.
  A file's bookkeeping about its own records (dates an entry was made, colours, versions, licences, usage notes,
  templates, who edited a row) is not testimony: name it with `omit`, or leave the column out of `attest`. Never
  `attest *`.
- **A source's identifiers are of two kinds: highway nodes, which are content, and its own pointers, which resolve.**
  - **A highway node is content (`highway.bin` Perf-Cache Law).** An ILI (`i46360`), an OEWN synset (`oewn-00001740-n`), a PropBank roleset (`abandon.01`), a VerbNet class (`leave-51.2`), a
    FrameNet frame, frame element or lexical unit, a VerbAtlas frame (`va:0001f`), a language code (`eng`): a vocabulary many
    sources share, the road classes whose lists `laplace highway` holds. Its own text is the node, an entity whose ID is BLAKE3 of
    its canonical Tier-0 composition: `i46360` is `[i,4,6,3,6,0]` exactly as `3.14159` is `[3,.,1,4,1,5,9]`, so every source that cites it
    lands on the same node.
    - **Self-Contained Perf-Cache Invariant:** Requiring the full raw corpus suite on disk just to build or admit `highway.bin`, or dumping a `.nodes` sidecar of raw corpus trees (`highway.bin.nodes`), defeats the purpose of the highway perf-cache. Because every highway node is the Tier-0 composition of its canonical identifier string (deterministic from `tier0.bin` alone), `highway.bin` is a compact, self-contained binary ROM of canonical identifier IDs, fixed-width bit-banger masks (`upos`, `lexfile`, `deprel`, `vnrole`, `kind`), and crosswalk edges.
    - **No Corpus Glosses as Highway Node Identity:** Never use a corpus's prose definition (`skos:definition` in `cili/ili.recipe`, roleset `descr` in `propbank/frames.recipe`, or frame description/name in `verbatlas/frame-info.recipe`) as `key ... node` for a highway identifier. What a highway node *means* (its gloss, definition, examples, relations) is attested as claims about that identifier node during corpus ingestion (**Proof 3**).
    - A source that writes a highway node its own way (`vn:51.3.1` for a VerbNet class) or
    points at one with its own pointer (`mcr:ili-30-01976841-v` is prefix, scheme, release 30, offset and type: a
    pointer, decomposed by that notation, to the ILI node whose own text is `i46360`) names the column with
    `type COLUMN LIST`, which resolves it to that node; the written form is not the node's content and is not recorded.
    A FrameNet numeric `ID` is a pointer, below, not a highway node. Never rewrite a highway node to a made-up key.
    Any publicly known identifier used across sources and languages is a highway node the same way: a FIDE ID is the
    node for a player, as an ILI is for a concept, and the player's names in every language and script are its
    lexicalizations. An identifier is decomposed by its notation (`va:0001f`: namespace `va`, number, kind), and a name
    such as `TOLERATE` is a label attested of the node, used only to realize it: never ask which text "is" the node.

  - **A pointer resolves in its declared scope; an open resource ID does not require a cache slot.** A WordNet synset offset, a synset or sense id, a FrameNet numeric
    `ID`, a UD token number or `sent_id`, a Tatoeba sentence number, a geonameid, a data set's row or case id, a line
    number: how a source addresses its own records (or another release's). It is packaging. A WordNet 3.0 offset is
    the byte position of the synset's line in that release's `data.<pos>` file (byte 1,976,841 of `data.verb` starts
    `01976841 38 v 01 drop`), unique only with its type and only within its release. Name a pointer with `key` (the
    rows that point at it resolve to the thing it defines), `refer` (it is read as the thing another row or element of
    the same source defines) or, for a source's own key of a highway node, `type`: it resolves to the ID of what it
    points at. `refer FIELD TIER under CONTENT_PATH matching PATTERN [as TEMPLATE]` resolves structured notation
    against the same content scope as its declaration. `key TIER FIELD ... retain` indexes a semantic identifier
    without removing its content disposition. PropBank uses these to keep argument slots scoped to their roleset:
    `ARG0` under two different predicate senses does not identify one universal semantic role. Transformed scoped
    references retain `[[scope, original notation], refers to, resolved target]` in source provenance.
    Open identifier lists retain unfamiliar IDs as written, without choosing a nearby known ID or regenerating the
    highway. Closed bank vocabularies still require a valid class. `omit` resolves nothing and is appropriate for
    explicit absence markers such as VerbNet's `fn_mapping="None"`, not unfamiliar real identifiers.
    The identifier's vocabulary is also a source-attested claim, including an explicitly declared `type ... node`
    vocabulary which is not in the cache. Multiple declarations of that classification are one observation per
    record; compiler redundancy must not inflate the series replayed from containment.
    `types`, `keyed`, `alias` and `maps` also emit source-attested vocabulary, key and mapping claims during ordinary
    ingestion. Crosswalk predicates retain both resource vocabularies; a mapping does not assert unrestricted identity.
    Semantic omissions, unread fields, lost provenance and conflicting record metadata return failure and prevent
    source/observation publication. Already stored canonical structure can remain available for reuse.
  - **A pointer's notation still carries facts.** Decompose it by the source's own notation, declared in the recipe,
    never by word breaks, and attest the facts it carries: a WordNet offset's type `v`, a sense key's lexicographer
    file (`drop%2:38:00::` is lemma `drop`, type 2, lexicographer file 38, lex_id 00). The facts are attested; the
    pointer is not. A sense key (`dog%1:05:00::`) is WordNet's pointer to a lexicalization: it is decomposed for its
    facts (lemma, type, lexicographer file), resolved through the highway's perf-cache to the lexicalization, and not
    recorded.
  - **Where a record stands is not an identifier**: its line, its row number, its byte offset, a number that only
    counts a file's own records (a word's `ID` in its sentence, the `-- record N` of `--claims`) are occurrence under
    the file's trunk, never part of any ID.
- **Whoever a corpus names is content; the corpus is the witness.** A worker, an annotator, an author, an owner, a
  contributor, a speaker, a member named inside a corpus is content, not a witness. The corpus witnesses what it
  reports of them, and those reports are relations the corpus attests, added explicitly: this annotator labelled this
  case hateful, this speaker said this sentence, this member added this sentence, this author wrote this book.
  Record the person and their judgment together as content under the corpus's single witness. What the source
  states about the person is attested `of` that content identifier. "Who edited a row" in the bookkeeping list below means housekeeping about the file's
  records, never the person whose words or answers the row holds.
- **A thing is its content.** A word is what the lemma writes; a sentence is its text or its words; a place is its
  name, latitude and longitude; a case or a post is its text. A concept is its highway node: a synset is its ILI, never its word
  list (19,619 of WordNet 3.0's 117,659 synsets list exactly the words another lists; 11 verb synsets are just `drop`).
  `wn-lmf.recipe` resolves a synset's ILI to its canonical highway identifier; its word list is content about it.
  Nothing is order-dependent: every node is content-derived, so what two sources say links by entity collision
  whichever is ingested first, and no placeholder waits for another source. A synset whose ILI is `in` (a new concept
  CILI has not yet numbered) is given no stand-in identity; its content-derived nodes link by collision when CILI
  states its ILI. A word in a language is the lexicalization `[lemma, language, ILI]`, the language (an ISO 639 highway node) a
  part of it, never a context column or a mask bit; another language is the same ILI with another language part.
  Say `identity`, `subject in` or `named ... by` over content, never over a pointer column.
- **Types are attested; the highway is their perf-cache.** Parts of speech, dependency relations, ILI concepts,
  VerbNet classes and roles, FrameNet frames, frame elements and lexical units, PropBank rolesets, VerbAtlas frames are
  listed by `laplace highway`: a perf-cache whose lists resolve the sources' highway nodes to their slots, a stable slot being an
  index over content, never the identity of a meaning. A roleset, a VerbNet class, a FrameNet frame, frame element or
  lexical unit and a VerbAtlas frame are highway nodes exactly as an ILI is: concept nodes of the linguistic highway,
  each kind a road class, which a word goes up to and the interchanges connect. What a node is (an ILI, a roleset, a
  class) is what the source that says so attests. A node holding more claims than a read's fan (NOUN, `eng`) is a
  fan-limited node: a route reaches it and does not cross it. A file that maps highway nodes onto one another (SemLink, PredicateMatrix, CILI's maps, VerbAtlas's
  bridges) is read by the highway through its recipe's `types`, `keyed`, `alias` and `maps` lines
  (`semlink/pb-vn.recipe`); its mappings are the interchanges, and they are what that source attests.
- **An enumerated value lands where the source asserts it.** "dog is a noun" stands in several shapes, each where a
  source says it. A treebank's sentence carries one layer claim per layer (its UPOS, its relations), a path of slots
  aligned to its tokens; what the treebank says of a word by itself is the claim `[dog, NOUN]`, NOUN a registry slot,
  the treebank's taggings of it the claim's games, and the bit on `dog`'s row a filter. UD records the aligned
  sentence layer as well as `[forces, NOUN]`; an annotator's identifier is content, and the UD corpus trunk witnesses
  both. WordNet says the concept is a noun and the word lexicalizes it;
  FrameNet's `dog.n` has the part of speech as a part; a Unicode property is said of the codepoint. Each source's code
  (`n`, `.n`, `NOUN`) is recorded as written and sets the same slot; nothing is blanketed across tiers it was not
  asserted at, and a mask miss is never absence.
- **A repeat is games, not rows.** A witness asserting one claim n times contributes one cumulative series with
  n games and its mean score. Record-path occurrences retain their run and outcome; `laplace_attested_series`
  recovers them from containment. `laplace_attest_series` applies the Native solver continuously in PostgreSQL as
  COPY batches merge. A later ingestion of an already recorded file contributes nothing again. A usage count or
  sense order written by the source is content, not games. Packaging cross products do not manufacture repeated
  observations; calculated annotations retain their declared calculation root.
- **Trust bounds a vote; count is never independence.** Each game's outcome is pulled toward a draw by the witness's
  trust, `s_eff = 0.5 + t(s - 0.5)`, so no repetition takes a source past its trust's ceiling, symmetrically for
  affirmation and refutation, and one witness alone gives a claim only bounded certainty. Witnesses sharing a
  dependence root or a trust class count as one root, capped; a prompt is an observation and attests nothing by being
  said. Independently rooted agreement can raise both the claim's rating and its certainty; repetitions of the same
  source do not become independent witnesses. The Native finite-source joint posterior owns the rating,
  deviation and volatility calculation. A recipe declares the source's trust class and any calculated dependence
  classes; it does not implement its own standing math.
- **A relation is meaning, never a field name.** A field, column, attribute or layer name never becomes a relation:
  what a claim relates is what the source means, resolved through the road classes. A tagset value is a value of its
  tagset with its attested equivalence to the shared vocabulary (FrameNet's BNC layer `NN1` is a CLAWS C5 singular
  common noun, its PENN layer `nns` a Penn Treebank plural common noun, each reaching `NOUN` through that
  equivalence). A per-span label (a frame element, grammatical function or phrase type in one sentence's annotation)
  goes in the sentence's layer, never on the word. A pointer's attribute name (`feID`, `ID`) never appears in a claim.
  The recipe says what each field a claim is made of means, with `value LIST NAME...`: the field's value is a value of
  the road class LIST (a list `laplace highway` holds, such as `upos`, `deprel`, `lexfile`, `ili`; `-` where the
  highway lists none yet and the field's values are the source's own vocabulary), recorded as written. Then
  `attest TIER NAME` gives `[thing, value]` (`[forces, NOUN]`, `[forces, force]`), a `KEY=VALUE` piece of a valued
  field gives `[thing, [KEY, VALUE]]` (`[forces, [Number, Plur]]`), `holds` gives `[thing, other]`, and `relate TIER
  NAME to NAME` takes its relation from a field `value` names (WN-LMF's `relType`, FrameNet's frame-relation type).
  A field that states a relation of the thing to its value (a definition, an example, a name, a form, a number the
  source states of it, a mapping to another resource's node) says it with `relation "TEXT" NAME...`: `[thing, TEXT,
  value]`, where TEXT is the relation **as the source's own documentation words it** (quote the documentation line in a
  `#` comment above), content the source writes, never the field's own name (the engine refuses `relation "lexfile"
  lexfile`). Two sources' wordings of one relation are linked by an attested equivalence, as `n` and `NOUN` are.
  A field a claim names that neither `value` nor `relation` gives a meaning, `relate {file}` and `line ... :: predicate` make **no
  claim**: each is an unresolved obligation, counted by its name and listed at the end of the ingest
  (`== unresolved obligations`). Give a field a meaning only where the source's own documentation says what its
  values are (quote it in a `#` comment); never to restore the header as a relation. Until a field's meaning is
  given (a definition, an example, a number the source states, a per-span label that belongs in its sentence's
  layer, a pointer's attribute such as `feID`), leave it an obligation rather than guess.

These are stated design for recipe authors. The engine does not do all of them yet: read a file this way, and
report where a recipe cannot say it.

- **Seeding is extraction.** A recipe extracts the raw product from the packaging: the content and what the source
  states, out of its formats, records and layout, which stay as provenance with their trunks. The format's grammar
  exposes the structure; the recipe says what each node becomes. Content decomposes to codepoints for identity; a
  claim attaches once, at the tier the source asserts it, never sprayed over the words below it; an XML attribute is
  said of its element. OpenSubtitles says that this sentence translates to that sentence, both languages, at the
  sentence tier.

- **A mapping row is one interchange.** A row that states one correspondence across resources (a PredicateMatrix
  row aligning a VerbNet class, a FrameNet frame, a PropBank roleset and a WordNet sense; a SemLink entry) is one
  correspondence strand, an interchange, a path over the resolved highway nodes, never split into a claim per column.
  Where a file lays its rows out as a cross product (each of one side's members against each of the other's, a row
  apiece), the repeats are the layout's packaging, not repetition. `predicate-matrix/matrix.recipe` reads a row as that
  strand (`thing row ... resolved`): the nodes it aligns and the role it maps in each, and what else the row says is
  said of the strand in the matrix README's words. A key of a version older than the one the highway is built from
  (VerbNet 3.2, FrameNet 1.5, PropBank before its unification) is never mapped by a guess: a roleset or frame, written
  as the node's own text, is that node though no list holds it (`type ... node`), content in the path; a VerbNet class
  number, a pointer to a class no source of the data root has, is left out of the path and owed. No row is lost.
- **A declared default attests nothing.** A value that stands only because the source's format declares it the
  default (Unicode's `nv=NaN`; an `scx` that is only `sc` again) is not something the source says of that thing, just
  as its own empty marker is not.
- **An automatic tagger's layer is a calculation.** Tags a program assigned (a tagger's or parser's per-token parts of
  speech, lemmas, relations) are one calculation with one root, its analyzer's, recorded as a layer per sentence, never
  as testimony per token, and its per-token output is packaging, not repetition.

## Where you work, and what you must not touch

- Recipes go in **`recipes/<source-name>/`** of this repository: a file `source`, and one `.recipe` file per kind of
  file. Work **only inside the source directories you were assigned**. Do not edit other directories there.
- **Never write to the database.** Only ever run the engine with `--claims`, `--no-load` or `--plan`.
- **Never modify** the engine's sources (`src/`), the builds, or another source's recipes. If the engine cannot express what a file
  needs, do not work around it: report exactly what is missing, with three lines of the raw data.
- A source's data is where Laplace-Operations' `data.tsv` declares its set: `$LAPLACE_DATA/<Set>`, fetched from its
  publisher by `data.sh fetch` with an archive opened in place. A source has **one root, that place**: never a
  machine's own path (`/vault/Data/...`), never a dated or staging directory (`.refresh-*`, `extracted`), and no second
  root to fall back to. A recipe that names one machine's directory reads nothing on every other machine, and two
  roots let two machines read two releases of one set. If the set is not declared yet, say so with its publisher's
  URL: the declaration is added first, then the recipe names it.
- Change nothing under `$LAPLACE_DATA`: fetching and opening archives is `data.sh`'s.

## The engine

```sh
. $LAPLACE_SRC/Laplace-Operations/laplace.env          # LAPLACE_SRC, LAPLACE_BUILD, LAPLACE_DATA, the paths
N=$LAPLACE_BUILD/Laplace-Engine/icx-release/laplace
$N ingest --plan   <source-name>                      # which recipe takes which files
$N ingest --claims <source-name> > out.txt 2> err.txt # every claim, as text
$N ingest --no-load <source-name>                     # counts and timings only
$N ingest -s <source-name> --claims FILE...           # files read as that source's (a sample in your scratch directory)
$N tree -g xml|json|turtle|... -n 60 FILE             # a file's syntax tree
```

`--claims` prints one claim per line, `[a, b, c]`, and `-- record N` before the claims of a record. Put scratch output
in the scratch directory you are given, never in the recipes directory. For a very large file, test on the first few
thousand lines copied to your scratch directory (keep the file's name, recipes match by name), then run `--no-load` on
the whole source once at the end.

## The recipe language

The recipe compiler is `src/recipe_compile.c`, with immutable program types in `src/recipe_program.h`.
The grammar walker is `src/structure.c` and its layout is `src/structure.h`. Selector binding, semantic values,
record output and streaming have separate `src/record_*.c` owners; `src/say.c` executes statement instructions.
Working examples to copy from, all under `recipes/`:

| Kind of file | Example |
|---|---|
| table with a header row, every column said of one column's value | `iso-639/iso-639-3.recipe` (`attest *`) |
| table without a header, predicate in the file's name | `princeton-wordnet/exceptions.recipe` |
| table whose pointer column other files point at | `tatoeba/sentences.recipe` (`key`), `tatoeba/links.recipe` (`refer`) |
| table whose column resolves to a highway identifier | `verbatlas/frame-info.recipe` (`va:0001f` is the frame identifier, `TOLERATE` its attested label); `princeton-wordnet/cntlist.recipe` resolves sense-key pointers to lexicalizations |
| table of pairs (nothing written between the two) | `unicode/aliases.recipe` (`pair`) |
| table whose row is one record, said together | use `together` in the claims block |
| records of rows (a sentence, a row per word, a tree by heads) | `universal-dependencies/conllu.recipe` |
| records of `Key: Value` lines | `iso-639/iana.recipe` (`grammar fields`) |
| XML, elements that are things, pointers resolved, highway nodes as a resource writes them | `wn-lmf.recipe` (`key`, `refer`, `type`, `link`), `framenet/framenet.recipe`, `propbank/frames.recipe`, `verbnet/classes.recipe` |
| XML, a sentence as its words | `wsd-evaluation-framework/data.recipe` (`words`) |
| XML by tree-sitter patterns (slower; only when `identity` cannot say it) | `unicode/ucd.recipe` |
| Turtle | `turtle.recipe`, used through `like turtle` (`framebase/schema.recipe`) |
| JSON whose keys are the things | `universal-dependencies-tools/data.recipe` (`keys things`) |
| JSON objects named by members, pointers never recorded; `records` for a value on every line | `wiktionary-kaikki/kaikki.recipe` (`named ... by`, `key`) |
| ordinary text (READMEs, documentation) | add `reads text` to the `source` file |

A `source` file:

```
name my-source
# one or two lines: what it is
witness The Name The Source Gives Itself
# its trust class, one of Laplace-Native/manifest/trust_classes.toml, as the wiki (Sequence: Sources, the estate) gives it for the source
class AcademicCurated
root $LAPLACE_DATA/Set
after unicode iso-639
reads text
```

Notes:
- In a recipe, `#` begins a comment when it starts a line or follows a space. `comment #` and `remark #` are the way to
  name `#` itself as a character.
- Do not use `enter`. `line ... :: predicate TEXT` makes no claim (an obligation): a heading is no relation. Never
  write a word of your own as a relation.
- `match` globs are matched against the file's name only, and the most specific wins. Inside one source make them
  unambiguous.
- Use one witness per corpus, its source trunk, with each file under it by its path. Write the `witness` line as
  the corpus names itself; directory and internal-label substitutions are not source identities.
- Files a source's recipes do not match are simply not read. Your report must list them.

## What to deliver

For **every file** of every set you were assigned, one of: a recipe that reads it, verified with `--claims`; or a line
in your report saying why not (what the engine lacks, or what the source does not document).

Your final report, in plain text, per set:
1. The source directory you wrote and its recipes.
2. `--no-load` totals (files, MB, attestations, seconds).
3. Ten representative claims, copied from `--claims` output.
4. Files not read, each with the reason.
5. Every place you were unsure whether a name was the source's own. Say so plainly; do not guess in the recipe.
6. Engine features that are missing, each with three raw lines of data that need it.

Do not summarize what the data "means" or propose schema changes. The substrate stores entity, physicality,
witness and consensus. A physicality belongs to its entity and has no independent identity. Provenance is
containment under the source trunk; `laplace_attested_series` recovers observations from record paths, and
consensus stores the Native series result keyed by the claim. Recipes do not create separate observation rows.

## Chained Tree-sitter Grammars, Assembly-Line ETL, Bidirectional Tier Traversal & The Three Proofs

1. **Packaging vs. Product: Chained Tree-sitter $\rightarrow$ Laplace Grammars**:
   - Every file is **packaging** opened to extract the raw **product** (canonical content + source-attested claims) while preserving the packaging hierarchy as provenance trunks (`[metadata, content]`).
   - **Multi-Level Grammar Chaining (`ts_parser_set_included_ranges`)**: Outer packaging grammars (`tree-sitter-xml`, `tree-sitter-json`, `tree-sitter-turtle`, `tree-sitter-csv`/`tsv`/`psv`, `tree-sitter-conllu`, `tree-sitter-wndb`, `tree-sitter-pgn`, plus 300+ code grammars) frame records and top-level structure; inner field byte ranges with structured sub-syntax (CoNLL-U `FEATS` `Key=Val|Key=Val`, `DEPS`, `MISC`, JSON/delimited cells inside TSV, or structured identifiers like `drop%2:38:00::` / `mcr:ili-30-01976841-v`) chain directly into child Tree-sitter grammars via `ts_parser_set_included_ranges`, down to UAX #29 text leaves and `tier0.bin` / `highway.bin`.
   - **Compiled Integer Symbol Dispatch**: `.format` and `.recipe` rules compile once at recipe load time against `ts_language_symbol_for_name()` / `ts_language_field_id_for_name()` into $O(1)$ integer `TSSymbol` / `TSFieldId` jump tables. Because Laplace's Merkle DAG *is* an AST, ingest and structured export/realization are exact inverses.
2. **Bidirectional Tier Traversal + Perf-Cache (`tier0.bin` + `highway.bin`)**:
   - **Leaf $\rightarrow$ Trunk (Client-Side Creation & In-RAM Deduplication)**: Content is composed bottom-up from the mmap'd `tier0.bin` and `highway.bin` perf-caches through canonical tier boundaries (UAX #29 for text: `codepoint` $\rightarrow$ `grapheme` $\rightarrow$ `word` $\rightarrow$ `phrase/sentence`; chained Tree-sitter AST nodes for structured records/files). Every repeated character, word, phrase, or record collapses in $O(1)$ in the client's in-RAM `NODE[]` / `staged_set` table before any socket I/O occurs.
   - **Trunk $\rightarrow$ Leaf (Top-Down Self-Reducing Subtree Pruning Against PostgreSQL)**: Deduplication against PostgreSQL proceeds top-down from the highest logical roots (File Trunk $\rightarrow$ Section/Record Trunks $\rightarrow$ Sentences $\rightarrow$ Words), skipping synthetic binary-tree (`blocks_of`) spine nodes. If a node at tier $T$ already exists in PostgreSQL (or in the perf-cache / `staged_set`), its entire descendant subtree is already recorded and is immediately amputated on the client (`keep = recorded`).
3. **True Assembly-Line Record Streaming ETL (Zero Lagged "Consensus Fold")**:
   - **$O(1)$ Pre-Check**: Probe the file trunk / OS metadata before reading or decomposing file bytes; never read or decompose a file twice (`read_file_twice` / `pass == 0` then `pass == 1` is forbidden).
   - **Single-Pass Grammar-Bounded Record Streaming**: Stream files in a single pass on outer grammar record boundaries (XML `<LexicalEntry>` / `<Synset>`, Turtle statement block, JSONL line, CoNLL-U sentence block, PGN game, CSV/TSV row)—never split at arbitrary byte offsets.
   - **Parallel Arena-Allocated Record Workers**: Worker threads parse records via reusable thread-local `TSParser` + compiled integer jump tables + thread-local arenas (`0` `malloc`/`free` calls per record), compose bottom-up into lock-free `NODE[]`/`VTX[]`, and accumulate per-witness `(claim, games, score)` series in-stream. Cross-record pointers (`key` / `refer`) resolve against a lock-free concurrent `key_hash -> EntityID` table—never by grafting a whole-file `STree`/`SNode` DOM in RAM.
   - **Continuous Pipelined `stage.*` COPY & In-DB Series Updates**: Stream novel rows and accumulated `(claim, games, score)` series via binary `COPY` into `UNLOGGED` `stage.*` tables with bounded connection concurrency (no 16-connection `PREPARE TRANSACTION` 2PC on unlogged tables, no lagged end-of-source `standings_write()` "consensus fold", and no client-RAM partition rewrites). Update standings in PostgreSQL via set-based `laplace_attest_series`, merge `stage.*` $\rightarrow$ `public.*` in Hilbert order, clean GIN pending lists, and register the `witness` trunk atomically.
4. **The Three Proofs (Execution Sequence)**:
   - **Proof 1 (`db-roundtrip`)**: Decompose real multi-domain inputs into `entity` + `physicality` and recompose 100% byte-for-byte from PostgreSQL alone while verifying all structural invariants.
   - **Proof 2 (`Linguistic Super Highway`)**: Self-contained `highway.bin` perf-cache (bit-banger masks + cross-source identifier indexing, filtering, and hops) built from canonical identifier strings and crosswalks without requiring raw corpus definitions or `.nodes` sidecar dumps.
   - **Proof 3 (`Full Corpus Ingestion`)**: Full multi-corpus ingestion (`recipes/order`) on top of Proofs 1 and 2, where cross-source claims collide on the deterministic highway identifier nodes and update standings via `laplace_attest_series`.

