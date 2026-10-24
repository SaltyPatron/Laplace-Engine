# PostgreSQL and PL/pgSQL providers

Source: https://github.com/gmr/tree-sitter-postgres/tree/d6ba3d1f1db62922aa5f2b94c04d33afbcdbc6ed

The four derivation manifests pin the same external dependency and every input
Git blob. The normal builder generates parsers outside the repository:

- postgres: PostgreSQL statements and declaration-language fields;
- postgresql: the same SQL with psql command packaging;
- postgresql_expr: PostgreSQL's PL/pgSQL expression start rule;
- plpgsql: procedural syntax with distinct statement and expression injections.

The patches add field bindings/start rules and distinguish statements from
expressions. Native compiled format profiles select the body grammar from
LANGUAGE, retain decoded scalar products and recurse into actual body syntax.
The original source license is retained in provider.LICENSE. Generated parser
sources and the replaced derekstride SQL provider are not vendored or retained.
