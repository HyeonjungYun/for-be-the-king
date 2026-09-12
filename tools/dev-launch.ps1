<#
.SYNOPSIS
    개발용 클라이언트를 계정별로 띄운다. 패키징 불필요.

.DESCRIPTION
    시딩 계정의 토큰은 SHA256(계정명) 이다 (seed_bots.sql · seed_dev.sql).
    이 스크립트가 토큰을 계산해 -token= 으로 넘기므로 클라에 암호 코드가 필요 없다.

    UE 는 Windows 에서 SHA-256 을 제공하지 않는다 —
    FPlatformMisc::GetSHA256Signature 의 Generic 구현이 checkf(false) 로 막혀 있고
    Windows 오버라이드가 없다 (GenericPlatformMisc.cpp:2019). 그래서 여기서 계산한다.

.EXAMPLE
    .\tools\dev-launch.ps1 dev_1
    한 명. dev_1 로 접속

.EXAMPLE
    .\tools\dev-launch.ps1 dev_1 bot_1 bot_2 bot_3
    네 명 동시. AC-5 체인 CC 검증용

.EXAMPLE
    .\tools\dev-launch.ps1 -Token 1a43c87c...
    AuthServer 가 발급한 토큰을 직접 쓸 때

.NOTES
    🔴 GameServer 를 먼저 띄울 것. 안 돌면 클라가 Connection Failed 로 끝난다.
    🔴 같은 계정을 두 번 주면 두 번째가 거절된다 (AccountManager 중복 로그인 차단).
#>
[CmdletBinding(DefaultParameterSetName = 'Accounts')]
param(
    # 시딩 계정 이름. 여러 개 주면 그만큼 클라가 뜬다
    [Parameter(ParameterSetName = 'Accounts', Position = 0, ValueFromRemainingArguments = $true)]
    [string[]] $Accounts = @('dev_1'),

    # 64자 토큰 직접. AuthServer 발급분을 쓸 때
    [Parameter(ParameterSetName = 'Token', Mandatory = $true)]
    [string] $Token,

    [int] $ResX = 960,
    [int] $ResY = 540,

    # 전체화면으로 띄운다 (기본은 창)
    [switch] $Fullscreen,

    # 실행하지 않고 명령만 출력한다
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

$Editor  = 'C:\Unreal5.8\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe'
$Project = 'C:\Server\MMO\S1\S1.uproject'

foreach ($p in @($Editor, $Project)) {
    if (-not (Test-Path $p)) { throw "찾을 수 없음: $p" }
}

function Get-Sha256Hex([string] $Text) {
    # 서버의 Utils::Sha256Hex 와 같은 출력 — UTF-8 입력, 소문자 hex 64자
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($Text))
        return -join ($bytes | ForEach-Object { $_.ToString('x2') })
    }
    finally { $sha.Dispose() }
}

# ── 띄울 목록 만들기 ──────────────────────────────────────────────────────────
$targets = @()

if ($PSCmdlet.ParameterSetName -eq 'Token') {
    if ($Token.Length -ne 64) { throw "토큰은 64자여야 한다. 받은 길이: $($Token.Length)" }
    $targets += [pscustomobject]@{ Label = '(직접 지정)'; Token = $Token.ToLower() }
}
else {
    $dupes = $Accounts | Group-Object | Where-Object Count -gt 1
    if ($dupes) {
        throw "같은 계정이 중복됐다: $($dupes.Name -join ', ') — 두 번째 접속은 거절된다"
    }
    foreach ($a in $Accounts) {
        $targets += [pscustomobject]@{ Label = $a; Token = Get-Sha256Hex $a }
    }
}

# ── 서버가 떠 있나 ────────────────────────────────────────────────────────────
if (-not (Get-Process GameServer -ErrorAction SilentlyContinue)) {
    Write-Warning 'GameServer 가 실행 중이 아니다. 클라가 Connection Failed 로 끝난다.'
}

# ── 실행 ──────────────────────────────────────────────────────────────────────
$common = @($Project, '-game', '-log', "-ResX=$ResX", "-ResY=$ResY")
if (-not $Fullscreen) { $common += '-WINDOWED' }

foreach ($t in $targets) {
    $args = $common + @("-token=$($t.Token)")

    if ($DryRun) {
        Write-Host ('{0,-10} {1}' -f $t.Label, ($args -join ' '))
        continue
    }

    Write-Host ('{0,-10} token={1}...' -f $t.Label, $t.Token.Substring(0, 16))
    Start-Process -FilePath $Editor -ArgumentList $args | Out-Null

    # 동시에 던지면 셰이더 컴파일과 DDC 접근이 겹친다. 한 박자 띄운다
    if ($targets.Count -gt 1) { Start-Sleep -Milliseconds 1500 }
}

if (-not $DryRun) {
    Write-Host ''
    Write-Host ("{0}개 실행. 클라 로그에서 [LOGIN] token from command line 을," -f $targets.Count)
    Write-Host '서버 로그에서 [ITEM] enter char=<id> 를 확인할 것.'
}
