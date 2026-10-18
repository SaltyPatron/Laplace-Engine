#Requires -Version 7
<#
  Single-head ablation of the forward pass (the recipe schema's validation: one operator alone, then all of them).
  For each prompt: one turn with every head (the firmware as it is), then one turn per head with `only predicate HEAD`.
  Records, per run: the heads that responded, the senses the forks chose, the answer, and the time. Read-only (--read).
    pwsh -File tools/ablate.ps1 [-Heads nmod,amod,...] [-Prompts "...","..."] [-Exe path\to\laplace.exe]
  The runs go to $LAPLACE_WORK\logs\ablate-<time>.
#>
param([string]$Exe = '', [string[]]$Heads = @(), [string[]]$Prompts = @('the bank of the river', 'I put my money in the bank', 'a dog barked at the cat', 'what is a dog'))
$ErrorActionPreference = 'Stop'
. D:\Libraries\load-env.ps1 *> $null; . D:\Repositories\Laplace\Laplace-Operations\laplace.env.ps1 *> $null
$exe = if ($Exe) { $Exe } else { Join-Path $env:LAPLACE_BUILD 'Laplace-Engine\icx-release\laplace.exe' }
$base = Get-Content (Join-Path $PSScriptRoot '..\firmware\program.firmware') -Raw
$out = Join-Path $env:LAPLACE_WORK ("logs\ablate-" + (Get-Date).ToString('yyyyMMddTHHmmss')); New-Item -ItemType Directory -Force $out | Out-Null
$env:LAPLACE_TIMES = '1'

function Run([string]$label, [string]$fwText, [string]$prompt) {
    $fw = Join-Path $out ("$label.firmware"); Set-Content -Path $fw -Value $fwText -NoNewline
    $t = Get-Date; $lines = & $exe turn --read --firmware $fw $prompt 2>&1 | ForEach-Object { "$_" }; $ms = ((Get-Date) - $t).TotalMilliseconds
    $lines | Set-Content (Join-Path $out ("$label--" + ($prompt -replace '\W', '_') + '.txt'))
    $heads = ($lines | Where-Object { $_ -match '^\s+heads: (\d+) relations' } | Select-Object -First 1) -replace '.*heads: (\d+) relations.*', '$1'
    $picks = ($lines | Where-Object { $_ -match '^fork: \* ' } | ForEach-Object { ($_ -replace '^fork: \* ', '').Substring(0, [Math]::Min(48, ($_ -replace '^fork: \* ', '').Length)).Trim() }) -join ' | '
    $answer = ($lines | Where-Object { $_ -match '^REALIZE' } | Select-Object -First 1) -replace '^REALIZE\s+', ''
    [pscustomobject]@{ run = $label; prompt = $prompt; heads = $heads; seconds = [math]::Round($ms / 1000, 1); senses = $picks; answer = $answer }
}

$rows = foreach ($p in $Prompts) {
    Run 'all' $base $p
    $hs = $Heads
    if (-not $hs.Count) {                     # the heads this prompt's own trace names, strongest first
        $trace = Get-Content (Join-Path $out ('all--' + ($p -replace '\W', '_') + '.txt'))
        $line = $trace | Where-Object { $_ -match '^\s+heads: \d+ relations respond; the strongest:' } | Select-Object -First 1
        $hs = [regex]::Matches($line, ' ([^ (]+)\(\d+ strands') | ForEach-Object { $_.Groups[1].Value } | Select-Object -First 6
    }
    foreach ($h in $hs) { Run ("only-" + ($h -replace '\W', '_')) ($base + "`n  only predicate $h`n") $p }
}
$rows | Export-Csv (Join-Path $out 'ablation.csv') -NoTypeInformation
$rows | Format-Table -AutoSize -Wrap | Out-String -Width 240
"results in $out"
