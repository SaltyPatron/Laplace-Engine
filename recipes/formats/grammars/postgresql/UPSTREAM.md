The base SQL grammar and dollar-quote scanner are the MIT-licensed sources from
`derekstride/tree-sitter-sql` at `c2e1e08db1ea20dc23bdb8d228a81a8756e9c450`,
the revision pinned by Laplace-Operations/grammars.lock.tsv. The upstream files
are unchanged; `grammar.js` is stored as `upstream/sql.js` to distinguish the
base module from the PostgreSQL dialect entry point. `source-lock.json` records
the upstream Git blob IDs. No generated parser is kept in the repository.

The dialect adds psql backslash commands, PostgreSQL's C function linkage
(`AS 'library', 'entrypoint'`), and anonymous `DO` blocks. Procedural block bodies
are retained as language-specific string literals, as PostgreSQL specifies;
they are not misparsed as standalone SQL statements. Other SQL productions
remain the upstream rules, with PostgreSQL `ALTER COLUMN TYPE ... USING`,
function-valued CHECK constraints and the FROM/FOR substring argument form.
