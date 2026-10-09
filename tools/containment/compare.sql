-- What A's attestation table answers, against what B's containment answers (a walk down from every witness's trunk:
-- laplace replay --tsv --voices --files), on the same sources. Loaded by proof.ps1 into laplace_proto_cmp:
--   a_att   (claim, witness, games, score, pos)   every attestation row of A; pos 0 is none
--   a_src   (source, witness)                     which source each of A's witnesses came in with
--   a_dirs  (witness, dir)                        A's witnesses that are a treebank's directory ({dir})
--   b_rows  (claim, who, games, tokens, score, pos)  who: a trunk, file:PATH, voice:ID; score: the sum of the scores
--   b_src   (source, trunk)                       which source each of B's trunks is
--   a_cons, b_cons                                each side's standings
\pset footer off
\echo '== 1. who said each claim: one row a claim and source'
CREATE TABLE a_by AS
  SELECT a.claim, s.source, sum(a.games) AS games, sum(a.score::float8 * a.games) AS score, min(nullif(a.pos, 0)) AS pos,
         count(*) AS nrows, bool_and(a.score = 1) AS all_wins
  FROM a_att a JOIN a_src s ON s.witness = a.witness GROUP BY 1, 2;
CREATE TABLE b_by AS
  SELECT r.claim, s.source, r.games, r.score, nullif(r.pos, 0) AS pos FROM b_rows r JOIN b_src s ON s.trunk = r.who;
CREATE INDEX ON a_by (claim, source); CREATE INDEX ON b_by (claim, source); ANALYZE a_by; ANALYZE b_by;
SELECT source, count(*) AS a_pairs,
       (SELECT count(*) FROM b_by b WHERE b.source = a.source) AS b_pairs,
       count(*) FILTER (WHERE NOT EXISTS (SELECT 1 FROM b_by b WHERE b.claim = a.claim AND b.source = a.source)) AS only_in_a,
       (SELECT count(*) FROM b_by b WHERE b.source = a.source AND NOT EXISTS (SELECT 1 FROM a_by x WHERE x.claim = b.claim AND x.source = b.source)) AS only_in_b,
       count(*) FILTER (WHERE nrows > 1) AS a_several_witnesses
FROM a_by a GROUP BY source ORDER BY source;
\echo '== 2. games: the records that say it (A: games summed over the source''s witnesses)'
SELECT a.source, count(*) AS pairs, count(*) FILTER (WHERE a.games = b.games) AS games_equal, count(*) FILTER (WHERE a.games <> b.games) AS games_differ,
       sum(a.games) AS a_games, sum(b.games) AS b_games
FROM a_by a JOIN b_by b USING (claim, source) GROUP BY 1 ORDER BY 1;
\echo '== 3. scores: A''s rows keep a real mean (float4) of their games; B''s vertices carry each outcome (exact for win, draw, loss and every score at or above 1/2; within 2^-25 below)'
SELECT a.source, count(*) FILTER (WHERE a.score = b.score) AS sum_equal,
       count(*) FILTER (WHERE a.score <> b.score AND abs(a.score - b.score) <= a.games * 6e-8) AS within_float4_of_a_mean,
       count(*) FILTER (WHERE abs(a.score - b.score) > a.games * 6e-8) AS beyond,
       count(*) FILTER (WHERE NOT a.all_wins) AS pairs_not_all_wins, max(abs(a.score - b.score) / a.games) AS max_per_game
FROM a_by a JOIN b_by b USING (claim, source) GROUP BY 1 ORDER BY 1;
\echo '== 4. positions: A keeps the position the first record that said it gave (per witness); B every record''s, the least read'
SELECT a.source, count(*) FILTER (WHERE a.pos IS NOT NULL) AS a_positioned, count(*) FILTER (WHERE b.pos IS NOT NULL) AS b_positioned,
       count(*) FILTER (WHERE a.pos = b.pos) AS least_equal, count(*) FILTER (WHERE a.pos > b.pos) AS b_lower, count(*) FILTER (WHERE a.pos < b.pos) AS a_lower,
       count(*) FILTER (WHERE (a.pos IS NULL) <> (b.pos IS NULL)) AS one_side_only
FROM a_by a JOIN b_by b USING (claim, source) GROUP BY 1 ORDER BY 1;
\echo '== 5. voices: A''s rows whose witness is a voice (a column of annotators, a speaker), against B''s voice:ID rows of the same claim'
CREATE TABLE bv AS SELECT claim, substr(who, 7) AS voice, games, score, nullif(pos, 0) AS pos FROM b_rows WHERE who LIKE 'voice:%';
CREATE INDEX ON bv (claim, voice); ANALYZE bv;
SELECT count(*) AS a_voice_rows, count(*) FILTER (WHERE a.games = v.games) AS games_equal, count(*) FILTER (WHERE a.games <> v.games) AS games_differ,
       count(*) FILTER (WHERE a.score::float8 * a.games = v.score) AS score_equal,
       (SELECT count(*) FROM bv) AS b_voice_rows
FROM a_att a JOIN bv v ON v.claim = a.claim AND v.voice = a.witness;
\echo '== 6. treebanks: A''s witness per directory, against B''s files under that directory'
CREATE TABLE bd AS SELECT claim, regexp_replace(substr(who, 6), '^.*/([^/]+)/[^/]+$', '\1') AS dir, sum(games) AS games, sum(score) AS score
  FROM b_rows WHERE who LIKE 'file:%' GROUP BY 1, 2;
CREATE INDEX ON bd (claim, dir); ANALYZE bd;
SELECT d.dir, count(*) AS a_rows, count(*) FILTER (WHERE a.games = b.games) AS games_equal, count(*) FILTER (WHERE a.games <> b.games OR b.games IS NULL) AS differ
FROM a_att a JOIN a_dirs d ON d.witness = a.witness LEFT JOIN bd b ON b.claim = a.claim AND b.dir = d.dir GROUP BY 1 ORDER BY 1;
\echo '== 7. standings: bit for bit'
CREATE TABLE several AS SELECT DISTINCT claim FROM a_by WHERE nrows > 1;
SELECT count(*) AS both_sides,
       count(*) FILTER (WHERE a.rating = b.rating AND a.deviation = b.deviation AND a.volatility = b.volatility AND a.matches = b.matches) AS identical,
       count(*) FILTER (WHERE NOT (a.rating = b.rating AND a.deviation = b.deviation AND a.volatility = b.volatility AND a.matches = b.matches)) AS differ,
       count(*) FILTER (WHERE NOT (a.rating = b.rating AND a.deviation = b.deviation AND a.volatility = b.volatility AND a.matches = b.matches) AND s.claim IS NOT NULL) AS differ_several_witnesses_in_a,
       count(*) FILTER (WHERE NOT (a.rating = b.rating AND a.deviation = b.deviation AND a.volatility = b.volatility AND a.matches = b.matches) AND s.claim IS NULL) AS differ_otherwise,
       count(*) FILTER (WHERE a.matches <> b.matches) AS matches_differ
FROM a_cons a JOIN b_cons b USING (claim) LEFT JOIN several s USING (claim);
SELECT (SELECT count(*) FROM a_cons a WHERE NOT EXISTS (SELECT 1 FROM b_cons b WHERE b.claim = a.claim)) AS standings_only_in_a,
       (SELECT count(*) FROM b_cons b WHERE NOT EXISTS (SELECT 1 FROM a_cons a WHERE a.claim = b.claim)) AS standings_only_in_b;
\echo '== 7b. standings that differ though A has one witness a source for the claim: by source, and the claim''s sources'
SELECT sources, count(*) AS claims FROM (
  SELECT a.claim, string_agg(x.source, ', ' ORDER BY x.source) AS sources
  FROM a_cons a JOIN b_cons b USING (claim) JOIN a_by x ON x.claim = a.claim
  WHERE NOT (a.rating = b.rating AND a.deviation = b.deviation AND a.volatility = b.volatility AND a.matches = b.matches)
    AND NOT EXISTS (SELECT 1 FROM several s WHERE s.claim = a.claim)
  GROUP BY a.claim) z GROUP BY 1 ORDER BY 2 DESC LIMIT 20;
