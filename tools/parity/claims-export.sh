#!/usr/bin/env bash
# What one pipeline claims of a source, as text, for claims.py (claim-level parity, Laplace#1879, #68). IDs differ
# between the engine and the monorepo by construction; the claims' text does not.
#
#   claims-export.sh engine SOURCE OUT.tsv FILE...       laplace ingest --claims-tsv -s SOURCE FILE... (nothing loaded)
#   claims-export.sh mono DBNAME OUT.tsv WITNESS...      the attestations of those witnesses: subject, relation, object,
#                                                        observations; each as its rendered text, else its display label
#   claims-export.sh mono-witnesses DBNAME               every witness of the monorepo database: id, label, attestations
#
# A WITNESS is a source id in hex (32 digits) or a source name (laplace.source_id(name)). The monorepo side only reads
# (a read-only session); the connection is PGHOST/PGUSER/PGPORT as libpq takes them, the database named. Text is
# written as COPY writes it: a tab, line end or backslash escaped as \t, \n, \\, the same as --claims-tsv.
# LAPLACE_ENGINE_BIN is the engine for the engine side (default laplace on PATH).
set -euo pipefail
kind=${1:?usage: claims-export.sh engine|mono|mono-witnesses ...}; shift
psql=${PSQL:-psql}
q(){ PGOPTIONS="${PGOPTIONS:-} -c default_transaction_read_only=on" "$psql" -XqAt -v ON_ERROR_STOP=1 -d "$1" -c "$2"; }
case "$kind" in
  engine)
    src=${1:?source}; out=${2:?output file}; shift 2; [ $# -gt 0 ] || { echo "claims-export.sh engine: name the files" >&2; exit 2; }
    "${LAPLACE_ENGINE_BIN:-laplace}" ingest --claims-tsv -s "$src" "$@" > "$out.raw" 2> "$out.log" || { tail -20 "$out.log" "$out.raw" >&2; exit 1; }
    grep $'^claim\t' "$out.raw" > "$out" || true; rm -f "$out.raw"
    printf '%s\t%s claims\n' "$src" "$(wc -l < "$out")" ;;
  mono)
    db=${1:?database}; out=${2:?output file}; shift 2; [ $# -gt 0 ] || { echo "claims-export.sh mono: name the witnesses" >&2; exit 2; }
    ids=""
    for w in "$@"; do
      if [[ "$w" =~ ^[0-9a-fA-F]{32}$ ]]; then ids+="${ids:+,}decode('$w','hex')"
      else ids+="${ids:+,}laplace.source_id('${w//\'/\'\'}')"; fi
    done
    sql="WITH a AS (SELECT subject_id s, type_id t, object_id o, sum(observation_count) n FROM laplace.attestations WHERE source_id = ANY (ARRAY[$ids]) GROUP BY 1, 2, 3),
      ids AS (SELECT array_agg(DISTINCT x) xs FROM a CROSS JOIN LATERAL (VALUES (s), (t), (o)) v(x) WHERE x IS NOT NULL),
      r AS (SELECT u.x, u.txt FROM ids, LATERAL unnest(ids.xs, realize.render_batch(ids.xs)) u(x, txt)),
      l AS (SELECT d.id x, d.label FROM ids, LATERAL realize.display_label_batch(ids.xs) d),
      nm AS (SELECT r.x, coalesce(nullif(r.txt, ''), l.label, encode(r.x, 'hex')) txt, coalesce(nullif(l.label, ''), r.txt, encode(r.x, 'hex')) label FROM r LEFT JOIN l USING (x))
      SELECT ns.txt, nt.label, coalesce(no.txt, ''), a.n FROM a JOIN nm ns ON ns.x = a.s JOIN nm nt ON nt.x = a.t LEFT JOIN nm no ON no.x = a.o"
    q "$db" "\\copy ($(tr '\n' ' ' <<<"$sql")) TO '$out'"   # \copy reads one line
    printf '%s\t%s claims\n' "$db" "$(wc -l < "$out")" ;;
  mono-witnesses)
    db=${1:?database}
    q "$db" "WITH w AS (SELECT source_id, count(*) n FROM laplace.attestations GROUP BY 1)
      SELECT encode(w.source_id, 'hex'), coalesce(l.label, ''), w.n FROM w LEFT JOIN realize.display_label_batch((SELECT array_agg(source_id) FROM w)) l ON l.id = w.source_id ORDER BY w.n DESC" ;;
  *) echo "claims-export.sh: engine, mono or mono-witnesses, not $kind" >&2; exit 2 ;;
esac