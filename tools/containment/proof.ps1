#Requires -Version 7
<#
  The containment proof (Engine#22, Engine#39 step 2): the same sources ingested by origin/main as built, provenance in
  the attestation table (A, database laplace_proto_a), and by this branch, provenance by containment and no attestation
  table (B, laplace_proto_b); then every read the table serves answered from containment and compared, row for row.
  Scratch databases only: Conn refuses any other name. Each phase leaves its files in -Out for the next.

    pwsh -File tools\containment\proof.ps1 -Phase a|b|compare|forget|reads|all [-Out DIR] [-Sources s1,s2,...]

  The sources go in one at a time, in recipes/order, each by itself (LAPLACE_INGEST_ONE): after each, the witnesses the
  database gained are that source's (A: its witness names and voices; B: its trunk).
#>
param([string]$Phase = 'all', [string]$Out = 'D:\Temp\containment-proof\run',
      [string[]]$Sources = @('unicode', 'iso-639', 'cili', 'open-english-wordnet', 'verbatlas', 'universal-dependencies', 'hatecheck', 'atomic-10x'))
. (Join-Path $PSScriptRoot 'lib.ps1')
$repoB = (Resolve-Path "$PSScriptRoot\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null
$o = $Out -replace '\\', '/'
function Stamp([string]$what) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $what }

function Seed([string]$side) {
    $build = if ($side -eq 'a') { $BuildA } else { $BuildB }; $db = "laplace_proto_$side"
    $env:LAPLACE_WORK = "$o/work-$side"; Recipes $repoB "$Out\recipes" (Join-Path $PSScriptRoot 'overrides'); $env:LAPLACE_RECIPES = "$o/recipes"
    Stamp "drop and deploy $db"; Drop $db | Out-Null
    Laplace $build $db @('deploy') | Select-String "^database|highway's contents|entities and paths"
    $env:LAPLACE_INGEST_ONE = '1'; $seen = @{}; $tsv = "$Out\$side-sources.tsv"; Set-Content $tsv -Value $null
    foreach ($s in $Sources) {
        $t = Get-Date; Laplace $build $db @('ingest', $s) | ForEach-Object { ($_ -split "`r")[-1] } | Set-Content "$Out\$side-ingest-$s.log"
        $sec = ((Get-Date) - $t).TotalSeconds
        $new = Sql $db 'SELECT id FROM witness ORDER BY 1' | Where-Object { $_ -and -not $seen.ContainsKey($_) }
        foreach ($w in $new) { $seen[$w] = $s; Add-Content $tsv "$s`t$w" }
        Stamp ("{0,-28} {1,7:N1} s   {2} witnesses new" -f $s, $sec, @($new).Count)
        Select-String -Path "$Out\$side-ingest-$s.log" -Pattern 'the series, played|witnesses, attestations|a batch at a time|its ID|records met again|could be put in no record|merged .* rows|== total|error|refused' | ForEach-Object { "    " + $_.Line.Trim() }
    }
    Remove-Item Env:\LAPLACE_INGEST_ONE
    Sql $db "\copy (SELECT claim, rating, deviation, volatility, matches FROM consensus) TO '$o/$side-consensus.tsv'" | Out-Null
    if ($side -eq 'a') { Sql $db "\copy (SELECT claim, witness, games, score, coalesce(position, 0) FROM attestation) TO '$o/a-attestation.tsv'" | Out-Null }
    else { Laplace $build $db @('replay', '--tsv', "$Out\b-replay.tsv", '--voices', '--files') | Select-Object -Last 4 | Set-Content "$Out\b-replay.log"; Get-Content "$Out\b-replay.log" }
    Stamp "sizes"; $q = @"
SET maintenance_work_mem = '2GB';
SELECT 'rows', (SELECT count(*) FROM entity), (SELECT count(*) FROM physicality), (SELECT count(*) FROM consensus), (SELECT count(*) FROM witness);
SELECT 'attestation rows', CASE WHEN to_regclass('attestation') IS NULL THEN -1 ELSE (SELECT count(*) FROM attestation) END;
SELECT relname_group, sum(pg_table_size(oid)), sum(pg_indexes_size(oid)) FROM (SELECT regexp_replace(c.relname, '_[0-9a-f]+$', '') AS relname_group, c.oid FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = 'public' AND c.relkind = 'r') x WHERE relname_group IN ('entity', 'physicality', 'attestation', 'consensus', 'witness') GROUP BY 1 ORDER BY 1;
SELECT 'database bytes', pg_database_size(current_database());
"@
    Set-Content "$Out\sizes.sql" $q; SqlFile $db "$Out\sizes.sql" | Set-Content "$Out\$side-sizes.tsv"; Get-Content "$Out\$side-sizes.tsv"
}

function Compare-Sides {
    $db = 'laplace_proto_cmp'; Drop $db | Out-Null
    & $script:Psql -X 'host=127.0.0.1 port=5432 user=laplace dbname=postgres' -qc "CREATE DATABASE $db" | Out-Null
    Stamp "load what A and B answer into $db"
    Sql $db @"
CREATE TABLE a_src (source text, witness text); CREATE TABLE b_src (source text, trunk text); CREATE TABLE a_dirs (witness text, dir text);
CREATE TABLE a_att (claim text, witness text, games bigint, score real, pos int);
CREATE TABLE b_rows (claim text, who text, games bigint, tokens bigint, score float8, pos int);
CREATE TABLE a_cons (claim text, rating float8, deviation float8, volatility float8, matches int);
CREATE TABLE b_cons (claim text, rating float8, deviation float8, volatility float8, matches int);
"@ | Out-Null
    Sql $db "\copy a_src FROM '$o/a-sources.tsv'" | Out-Null; Sql $db "\copy b_src FROM '$o/b-sources.tsv'" | Out-Null
    Sql $db "\copy a_att FROM '$o/a-attestation.tsv'" | Out-Null; Sql $db "\copy b_rows FROM '$o/b-replay.tsv'" | Out-Null
    Sql $db "\copy a_cons FROM '$o/a-consensus.tsv'" | Out-Null; Sql $db "\copy b_cons FROM '$o/b-consensus.tsv'" | Out-Null
    # A's treebank witnesses ({dir}): the ID of each directory's name, as text, computed by the engine
    $dirs = foreach ($d in 'UD_English-EWT', 'UD_English-GUM') { $m = & (Join-Path $BuildB 'laplace.exe') text $d 2>&1 | ForEach-Object { "$_" } | Select-String '^entity\s+([0-9a-f]{32})'; "$($m.Matches[0].Groups[1].Value)`t$d" }
    Set-Content "$Out\a-dirs.tsv" $dirs; Sql $db "\copy a_dirs FROM '$o/a-dirs.tsv'" | Out-Null
    Sql $db "CREATE INDEX ON a_att (claim); CREATE INDEX ON b_rows (claim, who); ANALYZE;" | Out-Null
    $sql = Get-Content (Join-Path $PSScriptRoot 'compare.sql') -Raw
    Set-Content "$Out\compare.sql" $sql; Stamp "compare"; SqlFile $db "$Out\compare.sql" | Tee-Object "$Out\compare.txt"
}

switch ($Phase) {
    'a'       { Seed 'a' }
    'b'       { Seed 'b' }
    'compare' { Compare-Sides }
    'forget'  { & (Join-Path $PSScriptRoot 'forget.ps1') -Out $Out }
    'reads'   { & (Join-Path $PSScriptRoot 'reads.ps1') -Out $Out }
    'all'     { Seed 'a'; Seed 'b'; Compare-Sides; & (Join-Path $PSScriptRoot 'forget.ps1') -Out $Out; & (Join-Path $PSScriptRoot 'reads.ps1') -Out $Out }
}
Stamp "done: $Phase"
