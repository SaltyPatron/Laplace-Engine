`upstream/c.js` is the unchanged MIT-licensed `grammar.js` from
`tree-sitter/tree-sitter-c` at `b780e47fc780ddc8da13afa35a3f4ed5c157823d`,
the revision pinned in Laplace-Operations/grammars.lock.tsv. The local dialect
recognizes the workspace's `LP_API` and `LAPLACE_CHESS_API` export qualifiers
and its conditional C linkage guards.
No preprocessing removes their bytes; the declaration source remains intact.
Generated parser code is kept in the build directory.
