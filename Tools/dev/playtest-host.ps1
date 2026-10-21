<#
.SYNOPSIS
    같은 공간 LAN 플레이테스트의 호스트 PC 를 띄운다 (2026-10-18, docs/playtest-network.md).

.DESCRIPTION
    1. 이 PC 의 LAN IPv4 를 찾는다 (테스터가 접속할 주소)
    2. 방화벽 인바운드 규칙 (TCP 7777 게임 서버 · 8080 인증 서버) - 관리자 권한이 없으면 명령만 보여 준다
    3. 인증 서버(AuthServer)를 0.0.0.0:8080 으로, 게임 서버를 0.0.0.0:7777 로 띄운다
    4. 테스터 PC 에서 돌릴 명령(Tools/dev/playtest-client.ps1 -HostIp <이 PC>)을 출력한다

    🔴 인증은 http 다 - 비밀번호가 평문으로 오간다. 같은 공간 LAN 까지만. 인터넷 원격은 docs/playtest-network.md § 원격 준비 목록.

.EXAMPLE
    .\Tools\dev\playtest-host.ps1
    .\Tools\dev\playtest-host.ps1 -DryRun      # 띄우지 않고 무엇을 할지만
#>
param(
    [int] $GamePort = 7777,
    [int] $AuthPort = 8080,
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'
$root = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent		# Tools\dev -> repo root
$bin = Join-Path $root 'Server\Binaries\Release'
$auth = Join-Path $root 'Server\AuthServer'

if ([string]::IsNullOrEmpty($env:FBTK_DB_PASSWORD)) { throw 'FBTK_DB_PASSWORD 가 없다 - DB 비밀번호 환경 변수를 먼저' }
if (-not (Test-Path (Join-Path $bin 'GameServer.exe'))) { throw "GameServer.exe 가 없다 - $bin (Release 빌드 먼저)" }

# 1. LAN 주소 - 사설 대역(10 · 172.16-31 · 192.168), 링크가 올라온 어댑터
$ips = @(Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
    Where-Object { $_.IPAddress -match '^(10\.|192\.168\.|172\.(1[6-9]|2[0-9]|3[01])\.)' -and $_.AddressState -eq 'Preferred' } |
    Select-Object -ExpandProperty IPAddress)
if ($ips.Count -eq 0) { Write-Host '[HOST] 사설 LAN 주소를 찾지 못했다 - 공유기에 연결돼 있는지 확인' -ForegroundColor Yellow }
else { Write-Host "[HOST] LAN 주소: $($ips -join ', ')" }

# 2. 방화벽
$rules = @(
    @{ Name = 'FBTK GameServer TCP'; Port = $GamePort },
    @{ Name = 'FBTK AuthServer TCP'; Port = $AuthPort }
)
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
foreach ($r in $rules) {
    $exists = Get-NetFirewallRule -DisplayName $r.Name -ErrorAction SilentlyContinue
    if ($exists) { Write-Host "[HOST] 방화벽 규칙 있음: $($r.Name)"; continue }
    $cmd = "New-NetFirewallRule -DisplayName '$($r.Name)' -Direction Inbound -Protocol TCP -LocalPort $($r.Port) -Action Allow -Profile Private"
    if ($isAdmin -and -not $DryRun) { Invoke-Expression $cmd | Out-Null; Write-Host "[HOST] 방화벽 규칙 추가: $($r.Name)" }
    else { Write-Host "[HOST] 방화벽 규칙 없음 - 관리자 PowerShell 에서: $cmd" -ForegroundColor Yellow }
}

if ($DryRun) {
    Write-Host "[HOST] (DryRun) AuthServer: Kestrel__Endpoints__Http__Url=http://0.0.0.0:$AuthPort dotnet run -c Release --project $auth"
    Write-Host "[HOST] (DryRun) GameServer: FBTK_BIND_ADDRESS=0.0.0.0 FBTK_PORT=$GamePort $bin\GameServer.exe"
    exit 0
}

# 3. 서버 둘 - 각자 창 하나 (로그를 볼 수 있게)
$env:Kestrel__Endpoints__Http__Url = "http://0.0.0.0:$AuthPort"
Start-Process -FilePath 'dotnet' -ArgumentList @('run', '-c', 'Release', '--project', "`"$auth`"") -WorkingDirectory $auth
Remove-Item Env:\Kestrel__Endpoints__Http__Url

$env:FBTK_BIND_ADDRESS = '0.0.0.0'
$env:FBTK_PORT = "$GamePort"
Start-Process -FilePath (Join-Path $bin 'GameServer.exe') -WorkingDirectory $bin
Remove-Item Env:\FBTK_BIND_ADDRESS
Remove-Item Env:\FBTK_PORT

# 4. 테스터 안내
$hostIp = if ($ips.Count -gt 0) { $ips[0] } else { '<이 PC 의 LAN 주소>' }
Write-Host ''
Write-Host "[HOST] 테스터 PC 에서:  .\Tools\dev\playtest-client.ps1 -HostIp $hostIp" -ForegroundColor Green
Write-Host "[HOST] 게임 서버 로그에 '[BOOT] listen port=$GamePort - accepting clients (bind=0.0.0.0)' 가 보이면 준비 끝"
