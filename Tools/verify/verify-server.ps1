<#
.SYNOPSIS
    서버 검증 툴 (2026-10-21) - 서버를 열기 전에 문제가 있는지 자동으로 확인한다. 운영 DB 와 운영 포트는 건드리지 않는다.

.DESCRIPTION
    네 묶음을 차례로 돌리고, 마지막에 "서버를 열어도 되는가" 를 판정한다. 결과는 보고서 폴더(report.md · results.json · 로그)에 남는다.

      static  정적 검사     워크플로 사본 · 줄끝 · 인코딩 · 패킷 생성물(.proto) · GameServer --validate (부팅 데이터 검사만)
      build   빌드 + 테스트  ServerCore · GameServer · DummyClient · 테스트 · 인증 서버 빌드 → GameServerTests · ServerCoreSTests → 커버리지 70 %
      db      DB 스키마     빈 임시 DB 에 setup_db · 지금 DB 의 스키마 사본에 이전 적용 → 둘의 표 · 열 · 인덱스 비교 · 코드가 쓰는 표 · 열이 다 있는지
      smoke   격리 부팅     임시 DB(fbtk_verify_smoke) · 다른 포트로 서버를 따로 띄워 봇 N 명을 M 초 → 크래시 · 오류 줄 · 안티치트 · 이동 거부 ·
                            Room 플러시 P95 / P99 / max · 메모리 → Ctrl+C 정상 종료. 기준은 thresholds.json

    판정: FAIL 이 하나라도 있으면 "열지 말 것" (종료 코드 1). WARN 은 열어도 되지만 볼 것.

.EXAMPLE
    .\Tools\verify\verify-server.ps1                              # 전부 (DB 비밀번호는 FBTK_DB_PASSWORD)
    .\Tools\verify\verify-server.ps1 -Stage static                # 정적 검사만 (몇 초)
    .\Tools\verify\verify-server.ps1 -Stage static,build -NoCoverage
    .\Tools\verify\verify-server.ps1 -Stage smoke -Bots 60 -Seconds 300 -Port 7811
#>
param(
    [ValidateSet('static', 'build', 'db', 'smoke')] [string[]] $Stage = @('static', 'build', 'db', 'smoke'),
    [int] $Bots = 30,
    [int] $Seconds = 120,
    [int] $Port = 7801,                 # 스모크 서버의 게임 포트 - 운영 7777 과 겹치지 않게
    [int] $AdminPort = 7802,            # 스모크 서버의 관리 포트 (FBTK_ADMIN_PORT)
    [string] $DbPassword = $env:FBTK_DB_PASSWORD,
    [string] $MySql = 'C:\mysql-8.0.46-winx64\bin\mysql.exe',
    [switch] $NoCoverage,
    [switch] $KeepDb,                   # 끝나도 fbtk_verify_* 스키마를 지우지 않는다 (조사용)
    [string] $Out
)
$ErrorActionPreference = 'Continue'     # 네이티브 도구의 stderr 를 예외로 바꾸지 않는다 (PowerShell 5.1)
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$serverDir = Join-Path $repo 'Server'
$rel = Join-Path $serverDir 'Binaries\Release'
$dbg = Join-Path $serverDir 'Binaries\Debug'
if (-not $Out) { $Out = Join-Path $PSScriptRoot ('reports\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force $Out | Out-Null
$th = Get-Content (Join-Path $PSScriptRoot 'thresholds.json') -Raw | ConvertFrom-Json
$env:PYTHONIOENCODING = 'utf-8'
$results = New-Object System.Collections.ArrayList
$started = Get-Date

function Add-Result([string] $group, [string] $name, [string] $status, [string] $detail, [double] $sec) {
    [void] $results.Add([pscustomobject]@{ Group = $group; Check = $name; Status = $status; Detail = $detail; Seconds = [math]::Round($sec, 1) })
    $color = @{ PASS = 'Green'; WARN = 'Yellow'; FAIL = 'Red'; SKIP = 'DarkGray' }[$status]
    Write-Host ("  [{0}] {1} - {2}" -f $status, $name, $detail) -ForegroundColor $color
}

# 한 검사 = 스크립트 블록 하나. @{ Status; Detail } 을 돌려준다 - 예외는 FAIL
function Step([string] $group, [string] $name, [scriptblock] $body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { $r = & $body } catch { $r = @{ Status = 'FAIL'; Detail = "예외: $($_.Exception.Message)" } }
    if ($null -eq $r -or -not $r.Status) { $r = @{ Status = 'FAIL'; Detail = '검사가 결과를 돌려주지 않았다' } }
    Add-Result $group $name $r.Status $r.Detail $sw.Elapsed.TotalSeconds
}

function Has([string] $s) { return $Stage -contains $s }

function Invoke-Logged([string] $exe, [string[]] $argv, [string] $log) {
    & $exe @argv *> $log
    return $LASTEXITCODE
}

function Tail([string] $log, [int] $n = 6) {
    if (-not (Test-Path $log)) { return '' }
    return ((Get-Content $log -Tail $n) -join ' | ')
}

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) { return (& $vswhere -latest -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1) }
    return $null
}

function Sql([string] $q, [string] $db = '') {
    $a = @('-h', '127.0.0.1', '-u', 'root', "-p$DbPassword", '-N', '-B', '-e', $q)
    if ($db) { $a += @('-D', $db) }
    $o = & $MySql @a 2>&1 | Where-Object { $_ -notmatch 'Warning.*command line' }
    return $o
}

function Schema-Snapshot([string] $db) {
    $cols = Sql "SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, IFNULL(COLUMN_DEFAULT, '<null>'), EXTRA FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '$db' ORDER BY TABLE_NAME, COLUMN_NAME"
    $idx = Sql "SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, GROUP_CONCAT(COLUMN_NAME ORDER BY SEQ_IN_INDEX) FROM information_schema.STATISTICS WHERE TABLE_SCHEMA = '$db' GROUP BY TABLE_NAME, INDEX_NAME, NON_UNIQUE ORDER BY TABLE_NAME, INDEX_NAME"
    return @{ Columns = @($cols | ForEach-Object { "col $_" }); Indexes = @($idx | ForEach-Object { "idx $_" }) }
}

function Setup-Db([string] $db, [switch] $NoSeed, [string] $log) {
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $repo 'Tools\db\setup_db.ps1'), '-Password', $DbPassword, '-Database', $db, '-MySql', $MySql)
    if ($NoSeed) { $a += '-NoSeed' }
    & powershell.exe @a *> $log
    return $LASTEXITCODE
}

Write-Host "=== 서버 검증 ($($Stage -join ', ')) - 보고서 $Out ===" -ForegroundColor Cyan

# ───────────────────────────── 1. 정적 검사 (빌드 없이) ─────────────────────────────
if (Has 'static') {
    Write-Host "[정적 검사]" -ForegroundColor Cyan
    Step '정적' '워크플로 사본' {
        $o = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'Tools\ci\sync-workflows.ps1') -Check 2>&1
        if ($LASTEXITCODE -eq 0) { @{ Status = 'PASS'; Detail = 'Tools/ci/workflows = .github/workflows' } }
        else { @{ Status = 'FAIL'; Detail = (($o | Out-String).Trim() -replace '\s+', ' ') + ' → Tools\ci\sync-workflows.ps1' } }
    }
    Step '정적' '줄끝 · 인코딩' {
        $json = Join-Path $Out 'text.json'
        $code = Invoke-Logged 'python' @('-I', (Join-Path $PSScriptRoot 'check_text.py'), $repo, $json) (Join-Path $Out 'text.log')
        $r = Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($code -eq 0) { @{ Status = $(if ($r.warnings.Count) { 'WARN' } else { 'PASS' }); Detail = "$($r.scanned) 파일 · 경고 $($r.warnings.Count)" + $(if ($r.warnings.Count) { " ($($r.warnings[0]))" } else { '' }) } }
        else { @{ Status = 'FAIL'; Detail = "$($r.errors.Count) 건 - $($r.errors[0])" } }
    }
    Step '정적' '패킷 생성물 (.proto)' {
        $json = Join-Path $Out 'proto.json'
        $code = Invoke-Logged 'python' @('-I', (Join-Path $PSScriptRoot 'check_proto.py'), $repo, $json) (Join-Path $Out 'proto.log')
        $r = Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json
        if ($code -eq 0) { @{ Status = 'PASS'; Detail = "생성물 $($r.compared) 개가 지금 .proto 와 같다" } }
        else { @{ Status = 'FAIL'; Detail = "$($r.errors.Count) 건 - $($r.errors[0])" } }
    }
}

# ───────────────────────────── 2. 빌드 + 단위 테스트 ─────────────────────────────
if (Has 'build') {
    Write-Host "[빌드 + 테스트]" -ForegroundColor Cyan
    $msbuild = Find-MSBuild
    $sln = Join-Path $serverDir 'Server.sln'
    $common = @('/p:Platform=x64', '/p:PreBuildEventUseInBuild=false', '/m', '/v:minimal', '/nologo')
    foreach ($b in @(
            @{ Name = 'ServerCore (Release · Debug)'; Runs = @(@('/t:ServerCore', '/p:Configuration=Release'), @('/t:ServerCore', '/p:Configuration=Debug')) },
            @{ Name = 'GameServer · DummyClient · GameServerTests (Release)'; Runs = @(, @('/t:GameServer', '/t:DummyClient', '/t:GameServerTests', '/p:Configuration=Release')) },
            @{ Name = 'ServerCoreSTests (Debug)'; Runs = @(, @('/t:ServerCoreSTests', '/p:Configuration=Debug')) })) {
        $spec = $b
        Step '빌드' $spec.Name {
            if (-not $msbuild) { return @{ Status = 'FAIL'; Detail = 'MSBuild 를 찾지 못했다 (Visual Studio 2022)' } }
            $warn = @{}; $i = 0
            foreach ($run in $spec.Runs) {
                $i++
                $log = Join-Path $Out ("build-{0}-{1}.log" -f ($spec.Name -replace '[^A-Za-z]', ''), $i)
                $code = Invoke-Logged $msbuild (@($sln) + $run + $common) $log
                Select-String -Path $log -Pattern 'warning (C|LNK)\d+' | ForEach-Object { $warn[($_.Line -replace '^\s+', '')] = 1 }
                if ($code -ne 0) {
                    $err = (Select-String -Path $log -Pattern ' error ' | Select-Object -First 1).Line
                    return @{ Status = 'FAIL'; Detail = "빌드 실패 - $err" }
                }
            }
            @{ Status = 'PASS'; Detail = "경고 $($warn.Count) 종" }
        }
    }
    Step '빌드' '인증 서버 (dotnet)' {
        $log = Join-Path $Out 'build-auth.log'
        $code = Invoke-Logged 'dotnet' @('build', (Join-Path $serverDir 'AuthServer'), '-c', 'Release', '--nologo', '-v', 'q') $log
        if ($code -eq 0) { @{ Status = 'PASS'; Detail = 'AuthServer Release' } } else { @{ Status = 'FAIL'; Detail = (Tail $log 3) } }
    }
    foreach ($t in @(@{ Name = 'GameServerTests'; Exe = (Join-Path $rel 'GameServerTests.exe') }, @{ Name = 'ServerCoreSTests'; Exe = (Join-Path $dbg 'ServerCoreSTests.exe') })) {
        $spec = $t
        Step '테스트' $spec.Name {
            if (-not (Test-Path $spec.Exe)) { return @{ Status = 'FAIL'; Detail = "$($spec.Exe) 가 없다" } }
            $log = Join-Path $Out "$($spec.Name).log"
            $code = Invoke-Logged $spec.Exe @("--gtest_output=xml:$(Join-Path $Out "$($spec.Name).xml")") $log
            $passed = (Select-String -Path $log -Pattern '\[  PASSED  \] (\d+) test' | Select-Object -Last 1).Matches.Groups[1].Value
            $failed = @(Select-String -Path $log -Pattern '^\[  FAILED  \] [A-Za-z]' | ForEach-Object { $_.Line -replace '^\[  FAILED  \] ', '' } | Select-Object -Unique)
            if ($code -eq 0) { @{ Status = 'PASS'; Detail = "$passed 개 통과" } }
            else { @{ Status = 'FAIL'; Detail = "실패 $($failed.Count) - $($failed | Select-Object -First 3)" } }
        }
    }
    Step '테스트' '커버리지 (ServerCore 70 %)' {
        $occ = 'C:\Program Files\OpenCppCoverage\OpenCppCoverage.exe'
        if ($NoCoverage) { return @{ Status = 'SKIP'; Detail = '-NoCoverage' } }
        if (-not (Test-Path $occ)) { return @{ Status = 'WARN'; Detail = 'OpenCppCoverage 가 없다 (choco install opencppcoverage)' } }
        $xml = Join-Path $Out 'coverage.xml'
        # 제외는 technical-preferences.md § Testing 과 같다 (CI 와도 같다)
        $a = @('--sources', (Join-Path $serverDir 'ServerCore'), '--excluded_sources', 'ServerCoreSTests', '--excluded_sources', 'SocketUtils',
            '--excluded_sources', 'DBConnection.cpp', '--excluded_sources', 'DBStatement.cpp', '--excluded_sources', 'CrashDump.cpp',
            '--export_type', "cobertura:$xml", '--quiet', '--', (Join-Path $dbg 'ServerCoreSTests.exe'))
        $code = Invoke-Logged $occ $a (Join-Path $Out 'coverage.log')
        if (-not (Test-Path $xml)) { return @{ Status = 'FAIL'; Detail = "커버리지 보고서가 없다 (exit $code)" } }
        $rate = [double] ([xml] (Get-Content $xml -Raw)).coverage.'line-rate'
        $pct = [math]::Round($rate * 100, 1)
        if ($rate -ge 0.70) { @{ Status = 'PASS'; Detail = "$pct %" } } else { @{ Status = 'FAIL'; Detail = "$pct % < 70 %" } }
    }
}

# 부팅 데이터 검사는 빌드 뒤에 (방금 만든 exe 로) - 정적 묶음에 속한다
if (Has 'static') {
    Step '정적' '부팅 데이터 (GameServer --validate)' {
        $exe = Join-Path $rel 'GameServer.exe'
        if (-not (Test-Path $exe)) { return @{ Status = 'FAIL'; Detail = 'GameServer.exe 가 없다 - build 단계를 먼저' } }
        $env:FBTK_DATA_DIR = Join-Path $serverDir 'Data'
        $log = Join-Path $Out 'validate.log'
        $code = Invoke-Logged $exe @('--validate') $log
        Remove-Item Env:\FBTK_DATA_DIR
        $okLine = (Select-String -Path $log -Pattern '\[VALIDATE\] OK' | Select-Object -First 1).Line
        $newest = Get-ChildItem (Join-Path $serverDir 'GameServer') -Include *.cpp, *.h -Recurse | Sort-Object LastWriteTime | Select-Object -Last 1
        $stale = (Get-Item $exe).LastWriteTime -lt $newest.LastWriteTime
        if ($code -eq 0 -and $okLine) {
            $detail = ($okLine -replace '^.*\[VALIDATE\] OK - ', '')
            if ($stale -and -not (Has 'build')) { @{ Status = 'WARN'; Detail = "통과했지만 exe 가 소스($($newest.Name))보다 오래됐다 - build 단계와 같이 돌릴 것. $detail" } }
            else { @{ Status = 'PASS'; Detail = $detail } }
        }
        else {
            $bad = (Select-String -Path $log -Pattern 'invalid|failed' | Select-Object -First 1).Line
            @{ Status = 'FAIL'; Detail = "exit $code - $bad" }
        }
    }
}

# ───────────────────────────── 3. DB 스키마 · 이전 ─────────────────────────────
$dbReady = $DbPassword -and (Test-Path $MySql)
$fresh = 'fbtk_verify_fresh'; $migr = 'fbtk_verify_migrate'; $smokeDb = 'fbtk_verify_smoke'
if (Has 'db') {
    Write-Host "[DB 스키마 · 이전]" -ForegroundColor Cyan
    if (-not $dbReady) {
        Add-Result 'DB' 'DB 접속' 'FAIL' 'FBTK_DB_PASSWORD 또는 mysql.exe 가 없다' 0
    }
    else {
        $dbDir = Join-Path $Out 'db'; New-Item -ItemType Directory -Force $dbDir | Out-Null
        Step 'DB' '빈 DB 에 setup_db' {
            [void] (Sql "DROP DATABASE IF EXISTS $fresh")
            $code = Setup-Db $fresh -log (Join-Path $dbDir 'setup-fresh.log')
            if ($code -eq 0) { @{ Status = 'PASS'; Detail = "$fresh - " + ((Get-Content (Join-Path $dbDir 'setup-fresh.log') | Select-Object -Last 1) -replace '\s+', ' ') } }
            else { @{ Status = 'FAIL'; Detail = (Tail (Join-Path $dbDir 'setup-fresh.log') 3) } }
        }
        Step 'DB' '지금 DB 에 이전 적용' {
            $dump = Join-Path $dbDir 'current-schema.sql'
            $dumpExe = Join-Path (Split-Path $MySql) 'mysqldump.exe'
            & cmd.exe /c "`"$dumpExe`" -h 127.0.0.1 -u root -p$DbPassword --no-data --skip-comments --routines=false forbetheking > `"$dump`" 2> `"$dbDir\dump.err`""
            if ($LASTEXITCODE -ne 0) { return @{ Status = 'FAIL'; Detail = 'mysqldump 실패 - ' + (Get-Content "$dbDir\dump.err" -Raw) } }
            [void] (Sql "DROP DATABASE IF EXISTS $migr; CREATE DATABASE $migr CHARACTER SET utf8mb4")
            & cmd.exe /c "`"$MySql`" -h 127.0.0.1 -u root -p$DbPassword -D $migr < `"$dump`" 2> `"$dbDir\load.err`""
            if ($LASTEXITCODE -ne 0) { return @{ Status = 'FAIL'; Detail = '지금 스키마 불러오기 실패 - ' + (Get-Content "$dbDir\load.err" -Raw) } }
            $script:beforeMigrate = Schema-Snapshot $migr
            $code = Setup-Db $migr -NoSeed -log (Join-Path $dbDir 'setup-migrate.log')
            if ($code -eq 0) { @{ Status = 'PASS'; Detail = "forbetheking 스키마 사본($migr)에 schema · migrate 전부 다시 적용 - 오류 없음" } }
            else { @{ Status = 'FAIL'; Detail = '이전 스크립트가 지금 DB 에서 실패 - ' + (Tail (Join-Path $dbDir 'setup-migrate.log') 3) } }
        }
        Step 'DB' '새 DB = 이전한 DB (표 · 열 · 인덱스)' {
            $a = Schema-Snapshot $fresh; $b = Schema-Snapshot $migr
            # 비교는 새 DB 에 있는 표만 - 운영 DB 에만 있는 표(하네스 백업 bc_*_bak 등)는 따로 알린다
            $tableOf = { param($line) (($line -split "`t")[0] -replace '^(col|idx) ', '') }
            $freshTables = @{}; foreach ($l in $a.Columns) { $freshTables[(& $tableOf $l)] = 1 }
            $bShared = @(($b.Columns + $b.Indexes) | Where-Object { $freshTables.ContainsKey((& $tableOf $_)) })
            $extra = @($b.Columns | ForEach-Object { & $tableOf $_ } | Where-Object { -not $freshTables.ContainsKey($_) } | Select-Object -Unique)
            $diff = @(Compare-Object ($a.Columns + $a.Indexes) $bShared)
            $onlyFresh = @($diff | Where-Object SideIndicator -eq '<=' | ForEach-Object InputObject)
            $onlyMigr = @($diff | Where-Object SideIndicator -eq '=>' | ForEach-Object InputObject)
            (@($onlyFresh | ForEach-Object { "새 DB 에만: $_" }) + @($onlyMigr | ForEach-Object { "이전한 DB 에만: $_" }) + @($extra | ForEach-Object { "운영 DB 에만 있는 표: $_" })) |
                Set-Content (Join-Path $dbDir 'schema-diff.txt') -Encoding UTF8
            $note = if ($extra.Count) { " · 운영 DB 에만 있는 표 $($extra.Count) 개 ($(($extra | Select-Object -First 3) -join ', ')) - 비교에서 뺐다" } else { '' }
            if ($onlyFresh.Count + $onlyMigr.Count -eq 0) { @{ Status = 'PASS'; Detail = "표 $($freshTables.Count) · 열 $($a.Columns.Count) · 인덱스 $($a.Indexes.Count) 일치$note" } }
            else { @{ Status = 'FAIL'; Detail = "차이 $($onlyFresh.Count + $onlyMigr.Count) 건 (db\schema-diff.txt) - 예: $(@($onlyFresh + $onlyMigr)[0])$note" } }
        }
        Step 'DB' '운영 DB 에 남은 이전' {
            $a = Schema-Snapshot $fresh
            $pending = @(Compare-Object ($a.Columns + $a.Indexes) ($script:beforeMigrate.Columns + $script:beforeMigrate.Indexes) | Where-Object SideIndicator -eq '<=')
            # 운영 DB 그대로(이전 전)에 없는 것 = 아직 안 돌린 이전 - 표가 통째로 없으면 그 표의 열이 전부 여기 들어온다
            if ($pending.Count -eq 0) { @{ Status = 'PASS'; Detail = 'forbetheking 은 최신 스키마' } }
            else { @{ Status = 'WARN'; Detail = "forbetheking 에 아직 없는 것 $($pending.Count) 건 - 열기 전에 Tools\db\setup_db.ps1 (다시 돌려도 무해). 예: $($pending[0].InputObject)" } }
        }
        Step 'DB' '코드가 쓰는 표 · 열' {
            $schemaJson = Join-Path $dbDir 'fresh-schema.json'
            $map = @{}
            Sql "SELECT TABLE_NAME, COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '$fresh'" | ForEach-Object {
                $p = $_ -split "`t"; if (-not $map.ContainsKey($p[0])) { $map[$p[0]] = New-Object System.Collections.ArrayList }; [void] $map[$p[0]].Add($p[1])
            }
            [IO.File]::WriteAllText($schemaJson, ($map | ConvertTo-Json -Depth 3), (New-Object Text.UTF8Encoding($false)))
            $json = Join-Path $dbDir 'sql-refs.json'
            $code = Invoke-Logged 'python' @('-I', (Join-Path $PSScriptRoot 'check_sql_refs.py'), $repo, $json, $schemaJson) (Join-Path $dbDir 'sql-refs.log')
            $r = Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json
            $tables = @($r.tables.PSObject.Properties).Count
            if ($code -eq 0) { @{ Status = 'PASS'; Detail = "표 $tables 개 · INSERT 열 전부 새 DB 에 있다" } }
            else { @{ Status = 'FAIL'; Detail = "$($r.errors.Count) 건 - $($r.errors[0])" } }
        }
    }
}

# ───────────────────────────── 4. 격리 부팅 스모크 ─────────────────────────────
if (Has 'smoke') {
    Write-Host "[격리 부팅 스모크 - 봇 $Bots 명 × $Seconds 초, 포트 $Port]" -ForegroundColor Cyan
    $sm = Join-Path $Out 'smoke'; New-Item -ItemType Directory -Force $sm | Out-Null
    $srvLog = Join-Path $sm 'server.log'
    $script:srv = $null; $script:botProc = $null; $script:smokeOk = $false
    Step '스모크' '임시 DB · 포트' {
        if (-not $dbReady) { return @{ Status = 'FAIL'; Detail = 'FBTK_DB_PASSWORD 또는 mysql.exe 가 없다' } }
        foreach ($p in @($Port, $AdminPort)) {
            if (Get-NetTCPConnection -LocalPort $p -State Listen -ErrorAction SilentlyContinue) { return @{ Status = 'FAIL'; Detail = "포트 $p 를 다른 프로그램이 쓰고 있다 (-Port / -AdminPort 로 바꿀 것)" } }
        }
        [void] (Sql "DROP DATABASE IF EXISTS $smokeDb")
        $code = Setup-Db $smokeDb -log (Join-Path $sm 'setup-smoke.log')
        if ($code -ne 0) { return @{ Status = 'FAIL'; Detail = (Tail (Join-Path $sm 'setup-smoke.log') 3) } }
        $script:smokeOk = $true
        @{ Status = 'PASS'; Detail = "$smokeDb (시드: 봇 120 · dev_1) · 게임 $Port · 관리 $AdminPort" }
    }
    if ($script:smokeOk) {
        Step '스모크' '서버 부팅' {
            $set = @{
                FBTK_DATA_DIR = (Join-Path $serverDir 'Data'); FBTK_DB_SCHEMA = $smokeDb; FBTK_DB_USER = 'fbtk_app'; FBTK_DB_PASSWORD = $DbPassword
                FBTK_PORT = "$Port"; FBTK_ADMIN_PORT = "$AdminPort"; FBTK_METRICS_DIR = (Join-Path $sm 'metrics'); FBTK_JOURNAL_DIR = (Join-Path $sm 'journal')
                FBTK_CRASH_DIR = (Join-Path $sm 'crash'); FBTK_TELEMETRY_DIR = (Join-Path $sm 'telemetry'); FBTK_EVENTS_DIR = (Join-Path $sm 'events'); FBTK_BOT_LOADOUTS = '1'
                FBTK_ADMIN_FILE = (Join-Path $sm 'admin.json')
            }
            $saved = @{}
            foreach ($k in $set.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $set[$k]) }
            try {
                $script:srv = Start-Process -FilePath (Join-Path $rel 'GameServer.exe') -WorkingDirectory $rel -RedirectStandardOutput $srvLog `
                    -RedirectStandardError (Join-Path $sm 'server.err') -WindowStyle Hidden -PassThru
            }
            finally { foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) } }
            $null = $script:srv.Handle      # 핸들을 잡아 둬야 끝난 뒤 ExitCode 가 읽힌다 (Start-Process -PassThru)
            $deadline = (Get-Date).AddSeconds(90)
            while ((Get-Date) -lt $deadline) {
                if ($script:srv.HasExited) { return @{ Status = 'FAIL'; Detail = "부팅 중 종료 (exit $($script:srv.ExitCode)) - $(Tail $srvLog 3)" } }
                if (Select-String -Path $srvLog -Pattern '\[BOOT\] listen port=' -Quiet) {
                    $line = (Select-String -Path $srvLog -Pattern '\[BOOT\] listen port=').Line
                    return @{ Status = 'PASS'; Detail = ($line -replace '^.*\[BOOT\] ', '') }
                }
                Start-Sleep -Milliseconds 500
            }
            @{ Status = 'FAIL'; Detail = '90 초 안에 listen 줄이 없다' }
        }
        if ($script:srv -and -not $script:srv.HasExited) {
            $botLabel = "봇 $Bots 명 × $Seconds 초"
            Step '스모크' $botLabel {
                $env:FBTK_DATA_DIR = Join-Path $serverDir 'Data'
                $script:botProc = Start-Process -FilePath (Join-Path $rel 'DummyClient.exe') -ArgumentList @('-bots', "$Bots", '-ai', '-port', "$Port") -WorkingDirectory $rel `
                    -RedirectStandardOutput (Join-Path $sm 'bots.log') -RedirectStandardError (Join-Path $sm 'bots.err') -WindowStyle Hidden -PassThru
                Remove-Item Env:\FBTK_DATA_DIR
                $peakMb = 0; $begin = Get-Date; $end = $begin.AddSeconds($Seconds); $script:probeCode = $null
                $probeAt = [math]::Min(20, [math]::Max(8, [int] ($Seconds / 3)))      # 봇이 다 들어온 뒤 - 관리 포트 탐침 (공지 · 강제 종료 포함)
                while ((Get-Date) -lt $end) {
                    if ($script:srv.HasExited) { return @{ Status = 'FAIL'; Detail = "서버가 도중에 죽었다 (exit $($script:srv.ExitCode)) - $(Tail $srvLog 3)" } }
                    try { $peakMb = [math]::Max($peakMb, [math]::Round((Get-Process -Id $script:srv.Id).PrivateMemorySize64 / 1MB)) } catch {}
                    if ($null -eq $script:probeCode -and ((Get-Date) - $begin).TotalSeconds -ge $probeAt) {
                        $script:probeCode = Invoke-Logged 'python' @('-I', (Join-Path $PSScriptRoot 'admin_probe.py'), (Join-Path $sm 'admin.json'), (Join-Path $sm 'admin-probe.json'), '--notice', '--kick') (Join-Path $sm 'admin-probe.log')
                    }
                    Start-Sleep -Seconds 2
                }
                $script:peakMb = $peakMb
                $botsAlive = -not $script:botProc.HasExited
                if (-not $botsAlive) { return @{ Status = 'FAIL'; Detail = "봇 프로세스가 도중에 끝났다 - $(Tail (Join-Path $sm 'bots.log') 3)" } }
                @{ Status = 'PASS'; Detail = "서버 · 봇 끝까지 살아 있음 · 커밋 메모리 최대 $peakMb MB" }
            }
            Step '스모크' '관리 포트 (ServerMonitor 경로)' {
                $json = Join-Path $sm 'admin-probe.json'
                if ($null -eq $script:probeCode -or -not (Test-Path $json)) { return @{ Status = 'FAIL'; Detail = "탐침이 돌지 않았다 - $(Tail (Join-Path $sm 'admin-probe.log') 3)" } }
                $r = Get-Content $json -Raw -Encoding UTF8 | ConvertFrom-Json
                $names = @($r.checks.PSObject.Properties | ForEach-Object { $_.Name })
                $okN = @($r.checks.PSObject.Properties | Where-Object { $_.Value.ok }).Count
                if ($script:probeCode -eq 0) { @{ Status = 'PASS'; Detail = "$okN/$($names.Count) - 이벤트 $($r.events) · $($r.checks.map.detail)" } }
                else { @{ Status = 'FAIL'; Detail = "$okN/$($names.Count) - $($r.errors[0])" } }
            }
            Step '스모크' '이벤트 로그 (JSONL)' {
                $f = Get-ChildItem (Join-Path $sm 'events') -Filter *.jsonl -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $f) { return @{ Status = 'FAIL'; Detail = 'events/*.jsonl 이 없다' } }
                $lines = @(Get-Content $f.FullName -Encoding UTF8)
                $bad = 0; $tagCount = @{}
                foreach ($l in $lines) { try { $e = $l | ConvertFrom-Json; $tagCount[$e.tag] = 1 + [int] $tagCount[$e.tag] } catch { $bad++ } }
                $top = ($tagCount.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 6 | ForEach-Object { "$($_.Key) $($_.Value)" }) -join ' · '
                if ($bad -eq 0 -and $lines.Count -gt 0) { @{ Status = 'PASS'; Detail = "$($lines.Count) 줄 · 태그 $($tagCount.Count) 종 ($top)" } }
                else { @{ Status = 'FAIL'; Detail = "JSON 이 아닌 줄 $bad / $($lines.Count)" } }
            }
            if ($script:botProc -and -not $script:botProc.HasExited) { Stop-Process -Id $script:botProc.Id -Force; Start-Sleep -Seconds 3 }
            Step '스모크' 'Ctrl+C 정상 종료' {
                if ($script:srv.HasExited) { return @{ Status = 'FAIL'; Detail = '이미 종료돼 있었다' } }
                $sw = [Diagnostics.Stopwatch]::StartNew()
                & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'send-ctrlc.ps1') -ProcessId $script:srv.Id | Out-Null
                if (-not $script:srv.WaitForExit([int] ($th.shutdown_exit_seconds_max * 1000))) {
                    Stop-Process -Id $script:srv.Id -Force
                    return @{ Status = 'FAIL'; Detail = "$($th.shutdown_exit_seconds_max) 초 안에 끝나지 않아 강제 종료" }
                }
                $done = (Select-String -Path $srvLog -Pattern '\[SHUTDOWN\] done' | Select-Object -Last 1).Line
                if ($script:srv.ExitCode -eq 0 -and $done) { @{ Status = 'PASS'; Detail = "$([math]::Round($sw.Elapsed.TotalSeconds, 1)) 초 · " + ($done -replace '^.*\[SHUTDOWN\] ', '') } }
                elseif ($done) { @{ Status = 'WARN'; Detail = "종료 코드 $($script:srv.ExitCode) · $($done -replace '^.*\[SHUTDOWN\] ', '')" } }
                else { @{ Status = 'FAIL'; Detail = "[SHUTDOWN] done 줄이 없다 (exit $($script:srv.ExitCode))" } }
            }
        }
        if ($script:srv -and -not $script:srv.HasExited) { Stop-Process -Id $script:srv.Id -Force }

        # ── 판정 (로그 · 통계 CSV) ──
        if (Test-Path $srvLog) {
            $logText = Get-Content $srvLog -Encoding UTF8
            Step '스모크' '크래시 덤프' {
                $dumps = @(Get-ChildItem (Join-Path $sm 'crash') -Filter *.dmp -ErrorAction SilentlyContinue)
                if ($dumps.Count -eq 0) { @{ Status = 'PASS'; Detail = '0 개' } } else { @{ Status = 'FAIL'; Detail = "$($dumps.Count) 개 - $($dumps[0].FullName)" } }
            }
            Step '스모크' '오류 줄 (DB · 커밋 · 내부)' {
                # 게임 규칙의 실패(블링크 자리 없음 · 길찾기 실패)는 오류가 아니다 - DB · 저장 · 서버 내부만
                $rx = '(query|insert|update|delete|commit|begin|lock|load|connect|init|dispatch|accept slot) failed|refusing to start|\[CRASH\]|ASSERT|exception'
                $bad = @($logText | Where-Object { $_ -match $rx -and $_ -notmatch 'injected|TEST ONLY' })
                $bad | Set-Content (Join-Path $sm 'error-lines.txt') -Encoding UTF8
                $errFile = Join-Path $sm 'server.err'
                $stderr = @(if (Test-Path $errFile) { Get-Content $errFile | Where-Object { $_.Trim() } })
                $n = $bad.Count + $stderr.Count
                if ($n -le $th.error_lines_max) { @{ Status = 'PASS'; Detail = '0 줄' } }
                else { @{ Status = 'FAIL'; Detail = "$n 줄 (smoke\error-lines.txt) - $(@($bad + $stderr)[0])" } }
            }
            Step '스모크' '입장 · 활동' {
                $entered = @($logText | Select-String '\[ENTER\] point=' | ForEach-Object { if ($_.Line -match 'char=(\d+)') { $Matches[1] } } | Select-Object -Unique).Count
                $kills = @($logText | Select-String '\[DIED\]').Count
                $fires = @($logText | Select-String '\[SKILL FIRE\]').Count
                $loadouts = @($logText | Select-String '\[BOT LOADOUT\]').Count
                $ratio = if ($Bots -gt 0) { $entered / $Bots } else { 0 }
                $detail = "입장 $entered / $Bots · 스킬 $fires · 사망 $kills · 빌드 장착 $loadouts"
                if ($ratio -ge $th.entered_ratio_min -and $fires -gt 0) { @{ Status = 'PASS'; Detail = $detail } } else { @{ Status = 'FAIL'; Detail = "$detail (기준: 입장 $([math]::Round($th.entered_ratio_min * 100)) % 이상 · 스킬 1 회 이상)" } }
            }
            Step '스모크' '안티치트 (봇 오탐)' {
                $kicks = @($logText | Select-String '\[CHEAT\] kick').Count
                $bans = @($logText | Select-String '\[CHEAT\] ban').Count
                $warns = @($logText | Select-String '\[CHEAT\] warn').Count
                if ($kicks -le $th.cheat_kicks_max -and $bans -le $th.cheat_bans_max) { @{ Status = $(if ($warns) { 'WARN' } else { 'PASS' }); Detail = "kick $kicks · ban $bans · warn $warns" } }
                else { @{ Status = 'FAIL'; Detail = "kick $kicks · ban $bans - 정상 봇이 끊겼다" } }
            }
            Step '스모크' '이동 거부' {
                $lines = @($logText | Select-String '\[MOVE REJECT\] id=\d+ reason=(?!suppressed)').Count
                $sup = 0; $logText | Select-String 'reason=suppressed n=(\d+)' | ForEach-Object { $sup += [int] $_.Matches.Groups[1].Value }
                $rate = ($lines + $sup) / [math]::Max(1, $Bots * $Seconds / 60.0)
                $detail = "$($lines + $sup) 건 = 봇·분당 $([math]::Round($rate, 2)) (기준 $($th.move_rejects_per_bot_minute_max))"
                if ($rate -le $th.move_rejects_per_bot_minute_max) { @{ Status = 'PASS'; Detail = $detail } } else { @{ Status = 'FAIL'; Detail = $detail } }
            }
            Step '스모크' 'Room 플러시 · 메모리 (SV-1 · SV-6)' {
                $csv = Get-ChildItem (Join-Path $sm 'metrics') -Filter *.csv -ErrorAction SilentlyContinue | Select-Object -First 1
                if (-not $csv) { return @{ Status = 'FAIL'; Detail = '통계 CSV 가 없다' } }
                $rows = @(Import-Csv $csv.FullName | Select-Object -Skip ([int] $th.warmup_rows_skipped) | Where-Object { [int] $_.sessions -gt 0 })
                if ($rows.Count -eq 0) { return @{ Status = 'FAIL'; Detail = '접속자가 있는 통계 행이 없다' } }
                $p95 = ($rows | ForEach-Object { [double] $_.flush_p95_us } | Measure-Object -Maximum).Maximum / 1000
                $p99 = ($rows | ForEach-Object { [double] $_.flush_p99_us } | Measure-Object -Maximum).Maximum / 1000
                $max = ($rows | ForEach-Object { [double] $_.flush_max_us } | Measure-Object -Maximum).Maximum / 1000
                $mem = ($rows | ForEach-Object { [double] $_.commit_mb } | Measure-Object -Maximum).Maximum
                $detail = "P95 {0:N2} · P99 {1:N2} · max {2:N1} ms · 커밋 {3:N0} MB ({4} 행, 10 초 창마다 최악값)" -f $p95, $p99, $max, $mem, $rows.Count
                $fail = ($p95 -gt $th.flush_p95_ms) -or ($p99 -gt $th.flush_p99_ms) -or ($max -gt $th.flush_max_ms) -or ($mem -gt $th.commit_mb_max)
                if ($fail) { @{ Status = 'FAIL'; Detail = "$detail - 기준 P95 $($th.flush_p95_ms) · P99 $($th.flush_p99_ms) · max $($th.flush_max_ms) ms · $($th.commit_mb_max) MB" } }
                else { @{ Status = 'PASS'; Detail = $detail } }
            }
        }
    }
}

# ───────────────────────────── 정리 · 보고서 ─────────────────────────────
if ($dbReady -and -not $KeepDb -and ((Has 'db') -or (Has 'smoke'))) {
    foreach ($d in @($fresh, $migr, $smokeDb)) {
        [void] (Sql "DROP DATABASE IF EXISTS $d")
        [void] (Sql "REVOKE ALL PRIVILEGES ON $d.* FROM 'fbtk_app'@'localhost'; REVOKE ALL PRIVILEGES ON $d.* FROM 'fbtk_app'@'127.0.0.1'")
    }
}

$fails = @($results | Where-Object Status -eq 'FAIL').Count
$warns = @($results | Where-Object Status -eq 'WARN').Count
$verdict = if ($fails -gt 0) { "열지 말 것 - 실패 $fails 건" } elseif ($warns -gt 0) { "열어도 된다 - 경고 $warns 건은 볼 것" } else { '열어도 된다' }
$elapsed = [math]::Round(((Get-Date) - $started).TotalSeconds)

$md = New-Object System.Text.StringBuilder
[void] $md.AppendLine("# 서버 검증 보고서 - $(Get-Date -Format 'yyyy-MM-dd HH:mm')")
[void] $md.AppendLine('')
[void] $md.AppendLine("**판정: $verdict** · 단계 $($Stage -join ', ') · $elapsed 초 · 커밋 $(git -C $repo rev-parse --short HEAD 2>$null)")
[void] $md.AppendLine('')
[void] $md.AppendLine('| 묶음 | 검사 | 결과 | 내용 | 초 |')
[void] $md.AppendLine('|---|---|---|---|---|')
foreach ($r in $results) { [void] $md.AppendLine("| $($r.Group) | $($r.Check) | $($r.Status) | $(($r.Detail -replace '\|', '/')) | $($r.Seconds) |") }
[IO.File]::WriteAllText((Join-Path $Out 'report.md'), $md.ToString(), (New-Object Text.UTF8Encoding($true)))
[IO.File]::WriteAllText((Join-Path $Out 'results.json'), (@{ verdict = $verdict; fails = $fails; warns = $warns; stages = $Stage; seconds = $elapsed; results = $results } | ConvertTo-Json -Depth 4), (New-Object Text.UTF8Encoding($false)))

Write-Host ''
Write-Host "=== 판정: $verdict ($elapsed 초) - $(Join-Path $Out 'report.md') ===" -ForegroundColor $(if ($fails) { 'Red' } elseif ($warns) { 'Yellow' } else { 'Green' })
exit $(if ($fails -gt 0) { 1 } else { 0 })
