# DB 초기 셋업 — schema 먼저, seed 나중. 순서를 틀리면 시딩이 절반만 들어간다
param([Parameter(Mandatory=$true)][string]$Password)

$mysql = "C:\mysql-8.0.46-winx64\bin\mysql.exe"
$tools = $PSScriptRoot

foreach ($file in @("schema.sql", "seed_bots.sql")) {
    Write-Host "[DB] running $file"

    & cmd /c "`"$mysql`" -u root -p$Password < `"$tools\$file`"" 2>&1 |
        Where-Object { $_ -notmatch "Warning.*command line interface" }

    if ($LASTEXITCODE -ne 0) {
        Write-Host "[DB] FAILED at $file" -ForegroundColor Red
        exit 1
    }
}

Write-Host "[DB] verifying"
$q = "SELECT (SELECT COUNT(*) FROM accounts WHERE username LIKE 'bot%') AS accounts, (SELECT COUNT(*) FROM characters WHERE name LIKE 'bot%') AS chars, (SELECT COUNT(*) FROM login_sessions) AS tokens;"
& $mysql -u root "-p$Password" -D forbetheking -e $q 2>&1 |
    Where-Object { $_ -notmatch "Warning.*command line interface" }
