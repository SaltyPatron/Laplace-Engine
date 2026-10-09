#Requires -Version 7
<#
  Shared by the containment proof (Engine#22, #39 step 2): the developer shell, scratch databases only, and the engine
  run with the database it is to use named twice: in LAPLACE_CONNINFO and as -d. A database whose name is not
  laplace_proto_* is refused before anything runs; the compiled-in default of both builds is laplace_proto_none.
#>
$ErrorActionPreference = 'Continue'                                      # the engine writes its progress on stderr
if (-not $env:LAPLACE_SRC) { $env:LAPLACE_SRC = 'D:\Repositories\Laplace' }      # a task started on its own has no environment of ours
. D:\Libraries\load-env.ps1 *> $null                                      # the developer shell: oneAPI's run-time DLLs
. (Join-Path $env:LAPLACE_SRC 'Laplace-Operations\laplace.env.ps1') *> $null
$env:LAPLACE_DATA = 'D:/Data/Ingest'                                        # the corpora, whatever the environment held before
$env:LAPLACE_CONNINFO = 'host=127.0.0.1 port=5432 user=laplace dbname=laplace_proto_none'
$env:PATH = "$env:LAPLACE_PG_DIR\bin;$env:LAPLACE_ICU_DIR\bin64;$env:LAPLACE_ZLIB_DIR\bin;$env:PATH"
$script:Psql = Join-Path $env:LAPLACE_PG_DIR 'bin\psql.exe'
$script:BuildA = 'D:\Libraries\build\Laplace\wt-engine-asbuilt'           # origin/main as built: provenance in the attestation table
$script:BuildB = 'D:\Libraries\build\Laplace\wt-engine-contain2'          # this branch: provenance by containment

function Conn([string]$db) {
    if ($db -notmatch '^laplace_proto_[a-z0-9_]+$') { throw "not a scratch database: $db" }
    "host=127.0.0.1 port=5432 user=laplace dbname=$db"
}
# laplace COMMAND -d CONN ARGS..., with LAPLACE_CONNINFO the same database
function Laplace([string]$build, [string]$db, [string[]]$cmd) {
    $c = Conn $db; $env:LAPLACE_CONNINFO = $c
    $argv = @($cmd[0], '-d', $c); if ($cmd.Count -gt 1) { $argv += $cmd[1..($cmd.Count - 1)] }
    & (Join-Path $build 'laplace.exe') @argv 2>&1 | ForEach-Object { "$_" } | Where-Object { $_ -notmatch '^OMP: ' }
    $env:LAPLACE_CONNINFO = 'host=127.0.0.1 port=5432 user=laplace dbname=laplace_proto_none'
}
function Sql([string]$db, [string]$q) { & $script:Psql -X (Conn $db) -At -F "`t" -c $q 2>&1 | ForEach-Object { "$_" } }
function SqlFile([string]$db, [string]$f) { & $script:Psql -X (Conn $db) -At -F "`t" -f $f 2>&1 | ForEach-Object { "$_" } }
function Drop([string]$db) { $null = Conn $db; & $script:Psql -X 'host=127.0.0.1 port=5432 user=laplace dbname=postgres' -qc "DROP DATABASE IF EXISTS $db WITH (FORCE)" 2>&1 }
# A recipes tree of the build's checkout, with a source's own file replaced where the proof narrows it (overrides\SOURCE\source)
function Recipes([string]$repo, [string]$to, [string]$overrides) {
    if (Test-Path $to) { Remove-Item -Recurse -Force $to }
    Copy-Item -Recurse (Join-Path $repo 'recipes') $to
    if ($overrides -and (Test-Path $overrides)) { Get-ChildItem -Directory $overrides | ForEach-Object { Copy-Item -Force (Join-Path $_.FullName '*') (Join-Path $to $_.Name) } }
}
