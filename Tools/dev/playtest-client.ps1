<#
.SYNOPSIS
    LAN 플레이테스트 테스터 PC 에서 클라를 호스트에 붙여 띄운다 (2026-10-18, docs/playtest-network.md).

.DESCRIPTION
    DefaultGame.ini 를 고치지 않는다 - 명령줄 -ini: 덮어쓰기로 인증 서버 · 게임 서버 주소만 바꾼다.
    로그인 화면(L1)부터 시작한다 (가입 · 로그인 · 캐릭터 이름). 패키징 전이라 에디터의 -game 으로 띄운다.

.EXAMPLE
    .\Tools\dev\playtest-client.ps1 -HostIp 192.168.0.12
#>
param(
    [Parameter(Mandatory = $true)] [string] $HostIp,
    [int] $GamePort = 7777,
    [int] $AuthPort = 8080,
    [int] $ResX = 1280,
    [int] $ResY = 720,
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$editor = 'C:\Unreal5.8\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$project = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) 'S1\S1.uproject'		# Tools\dev -> repo root
foreach ($p in @($editor, $project)) { if (-not (Test-Path $p)) { throw "찾을 수 없음: $p" } }

if ($HostIp -notmatch '^\d{1,3}(\.\d{1,3}){3}$') { throw "IPv4 주소가 아니다: $HostIp" }

# 섹션 [/Script/S1.S1LoginSubsystem] (DefaultGame.ini) - URL 은 따옴표로 (없으면 "http:" 에서 잘린다)
$section = '[/Script/S1.S1LoginSubsystem]'
$args2 = @(
    "`"$project`"", '-game', '-log', '-windowed', "-ResX=$ResX", "-ResY=$ResY",
    "-ini:Game:$($section):AuthServerUrl=`"http://$($HostIp):$AuthPort`"",
    "-ini:Game:$($section):GameServerHost=$HostIp",
    "-ini:Game:$($section):GameServerPort=$GamePort",
    # 개발 우회 토큰(에디터 바이너리만 읽는다)을 비운다 - 테스터는 로그인 화면(가입 · 로그인)부터
    '-ini:Game:[/Script/S1.S1GameInstance]:DevLoginToken='
)

Write-Host "[CLIENT] $editor $($args2 -join ' ')"
if ($DryRun) { exit 0 }
Start-Process -FilePath $editor -ArgumentList $args2
Write-Host "[CLIENT] S1/Saved/Logs/S1.log 의 '[LOGIN] login screen - auth=http://$($HostIp):$AuthPort game=$($HostIp):$GamePort' 줄로 주소를 확인한다"
