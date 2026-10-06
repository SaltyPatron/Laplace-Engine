WITH t AS (
  SELECT CASE WHEN c.relname ~ '^entity_' THEN 'entity' WHEN c.relname ~ '^physicality_' THEN 'physicality'
              WHEN c.relname ~ '^attestation_' THEN 'attestation' WHEN c.relname ~ '^consensus_' THEN 'consensus' ELSE c.relname END AS tbl,
         c.oid
  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
  WHERE n.nspname = 'public' AND c.relkind = 'r' AND (c.relname ~ '^(entity|physicality)_[0-9a-f]{2}$' OR c.relname ~ '^(attestation|consensus)_[0-9a-f]$' OR c.relname = 'witness'))
SELECT tbl, sum(pg_table_size(oid)) AS heap_bytes, sum(pg_indexes_size(oid)) AS index_bytes,
       sum(pg_total_relation_size(oid)) AS total_bytes
FROM t GROUP BY tbl ORDER BY tbl;
SELECT 'rows', (SELECT count(*) FROM entity), (SELECT count(*) FROM physicality), (SELECT count(*) FROM attestation), (SELECT count(*) FROM consensus), (SELECT count(*) FROM witness);
SELECT 'gin physicality_paths', sum(pg_relation_size(i.indexrelid)) FROM pg_index i JOIN pg_class c ON c.oid = i.indexrelid WHERE c.relname ~ '^physicality_[0-9a-f]{2}_path' ;
SELECT 'db', pg_database_size(current_database());
