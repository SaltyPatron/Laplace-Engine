# Writing recipes for Laplace: the brief

You are writing **recipes** that tell Laplace's engine how to read curated data sets. Read this whole brief first.

## What Laplace records, and the one rule that matters

A source is a **witness**. Laplace records what the witness wrote, exactly as it wrote it. The inventor's words:

> "we record as-is because [the source] is an observation... a witness... why would we make things up and put words in
> their mouth and generate fake placeholder hardcoded records in the universal substrate?"

So:

- **Nothing is renamed, normalized, abbreviated, expanded, translated, re-cased or re-spelled.** `Number=Sing` is
  recorded as `Number` and `Sing`, never as "singular". `Ref_Name` stays `Ref_Name`, never "Reference Name".
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
  the source's trust and lineage. The `witness` line names the source's record (its trunk's first part); `{dir}`,
  `{name}` and `{first NAME}` now only name what voices are composed under, and the treebank or file a claim came from
  is the file's path under the trunk. Who in a record says a claim (`voices`, `by`) is a voice vertex in the record:
  content of it, never a witness.
- **The corpus is the trunk, from the trunk to its leaves.** Universal Dependencies is one source trunk, `[source
  record, its files' trunks in path order]`, and one witness. Its treebanks (`UD_English-EWT/`) are directories of
  files: a directory's path is a file's metadata, and a treebank is never a witness of its own. Every file is
  `[metadata, content]`, and its README, LICENSE, `stats.xml` and `.txt` files are files under the trunk like the
  `.conllu`. A `.conllu` file's content is its records, one per sentence block, and a record is a path over what the
  corpus says of that sentence: the sentence (its `# text`, down to codepoints); its tokens, as UD segments them; its
  UPOS, XPOS, LEMMA and FEATS layers aligned to the tokens; its dependency tree, each DEPREL by its HEAD; its notes
  (`text_en` and the like) as sentence-tier strands; its speaker and annotators as content the corpus attests
  (`[sentence, speaker, SP]`, `[token, Annotator, Sv]`); and the strands it says of each word (`[forces, NOUN]`,
  `[forces, force]`, `[forces, Number=Plur]`). Its pointers (`ID`, `HEAD`, `sent_id`, `newdoc`, `newpar`, the MISC
  ids) resolve into the tree and are not recorded. As built, `universal-dependencies/source` says `witness {dir}`
  (each treebank its own witness), `conllu.recipe` names `speaker_id` and MISC `Annotator` as witnesses of their own
  (`own`, `by`), emits claims as events outside the tree without composing the layers, and says
  `relate row DEPREL to HEAD`, which makes the type-level `[word, nsubj, head word]` and loses the sentence; the
  READMEs are not read. The Open Multilingual Wordnet is likewise one trunk and one witness, its lexicons files under
  it; its `witness {first label}` (each lexicon a witness) is the same mistake as built.
- **Testimony, not bookkeeping.** A witness provides content and attestations about it: that is what a recipe reads.
  A file's bookkeeping about its own records (dates an entry was made, colours, versions, licences, usage notes,
  templates, who edited a row) is not testimony: name it with `omit`, or leave the column out of `attest`. Never
  `attest *`.
- **A source's identifiers are of two kinds: highway nodes, which are content, and its own pointers, which resolve.**
  - **A highway node is content.** An ILI (`i46360`), a PropBank roleset (`abandon.01`), a VerbNet class (`leave-51.2`), a
    FrameNet frame, frame element or lexical unit, a VerbAtlas frame (`va:0001f`), a language code: a vocabulary many
    sources share, the road classes whose lists `laplace highway` holds. Its own text is the node, an entity whose ID is BLAKE3 of
    its content: `i46360` is `[i,4,6,3,6,0]` exactly as `3.14159` is `[3,.,1,4,1,5,9]`, so every source that cites it
    lands on the same node. A source that writes a highway node its own way (`vn:51.3.1` for a VerbNet class) or
    points at one with its own pointer (`mcr:ili-30-01976841-v` is prefix, scheme, release 30, offset and type: a
    pointer, decomposed by that notation, to the ILI node whose own text is `i46360`) names the column with
    `type COLUMN LIST`, which resolves it to that node; the written form is not the node's content and is not recorded.
    A FrameNet numeric `ID` is a pointer, below, not a highway node. Never rewrite a highway node to a made-up key.
    Any publicly known identifier used across sources and languages is a highway node the same way: a FIDE ID is the
    node for a player, as an ILI is for a concept, and the player's names in every language and script are its
    lexicalizations. An identifier is decomposed by its notation (`va:0001f`: namespace `va`, number, kind), and a name
    such as `TOLERATE` is a label attested of the node, used only to realize it: never ask which text "is" the node.
  - **A pointer resolves, and is recorded nowhere.** A WordNet synset offset, a synset or sense id, a FrameNet numeric
    `ID`, a UD token number or `sent_id`, a Tatoeba sentence number, a geonameid, a data set's row or case id, a line
    number: how a source addresses its own records (or another release's). It is packaging. A WordNet 3.0 offset is
    the byte position of the synset's line in that release's `data.<pos>` file (byte 1,976,841 of `data.verb` starts
    `01976841 38 v 01 drop`), unique only with its type and only within its release. Name a pointer with `key` (the
    rows that point at it resolve to the thing it defines), `refer` (it is read as the thing another row or element of
    the same source defines) or, for a source's own key of a highway node, `type`: it resolves to the ID of what it
    points at and nothing of it is recorded. `omit` resolves nothing: it drops the part, and nothing reads it (`say.c`).
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
  case hateful, this speaker said this sentence, this member added this sentence, this author wrote this book. As
  built, `own` (`[the source's witness, COLUMN, id]`) names such a person as a witness of its own, `by` (or `voices`,
  for a column per annotator) attributes what they said to that witness, and what the source says of them is attested
  `of` them; the target is the corpus as the one witness, with the person and what the corpus reports of them as
  content and relations under it. "Who edited a row" in the bookkeeping list below means housekeeping about the file's
  records, never the person whose words or answers the row holds.
- **A thing is its content.** A word is what the lemma writes; a sentence is its text or its words; a place is its
  name, latitude and longitude; a case or a post is its text. A concept is its highway node: a synset is its ILI, never its word
  list (19,619 of WordNet 3.0's 117,659 synsets list exactly the words another lists; 11 verb synsets are just `drop`).
  As built, `wn-lmf.recipe` makes a synset the words it lists and reads its `ili` with `type`; the ILI is the target.
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
  the treebank's taggings of it the claim's games, and the bit on `dog`'s row a filter. As built,
  `attest row LEMMA UPOS XPOS FEATS by MISC.Annotator` (`universal-dependencies/conllu.recipe`) says `[forces, UPOS,
  NOUN]` of each token row, witnessed by the annotator MISC names or else by the treebank, and no layer claim is
  composed. WordNet says the concept is a noun and the word lexicalizes it;
  FrameNet's `dog.n` has the part of speech as a part; a Unicode property is said of the codepoint. Each source's code
  (`n`, `.n`, `NOUN`) is recorded as written and sets the same slot; nothing is blanketed across tiers it was not
  asserted at, and a mask miss is never absence.
- **A repeat is games, not rows.** If WordNet says a dog is a noun 30 times, that is one attestation with 30 games:
  a witness that asserts one claim n times gives one attestation of n games with a score, and the client folds the
  source's repeats and solves that series as one update, so the database takes one update per claim per witness: the
  rating where the claim's prior standing and the series' score agree (the log posterior is concave, so bisection on
  its slope converges), with the deviation from the series' games. It is not Glicko-2's single period step, one
  linearized move that is fine for small moves and wrong for a long series: a 450 player against a 3400 player, 1000
  games, 100 wins and 100 draws, score 0.15, played game by game converges to 3100 with deviation 15; solved as one
  update, 3092 with deviation 16; one period step from 450 gives 105,823. Run length is content structure only, identical consecutive children in a path; it attests
  nothing. A number the source states (a usage count, a sense's order) is content, an observation, never games.
  Packaging is not the source asserting again: a cross-product layout or an automatic tagger's per-token output
  repeating one fact is not the source saying it again (UD EWT's README says its UPOS and features were mainly
  assigned automatically). As built (`db.c`, `standings_write`), what a source says is folded over all its batches,
  each claim's games the records that say it and its score their mean, and played once, at the source's trust, as one
  solved series (`lp_attest_series`); a later ingestion of the same files says nothing again.
- **Trust bounds a vote; count is never independence.** Each game's outcome is pulled toward a draw by the witness's
  trust, `s_eff = 0.5 + t(s - 0.5)`, so no repetition takes a source past its trust's ceiling, symmetrically for
  affirmation and refutation, and one witness alone gives a claim only bounded certainty. Witnesses sharing a
  dependence root or a trust class count as one root, capped; a prompt is an observation and attests nothing by being
  said. A witness's trust is its own rating, from its class's default, earned only through independent agreement and
  verified outcomes, never by repeating itself. As built, `lp_attest` plays every witness as a fixed 1500 opponent
  whose trust sets only its deviation, and a claim enters at that deviation, so an affirming series leaves a claim
  higher the lower the trust (1000 games: trust 0.2 ends at 3235, trust 0.9 at 1884). A recipe says nothing of trust
  beyond its source's class.
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
  apiece), the repeats are the layout's packaging, not repetition. As built, `predicate-matrix/matrix.recipe` attests each column of a row
  (`attest row *`).
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

The full reference is the comment at the top of **`/repos/src/Laplace-Engine/src/recipe.c`**; the one decomposer is
described at the top of `structure.c` and its layout in `structure.h`, and what each part of a file is (content, key,
omit, attest...) at the top of `say.c`, in the same directory. **Read those comments.**
Working examples to copy from, all under `recipes/`:

| Kind of file | Example |
|---|---|
| table with a header row, every column said of one column's value | `iso-639/iso-639-3.recipe` (`attest *`) |
| table without a header, predicate in the file's name | `princeton-wordnet/exceptions.recipe` |
| table whose pointer column other files point at | `tatoeba/sentences.recipe` (`key`), `tatoeba/links.recipe` (`refer`) |
| table whose column is a source's key that `type` resolves to a highway node | `verbatlas/frame-info.recipe` (`type "prototypical synset" ili`; as built the frame's `va:0001f` is `key` and the frame is its name; the target is the frame as its node `va:0001f`, its name `TOLERATE` a label), `princeton-wordnet/cntlist.recipe` (as built, `type sense_key ili`; the target resolves a sense key to its lexicalization strand) |
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
- As built, a set can give a witness per file or directory, named with `{dir}` or `{first NAME}` (a treebank, a
  lexicon or a data set each its own witness). The target is one witness per corpus, its source trunk, with each file
  under it by its path; write the `witness` line as the corpus names itself.
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

Do not summarize what the data "means". Do not propose schema changes. As built there are five database tables
(entity, physicality, witness, attestation, consensus); a physicality is the entity's row, keyed by its ID, with no
identity of its own, and attestations are of entities only. The target is provenance by containment under the source's
trunk, with standing stored in one table keyed by the claim (Laplace-Engine#22); the per-claim, per-witness attestation
rows stay until a working prototype on real data shows containment answers everything they answer with nothing lost. A
recipe is written the same for both.
