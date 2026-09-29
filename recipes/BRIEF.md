# Writing recipes for Laplace: the brief

You are writing **recipes** that tell Laplace's engine how to read curated data sets. Read this whole brief first.

## What Laplace records, and the one rule that matters

A source is a **witness**. Laplace records what the witness wrote, exactly as it wrote it. The inventor's words:

> "we record as-is because [the source] is an observation... a witness... why would we make things up and put words in
> their mouth and generate fake placeholder hardcoded records in the universal substrate?"

So:

- **Nothing is renamed, normalized, abbreviated, expanded, translated, re-cased or re-spelled.** `Number=Sing` is
  recorded as `Number` and `Sing`, never as "singular". `Ref_Name` stays `Ref_Name`, never "Reference Name".
- **Every predicate comes from the source itself**: a header row, an attribute or element name, a JSON key, a
  `KEY=VALUE` key, a name in the file's name. For a table with no header row, column names may come **only** from the
  source's own documentation (its README, its format page, in the set's directory). Quote the documentation line in a
  `#` comment of the recipe. If the source does not name a column anywhere, **do not invent a name**: use a pair or a
  tuple in the row's own order (see `pair`, and `subject in / predicate in / object in`), or report it.
- **Break values down as far as the source's own notation says**, no further: a field of `A=B|C=D` parts is four
  entities and two claims, not one string. Stop at words.
- A claim is a **tuple of entities** (`[subject, predicate, object]`, a pair, or a longer path). Every part is content.
- Whatever a source leaves empty (its own empty marker) attests nothing.
- The witness is named **as the source names itself** (its README title, its own `label`).
- Do not decide what is "useful". If the source says something of a thing, it is attested.

## Where you work, and what you must not touch

- Recipes go in **`/repos/work/recipes-next/<source-name>/`**: a file `source`, and one `.recipe` file per kind of file.
  Work **only inside the source directories you were assigned**. Do not edit other directories there.
- **Never write to the database.** Only ever run the engine with `--claims`, `--no-load` or `--plan`.
- **Never modify** anything under `/repos/src`, `/repos/build`, or the engine. If the engine cannot express what a file
  needs, do not work around it: report exactly what is missing, with three lines of the raw data.
- Under `/vault/Data` you may only **extract an archive** into a directory named `extracted` beside the archive (this is
  the existing convention, see `/vault/Data/.refresh-20260903/CILI/extracted`). Change nothing else there.
- Prefer the refreshed copy of a set in `/vault/Data/.refresh-20260903/<Set>` when one exists; otherwise `/vault/Data/<Set>`.

## The engine

```sh
source /repos/src/Laplace-Engine/laplace.env
N=/repos/build/Laplace-Engine/icx-next/laplace
$N ingest -r /repos/work/recipes-next --plan   <source-name>     # which recipe takes which files
$N ingest -r /repos/work/recipes-next --claims <source-name> > out.txt 2> err.txt   # every claim, as text
$N ingest -r /repos/work/recipes-next --no-load <source-name>    # counts and timings only
$N tree -g xml|json|turtle|... -n 60 FILE                        # a file's syntax tree
```

`--claims` prints one claim per line, `[a, b, c]`, and `-- record N` before the claims of a record. Put scratch output
in the scratch directory you are given, never in the recipes directory. For a very large file, test on the first few
thousand lines copied to your scratch directory (keep the file's name, recipes match by name), then run `--no-load` on
the whole source once at the end.

## The recipe language

The full reference is the comment at the top of **`/repos/src/Laplace-Engine/src/recipe.c`**; the native readers are
described at the top of `records.c`, `elements.c` and `members.c` in the same directory. **Read those four comments.**
Working examples to copy from, all in `/repos/work/recipes-next/`:

| Kind of file | Example |
|---|---|
| table with a header row, every column said of one column's value | `iso-639/iso-639-3.recipe` (`attest *`) |
| table without a header, predicate in the file's name, a score column | `cili/maps.recipe` |
| table of pairs (nothing written between the two) | `unicode/aliases.recipe` (`pair`) |
| table whose row is one record, said together | use `together` in the claims block |
| records of rows (a sentence, a row per word, a tree by heads) | `universal-dependencies/conllu.recipe` |
| records of `Key: Value` lines | `iso-639/iana.recipe` (`grammar fields`) |
| XML, elements that are things named by an attribute | `propbank/frames.recipe`, `verbnet/classes.recipe` (`identity`, `link`) |
| XML by tree-sitter patterns (slower; only when `identity` cannot say it) | `wn-lmf.recipe`, `unicode/ucd.recipe` |
| Turtle | `turtle.recipe`, used through `like turtle` (`cili/ili.recipe`) |
| JSON whose keys are the things | `semlink/mappings.recipe` (`keys things`) |
| JSON objects named by a member; `records` for a value on every line | `grammar json` + `identity KEY...` |
| ordinary text (READMEs, documentation) | add `reads text` to the `source` file |

A `source` file:

```
name my-source
# one or two lines: what it is
witness The Name The Source Gives Itself
# The deviation is this recipe's choice for a curated academic resource; the specification does not give one.
deviation 90
root $LAPLACE_DATA/.refresh-*/Set/extracted
root $LAPLACE_DATA/Set
after unicode iso-639
reads text
```

Notes:
- In a recipe, `#` begins a comment when it starts a line or follows a space. `comment #` and `remark #` are the way to
  name `#` itself as a character.
- Do not use `enter`. Do not use `predicate TEXT` with a word of your own: only with a name the source's own
  documentation gives, quoted in a comment.
- `match` globs are matched against the file's name only, and the most specific wins. Inside one source make them
  unambiguous.
- A set that gives several witnesses (one per file or directory) names them with `{dir}` or `{first NAME}`.
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

Do not summarize what the data "means". Do not propose schema changes. There are exactly six database tables and
there will be no others.
