<#
.SYNOPSIS
    S1 클라이언트를 패키징한다 (Win64 · Development, 2026-10-18 첫 플레이테스트 패키지).

.DESCRIPTION
    1. Tools/package/gen_cook_dirs.py --check - CSV 로 경로만 불리는 애니메이션 · 재질이 쿡 목록에 다 있는지
    2. RunUAT BuildCookRun - 게임 타깃 빌드 · Floor1 쿡 · pak · 보관(archive) · 선행 설치 파일(prereqs)
    3. 패키지 옆에 Server/Data/*.csv 복사 - 클라가 ProjectDir/../Server/Data 를 읽는다 (DefaultGame.ini ItemDataDirectory)
    4. 접속 배치 파일 S1_접속.bat - 호스트 IP 를 받아 인증 · 게임 서버 주소를 -ini: 로 덮어쓴다

    Development 로 만든다 - 보스 예고 · 투사체 · 장판 · 탑 구역이 DrawDebug 라 Shipping / Test 에서는 사라진다 (TD-013).

.EXAMPLE
    .\Tools\package\package-client.ps1
    .\Tools\package\package-client.ps1 -OutDir D:\S1-playtest
#>
param(
    [string] $OutDir = ("C:\Server\Packages\S1-" + (Get-Date -Format 'yyyyMMdd-HHmm')),
    [ValidateSet('Development', 'Shipping')] [string] $Config = 'Development',
    [switch] $SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent		# Tools\package -> repo root
$project = Join-Path $root 'S1\S1.uproject'
$uat = 'C:\Unreal5.8\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat'
foreach ($p in @($project, $uat)) { if (-not (Test-Path $p)) { throw "찾을 수 없음: $p" } }

# 1. 쿡 목록 - CSV 가 바뀌었는데 목록이 그대로면 패키지에서 애니메이션이 빠진다
& python -I -X utf8 (Join-Path $root 'Tools\package\gen_cook_dirs.py') --check
if ($LASTEXITCODE -ne 0) { throw '쿡 목록이 낡았다 - python Tools/package/gen_cook_dirs.py 를 먼저 실행' }

# 에디터가 떠 있으면 빌드 · 쿡이 파일 잠금에 걸린다 (memory: build-while-running)
if (Get-Process UnrealEditor -ErrorAction SilentlyContinue) { throw 'UnrealEditor 가 실행 중이다 - 닫고 다시 실행' }

New-Item -ItemType Directory -Force $OutDir | Out-Null
$log = Join-Path $OutDir 'package.log'

$uatArgs = @(
    'BuildCookRun', "-project=`"$project`"", '-noP4', '-utf8output',
    '-platform=Win64', "-clientconfig=$Config",
    '-cook', '-stage', '-pak', '-archive', "-archivedirectory=`"$OutDir`"", '-prereqs'
)
if (-not $SkipBuild) { $uatArgs += '-build' } else { $uatArgs += '-skipbuild' }

Write-Host "[PACKAGE] $uat $($uatArgs -join ' ')"
& $uat @uatArgs 2>&1 | Tee-Object -FilePath $log | Select-String -Pattern 'BUILD SUCCESSFUL|BUILD FAILED|Error:|error C|AutomationTool exiting' | ForEach-Object { $_.Line }
$uatExit = $LASTEXITCODE
if ($uatExit -ne 0) { throw "BuildCookRun 실패 (exit $uatExit) - $log" }

$game = Join-Path $OutDir 'Windows'
if (-not (Test-Path (Join-Path $game 'S1.exe'))) { throw "S1.exe 가 없다: $game" }

# 3. 서버 데이터 - ProjectDir(= Windows\S1\) 의 ../Server/Data
$data = Join-Path $game 'Server\Data'
New-Item -ItemType Directory -Force $data | Out-Null
Copy-Item (Join-Path $root 'Server\Data\*.csv') $data -Force
$copied = (Get-ChildItem $data -Filter *.csv).Count
Write-Host "[PACKAGE] Server/Data csv $copied 개 -> $data"

# 4. 접속 배치 파일 (ASCII 명령 + 한글 안내는 echo 만 - cmd 코드 페이지 문제를 피해 chcp 65001)
$bat = @'
@echo off
chcp 65001 >nul
set /p HOST=Host IP (enter = 127.0.0.1):
if "%HOST%"=="" set HOST=127.0.0.1
start "" "%~dp0S1.exe" -windowed -ResX=1280 -ResY=720 -log "-ini:Game:[/Script/S1.S1LoginSubsystem]:AuthServerUrl=\"http://%HOST%:8080\"" "-ini:Game:[/Script/S1.S1LoginSubsystem]:GameServerHost=%HOST%" "-ini:Game:[/Script/S1.S1LoginSubsystem]:GameServerPort=7777"
'@
Set-Content -Path (Join-Path $game 'S1_Connect.bat') -Value $bat -Encoding ascii

Write-Host "[PACKAGE] 완료: $game\S1.exe · 접속: $game\S1_Connect.bat · 로그: $log"
