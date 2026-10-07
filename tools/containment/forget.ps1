#Requires -Version 7
<#
  Forget and replay, in B (proof.ps1 seeds it): a source forgotten by its name (its trunk, what it alone held, and the
  standings it touched played again from what still holds them); replay then says whether every standing left follows
  from containment alone; the source ingested again; every standing compared bit for bit with before the forget.
#>
param([string]$Out = 'D:\Temp\containment-proof\run', [string]$Source = 'universal-dependencies', [string]$Called = 'Universal Dependencies', [string]$Middle = 'Collaborative Interlingual Index')
. (Join-Path $PSScriptRoot 'lib.ps1')
$o = $Out -replace '\\', '/'; $env:LAPLACE_RECIPES = "$o/recipes"; $env:LAPLACE_WORK = "$o/work-b"; $db = 'laplace_proto_b'
function Stamp([string]$what) { "[{0:HH:mm:ss}] {1}" -f (Get-Date), $what }
function Counts { Sql $db "SELECT (SELECT count(*) FROM entity) || ' entities, ' || (SELECT count(*) FROM consensus) || ' standings, ' || (SELECT count(*) FROM witness) || ' witnesses'" }
function Dump([string]$f) { Sql $db "\copy (SELECT claim, rating, deviation, volatility, matches FROM consensus ORDER BY 1) TO '$o/$f'" | Out-Null; (Get-FileHash "$Out\$f").Hash }
Stamp "before: $(Counts)"; $h1 = Dump 'forget-before.tsv'
Stamp "forget $Called"; Laplace $BuildB $db @('forget', $Called) | Select-String 'witness|claims their records|entities nothing held|standings they touched|== total'
Stamp "after: $(Counts)"
Stamp 'replay: every standing left, from containment alone'; Laplace $BuildB $db @('replay') | Select-Object -Last 3
Stamp "ingest $Source again"; $env:LAPLACE_INGEST_ONE = '1'; Laplace $BuildB $db @('ingest', $Source) | ForEach-Object { ($_ -split "`r")[-1] } | Select-String 'the series, played|its ID|== total'
Remove-Item Env:\LAPLACE_INGEST_ONE
Stamp "again: $(Counts)"; $h2 = Dump 'forget-after.tsv'
"  standings before the forget and after the ingest again: {0}" -f $(if ($h1 -eq $h2) { 'identical, bit for bit' } else { 'DIFFERENT' })
if ($h1 -ne $h2) { $d = Compare-Object (Get-Content "$Out\forget-before.tsv") (Get-Content "$Out\forget-after.tsv"); "  $(@($d).Count) lines differ" }
Stamp 'replay'; Laplace $BuildB $db @('replay') | Select-Object -Last 3
# A source in the middle of the order: forgotten, and every standing left still follows from containment
Stamp "forget $Middle (in the middle of the order)"; Laplace $BuildB $db @('forget', $Middle) | Select-String 'claims their records|entities nothing held|standings they touched|== total'
Stamp "after: $(Counts)"; Laplace $BuildB $db @('replay') | Select-Object -Last 3
