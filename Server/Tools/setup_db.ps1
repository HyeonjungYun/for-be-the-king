# DB 셋업 - 새 PC 에서 이것 하나로 게임 서버 · 인증 서버가 도는 DB 를 만든다 (2026-10-18 전체 순서).
# 예전엔 schema.sql 과 seed_bots.sql 만 적용해서 인증 · 아이템 · 보상 · 저장 기록 표와 열 이전이 빠졌다.
#
#   .\setup_db.ps1                      # 비밀번호는 FBTK_DB_PASSWORD
#   .\setup_db.ps1 -Password pw -NoSeed # 표만 (봇 · dev 계정 없음)
#   .\setup_db.ps1 -Database fbtk_check # 다른 이름으로 - 검증용 (파일의 forbetheking 을 바꿔 흘린다)
#
# 순서: 표(schema*) -> 열 이전(migrate_*, 다시 돌려도 무해) -> 시드(seed_bots · seed_dev). 모든 파일은 다시 돌려도 된다.
# 옛 데이터 이전(migrate_item_type_ids · migrate_skill_roster · migrate_overload_nihil_helmet)은 새 DB 에 할 일이 없어 넣지 않았다 -
# 그 시절 행이 남은 DB 에만 손으로 돌린다 (각 파일 머리 주석).
param(
    [string] $Password = $env:FBTK_DB_PASSWORD,
    [string] $Database = 'forbetheking',
    [switch] $NoSeed,
    [switch] $WithTestItems,
    [string] $MySql = 'C:\mysql-8.0.46-winx64\bin\mysql.exe',
    [int] $Port = 3306      # 실행 묶음(tools/package-bundle.ps1)의 DB 는 3307 (2026-10-19)
)
$conn = @('-h', '127.0.0.1', '-P', "$Port", '-u', 'root', "-p$Password")

if ([string]::IsNullOrEmpty($Password)) {
    Write-Host "[DB] no password - pass -Password or set FBTK_DB_PASSWORD" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path $MySql)) {
    Write-Host "[DB] mysql.exe not found at $MySql (pass -MySql)" -ForegroundColor Red
    exit 1
}

$tools = $PSScriptRoot
$files = @(
    'schema.sql',                  # accounts · characters · login_sessions
    'schema_auth.sql',             # accounts.password_iterations (ADR-0011)
    'schema_item.sql',             # item_instances · bags (lone_path · stack_count 포함)
    'schema_reward.sql',           # characters.gold · reward_grants (멱등 지급)
    'schema_commit_journal.sql',   # applied_commits (ADR-0010 6)
    'migrate_lone_path.sql',       # 옛 DB 에 lone_path 열 (있으면 건너뜀)
    'migrate_economy.sql',         # 옛 DB 에 stack_count 열 (있으면 건너뜀)
    'migrate_hash_tokens.sql'      # 토큰 해시 저장 - schema_migrations 표시로 한 번만
)
if (-not $NoSeed) {
    $files += 'seed_bots.sql'      # bot_1 .. bot_120 (부하 하네스)
    $files += 'seed_dev.sql'       # dev_1 (tools/dev-launch.ps1)
}
if ($WithTestItems) {
    $files += 'seed_test_items.sql'
}

# mysql 은 표준 입력을 UTF-8 로 받는다 - PowerShell 5.1 의 기본($OutputEncoding = ASCII)이면 한글이 ? 가 된다
$OutputEncoding = New-Object System.Text.UTF8Encoding($false)

foreach ($file in $files) {
    $path = Join-Path $tools $file
    Write-Host "[DB] $file -> $Database"
    $sql = [System.IO.File]::ReadAllText($path)
    if ($Database -ne 'forbetheking') {
        $sql = $sql.Replace('CREATE DATABASE IF NOT EXISTS forbetheking', "CREATE DATABASE IF NOT EXISTS $Database")
        $sql = $sql.Replace('USE forbetheking;', "USE $Database;")
        $sql = $sql.Replace("TABLE_SCHEMA = 'forbetheking'", "TABLE_SCHEMA = '$Database'")
    }
    if ($sql -notmatch '(?im)^\s*USE\s') {
        $sql = "USE $Database;`n" + $sql       # 시드 몇 개는 USE 가 없다 - 대상 DB 를 박아 준다
    }

    $out = $sql | & $MySql @conn --default-character-set=utf8mb4 2>&1 |
        Where-Object { $_ -notmatch 'Warning.*command line interface' }
    if ($LASTEXITCODE -ne 0) {
        $out | ForEach-Object { Write-Host "  $_" }
        Write-Host "[DB] FAILED at $file" -ForegroundColor Red
        exit 1
    }
}

Write-Host "[DB] verifying $Database"
$q = "SELECT (SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA = '$Database') AS tables, " +
     "(SELECT COUNT(*) FROM accounts WHERE username LIKE 'bot%') AS bot_accounts, " +
     "(SELECT COUNT(*) FROM accounts WHERE username = 'dev_1') AS dev, " +
     "(SELECT COUNT(*) FROM login_sessions) AS tokens;"
& $MySql @conn -D $Database -e $q 2>&1 | Where-Object { $_ -notmatch 'Warning.*command line interface' }
exit 0
