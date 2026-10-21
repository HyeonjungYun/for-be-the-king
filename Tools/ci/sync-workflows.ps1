# sync-workflows.ps1 - GitHub Actions workflow files (2026-10-21). ASCII only (PowerShell 5.1 reads BOM-less files as cp949)
#
# The source of every workflow lives in Tools/ci/workflows/*.yml. GitHub only runs files under .github/workflows/,
# so this script copies them there byte for byte. Edit the Tools copy, never the .github copy.
#
#   .\Tools\ci\sync-workflows.ps1          copy Tools/ci/workflows/*.yml -> .github/workflows/ (and remove .github files with no source)
#   .\Tools\ci\sync-workflows.ps1 -Check   exit 1 if the two folders differ (used by Tools/verify/verify-server.ps1 and CI)
param([switch] $Check)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$src = Join-Path $PSScriptRoot 'workflows'
$dst = Join-Path $repo '.github\workflows'

$srcFiles = @(Get-ChildItem $src -Filter *.yml -File)
if ($srcFiles.Count -eq 0) { throw "no workflow sources in $src" }

$diff = @()
foreach ($f in $srcFiles) {
    $target = Join-Path $dst $f.Name
    $same = (Test-Path $target) -and ((Get-FileHash $f.FullName).Hash -eq (Get-FileHash $target).Hash)
    if (-not $same) { $diff += $f.Name }
}
$orphans = @()
if (Test-Path $dst) {
    foreach ($g in Get-ChildItem $dst -Filter *.yml -File) {
        if (-not (Test-Path (Join-Path $src $g.Name))) { $orphans += $g.Name }
    }
}

if ($Check) {
    if ($diff.Count -eq 0 -and $orphans.Count -eq 0) { Write-Host "[OK] .github/workflows matches Tools/ci/workflows ($($srcFiles.Count) files)"; exit 0 }
    foreach ($n in $diff) { Write-Host "[DIFF] $n - run Tools\ci\sync-workflows.ps1" }
    foreach ($n in $orphans) { Write-Host "[ORPHAN] .github/workflows/$n has no source in Tools/ci/workflows" }
    exit 1
}

New-Item -ItemType Directory -Force $dst | Out-Null
foreach ($n in $diff) { Copy-Item (Join-Path $src $n) (Join-Path $dst $n) -Force; Write-Host "[COPY] $n" }
foreach ($n in $orphans) { Remove-Item (Join-Path $dst $n); Write-Host "[REMOVE] $n (no source)" }
Write-Host "[OK] $($srcFiles.Count) workflow(s) in sync"
