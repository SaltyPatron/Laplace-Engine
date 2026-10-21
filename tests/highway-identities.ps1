param([Parameter(Mandatory)][string]$Engine,[Parameter(Mandatory)][string]$Scratch)
$ErrorActionPreference='Stop'
$sourceRoot=Split-Path $PSScriptRoot -Parent
$recipes=Join-Path $Scratch 'recipes'
New-Item -ItemType Directory -Path $recipes -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $sourceRoot 'recipes/formats') -Destination (Join-Path $recipes 'formats') -Recurse -Force
Copy-Item -LiteralPath (Join-Path $sourceRoot 'recipes/file.recipe') -Destination (Join-Path $recipes 'file.recipe') -Force
$expected=@{cili='i46360';propbank='abandon.01';verbatlas='va:0001f'}
$files=@{cili='ili.recipe';propbank='frames.recipe';verbatlas='frame-info.recipe'}
$priorSingle=$env:LAPLACE_INGEST_ONE
$env:LAPLACE_INGEST_ONE='1'
try {
foreach($name in @('cili','propbank','verbatlas')) {
    $dir=Join-Path $recipes $name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $sourceRoot ('recipes/'+$name+'/'+$files[$name])) -Destination $dir -Force
    $data=(Join-Path $PSScriptRoot ('fixtures/highway-identities/'+$name)).Replace('\','/')
    [IO.File]::WriteAllLines((Join-Path $dir 'source'),@(('name '+$name),('witness Highway identity '+$name),'class AcademicCurated',('root '+$data)))
    $output=@(& $Engine ingest --no-load --claims-tsv -j 1 -r $recipes $name 2>&1)
    if($LASTEXITCODE) { throw ($output -join [Environment]::NewLine) }
    $text=($output | ForEach-Object {$_.ToString()}) -join [Environment]::NewLine
    [IO.File]::WriteAllText((Join-Path $Scratch ($name+'.log')),$text)
    if($text -notmatch ('(?m)^claim\t[^\t]+\t[^\t]+\t'+[regex]::Escape($expected[$name])+'\t')) { throw "$name claims lost their identifier subject" }
    if($name -eq 'cili' -and ($text -notmatch 'is defined as' -or $text -match '\tskos:definition\t')) { throw 'CILI did not attest a semantic gloss relation' }
    if($name -eq 'verbatlas' -and ($text -notmatch '\tname of a VerbAtlas frame\t' -or $text -notmatch '\tframe definition\t' -or $text -match 'type:#-1')) { throw 'Sizing the first batch lost composed relation text' }
    Write-Host "PASS $name canonical identifier subject $($expected[$name])"
}
} finally { $env:LAPLACE_INGEST_ONE=$priorSingle }
