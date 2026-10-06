#Requires -Version 7
<#
  Engine#22, side by side on real data: UD_English-EWT (release 2.18) ingested as built, provenance in the attestation
  table (A, database laplace_proto_a), and by containment (B, laplace_proto_b): each record a path over what the
  treebank says of its sentence, under its file's content tree, under the source's trunk, which is the witness.
  Both databases are dropped and made here, and left for inspection; no other database is touched.

    pwsh -File tools\containment-prototype\run.ps1 [-Build DIR] [-Out DIR]

  -Build: where this branch's laplace.exe is (cmake -S . -B DIR -G Ninja with Laplace-Operations' toolchain file).
  -Out:   where the scratch recipes, logs and the compared files go.
#>
param([string]$Build = 'D:\Libraries\build\Laplace\wt-containment', [string]$Out = "$env:TEMP\containment-prototype")
$ErrorActionPreference = 'Continue'                                      # the engine writes its progress on stderr
. D:\Libraries\load-env.ps1 *> $null                                       # the developer shell: oneAPI's run-time DLLs
. (Join-Path $env:LAPLACE_SRC 'Laplace-Operations\laplace.env.ps1')
$env:LAPLACE_DATA = 'D:/Data/Ingest'                                        # the corpora, whatever the environment held before
$env:PATH = "$Build;$env:LAPLACE_PG_DIR\bin;$env:LAPLACE_ICU_DIR\bin64;$env:LAPLACE_ZLIB_DIR\bin;$env:PATH"
foreach ($d in 'libpq.dll', 'z.dll', 'icuuc78.dll', 'mkl_rt.3.dll', 'libmmd.dll', 'libiomp5md.dll') { if (-not (Get-Command $d -ErrorAction SilentlyContinue)) { "$d is not on the PATH: not run"; exit 1 } }
$here = $PSScriptRoot; $repo = (Resolve-Path "$here\..\..").Path
New-Item -ItemType Directory -Force $Out | Out-Null; $o = $Out -replace '\\', '/'
$env:LAPLACE_WORK = "$o/work"; $env:LAPLACE_CONNINFO = 'host=127.0.0.1 port=5432 user=laplace dbname=laplace_proto_none'
$psql = Join-Path $env:LAPLACE_PG_DIR 'bin\psql.exe'
function Conn([string]$db) { if ($db -notmatch '^laplace_proto_[ab]$') { throw "not a prototype database: $db" }; "host=127.0.0.1 port=5432 user=laplace dbname=$db" }
function Laplace { & "$Build\laplace.exe" @args 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^OMP: ' } }
function Sql([string]$db, [string]$q) { & $psql -X (Conn $db) -At -F "`t" -c $q }
function Log { Get-Content "$Out\work\logs\ingest\universal-dependencies.log" | ForEach-Object { ($_ -split "`r")[-1] } | Select-String 'decompose \(|deduplication|COPY into|witnesses, att|source.s trunk|its ID|merged|== total|the witness' }

# the scratch recipes: the branch's, with the source pointed at EWT alone (A), and its recipe disposing by containment (B)
foreach ($s in 'a', 'b') { $r = "$Out\recipes-$s"; if (Test-Path $r) { Remove-Item -Recurse -Force $r }
    Copy-Item -Recurse "$repo\recipes" $r; Copy-Item -Force "$here\$s\universal-dependencies\*" "$r\universal-dependencies\" }

$A = Conn laplace_proto_a; $B = Conn laplace_proto_b
foreach ($s in 'a', 'b') { $db = "laplace_proto_$s"
    & $psql -X 'host=127.0.0.1 port=5432 user=laplace dbname=postgres' -qc "DROP DATABASE IF EXISTS $db WITH (FORCE)"
    "=== deploy $db"; Laplace deploy -d (Conn $db) | Select-String "highway's contents|^database"
    $env:LAPLACE_RECIPES = "$o/recipes-$s"; "=== ingest $db"; Laplace ingest -d (Conn $db) universal-dependencies | Select-String universal; Log }

"`n=== who said [forces, UPOS, NOUN], and how many times"
"--- A, the attestation table"; Laplace held -d $A --table forces UPOS upos:NOUN | Select-String 'witness|standing|games'
"--- B, a walk up the container index"; Laplace held -d $B --files forces UPOS upos:NOUN | Select-String 'trunk|witness|standing|under|walked'

"`n=== every word EWT tags NOUN, as one set read"
Laplace held -d $A --table --tsv "$o/nouns-a.tsv" '?' UPOS upos:NOUN | Select-String 'strands'
Laplace held -d $B --tsv "$o/nouns-b.tsv" '?' UPOS upos:NOUN | Select-String 'strands|walked|tokens'
$na = Get-Content "$Out\nouns-a.tsv" | ForEach-Object { ($_ -split "`t")[0, 2, 3, 4, 5, 6, 7, 8] -join "`t" } | Sort-Object
$nb = Get-Content "$Out\nouns-b.tsv" | ForEach-Object { ($_ -split "`t")[0, 2, 3, 4, 5, 6, 7, 8] -join "`t" } | Sort-Object
"  claim, games, score, position and standing, A against B: {0}" -f ($(if (-not (Compare-Object $na $nb -SyncWindow 0)) { "identical, $($na.Count) strands" } else { 'DIFFERENT' }))

"`n=== every attestation row of A, against B's trunk walked down (games off the tree, standings replayed)"
Laplace replay -d $B --tsv "$o/replay-b.tsv" | Select-String 'matchups|replayed'
Sql laplace_proto_a "\copy (SELECT claim, games FROM attestation ORDER BY 1) TO '$o/games-a.tsv'" | Out-Null
$ga = Get-Content "$Out\games-a.tsv" | Sort-Object; $gb = Get-Content "$Out\replay-b.tsv" | ForEach-Object { $f = $_ -split "`t"; "$($f[0])`t$($f[2])" } | Sort-Object
"  (claim, games), A's table against B's containment: {0}" -f ($(if (-not (Compare-Object $ga $gb -SyncWindow 0)) { "identical, $($ga.Count) rows" } else { 'DIFFERENT' }))
"  A's score and position: " + (Sql laplace_proto_a "SELECT 'score ' || string_agg(DISTINCT score::text, ',') || ', positions given ' || count(position) FROM attestation")

"`n=== rows and bytes (each database reindexed first, so the indexes are compact)"
foreach ($db in 'laplace_proto_a', 'laplace_proto_b') { "--- $db"; & $psql -X (Conn $db) -At -F "`t" -c "SET maintenance_work_mem = '2GB'" -c 'REINDEX TABLE physicality' -c 'REINDEX TABLE entity' -c 'REINDEX TABLE attestation' -c 'REINDEX TABLE consensus' -f "$here\sizes.sql" | Where-Object { $_ -notmatch '^(SET|REINDEX)$' } }

"`n=== forget UD by its trunk in B, then ingest it again: every standing as before"
Sql laplace_proto_b "\copy (SELECT claim, rating, deviation, volatility, matches FROM consensus ORDER BY 1) TO '$o/standings-b1.tsv'" | Out-Null
$trunk = Sql laplace_proto_b 'SELECT id FROM witness'
Laplace forget -d $B --trunk $trunk | Select-String 'entities nothing held|== total'
"  after the forget: " + (Sql laplace_proto_b "SELECT (SELECT count(*) FROM entity) || ' entities, ' || (SELECT count(*) FROM consensus) || ' standings, ' || (SELECT count(*) FROM witness) || ' witnesses'")
$env:LAPLACE_RECIPES = "$o/recipes-b"; Laplace ingest -d $B universal-dependencies | Select-String universal; Log
Sql laplace_proto_b "\copy (SELECT claim, rating, deviation, volatility, matches FROM consensus ORDER BY 1) TO '$o/standings-b2.tsv'" | Out-Null
"  standings before and after: {0}" -f ($(if ((Get-FileHash "$Out\standings-b1.tsv").Hash -eq (Get-FileHash "$Out\standings-b2.tsv").Hash) { 'identical, bit for bit' } else { 'DIFFERENT' }))
Laplace replay -d $B | Select-String 'replayed'
