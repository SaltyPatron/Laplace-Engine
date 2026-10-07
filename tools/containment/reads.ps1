#Requires -Version 7
<#
  The forward pass's reads of provenance, A against B (proof.ps1 seeds both): every claim a hop reads, with the place
  its witness gave it (positions_of: A the attestation table, B the records' vertices), the claims a firmware refuses
  by their witness (refused), and the fact a firmware returns by its witness's trust (forward.c, take fact); then the
  forward pass itself (pull) on the same prompts. What differs is listed, line for line.
#>
param([string]$Out = 'D:\Temp\containment-proof\run')
. (Join-Path $PSScriptRoot 'lib.ps1')
$o = $Out -replace '\\', '/'; $env:LAPLACE_RECIPES = "$o/recipes"
$R = "$Out\reads"; New-Item -ItemType Directory -Force $R | Out-Null
$fw = Get-Content (Join-Path $env:LAPLACE_SRC 'Laplace-Engine\firmware\program.firmware') -Raw
# A names a witness as its recipe does; B names a witness as its source's record: the same witnesses, by each side's name
Set-Content "$R\refuse-a.firmware" ($fw -replace "for pull`r?`n", "for pull`n  refuse witness UD_English-EWT UD_English-GUM`n" -replace "for hop`r?`n", "for hop`n  refuse witness UD_English-EWT UD_English-GUM`n")
Set-Content "$R\refuse-b.firmware" ($fw -replace "for pull`r?`n", "for pull`n  refuse witness `"Universal Dependencies`"`n" -replace "for hop`r?`n", "for hop`n  refuse witness `"Universal Dependencies`"`n")
Set-Content "$R\fact.firmware" ($fw -replace "for pull`r?`n", "for pull`n  fact 0.8`n  take fact`n")
function Strip($lines) { $lines | Where-Object { $_ -notmatch '\(\s*[0-9.,]+ ms|[0-9.,]+ s\)|^laplace |pool|snapshot' } | ForEach-Object { $_ -replace '\s+\(?[0-9.,]+ ms\)?', '' } }
function Both([string]$name, [string[]]$cmdA, [string[]]$cmdB) {
    $a = Strip (Laplace $BuildA 'laplace_proto_a' $cmdA); $b = Strip (Laplace $BuildB 'laplace_proto_b' $cmdB)
    Set-Content "$R\$name-a.txt" $a; Set-Content "$R\$name-b.txt" $b
    $d = Compare-Object $a $b -SyncWindow 0
    "{0,-34} A {1,6} lines   B {2,6} lines   {3}" -f $name, @($a).Count, @($b).Count, $(if (-not $d) { 'identical' } else { "$(@($d).Count) lines differ" })
}
# the claims a hop reads, with the place given: only the claim and its place, every claim (-n large)
function Placed([string]$name, [string[]]$q, [string]$fwA = '', [string]$fwB = '') {
    $ca = @('hop', '-n', '100000') + $(if ($fwA) { @('--firmware', $fwA) } else { @() }) + $q
    $cb = @('hop', '-n', '100000') + $(if ($fwB) { @('--firmware', $fwB) } else { @() }) + $q
    $pick = { param($lines) $lines | Where-Object { $_ -match '^\s+-?[0-9.]+\s+[0-9.]+\s+[0-9.]+\s+[0-9]+\s' } | ForEach-Object { $f = ($_.Trim() -split '\s+', 5); if ($f.Count -eq 5 -and $f[4].StartsWith('[')) { "{0}`t{1}" -f $f[4], '' } elseif ($f.Count -ge 5) { $g = ($_.Trim() -split '\s+', 6); "{0}`t{1}" -f $g[5], $g[4] } } | Sort-Object }
    $a = & $pick (Laplace $BuildA 'laplace_proto_a' $ca); $b = & $pick (Laplace $BuildB 'laplace_proto_b' $cb)
    Set-Content "$R\$name-a.txt" $a; Set-Content "$R\$name-b.txt" $b
    $a = @($a); $b = @($b); $d = if ($a.Count -and $b.Count) { Compare-Object $a $b } elseif ($a.Count -or $b.Count) { @($a) + @($b) } else { $null }
    "{0,-34} A {1,6} claims  B {2,6} claims   {3}" -f $name, @($a).Count, @($b).Count, $(if (-not $d) { 'the same claims, each at the same place' } else { "$(@($d).Count) lines differ" })
}
"== the claims a hop reads, and the place each was given (positions_of)"
Placed 'hop-dog' @('dog')
Placed 'hop-forces' @('forces')
Placed 'hop-eng' @('eng')
Placed 'hop-A' @('A')
Placed 'hop-dog-lemma' @('dog', '?', '?')
Placed 'hop-case' @('I hate women. ')
Placed 'hop-personx' @('PersonX decides to see a therapist')
"== refused by the witness (refused): A names UD's treebanks, B the source"
Placed 'refuse-forces' @('forces') "$R\refuse-a.firmware" "$R\refuse-b.firmware"
Placed 'refuse-dog' @('dog') "$R\refuse-a.firmware" "$R\refuse-b.firmware"
"== the forward pass (pull), whole output"
Both 'pull-dog' @('pull', 'dog') @('pull', 'dog')
Both 'pull-forces' @('pull', 'the forces') @('pull', 'the forces')
Both 'pull-fact-dog' @('pull', '--firmware', "$R\fact.firmware", 'dog') @('pull', '--firmware', "$R\fact.firmware", 'dog')
Both 'pull-fact-eng' @('pull', '--firmware', "$R\fact.firmware", 'eng') @('pull', '--firmware', "$R\fact.firmware", 'eng')
