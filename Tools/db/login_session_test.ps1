# GameServer login-session regression test (ADR-0011 Migration 2 / AC-AL-07, 2026-10-09). Talks raw TCP so it can do what no
# client does: two C_LOGIN on one connection, or hang up while the login is still in the DB queue. Boots GameServer.exe itself
# once per scenario (each needs different test env vars) and stops it afterwards. Uses the seeded bot tokens (SHA256 of the name).
#
#   powershell -File Tools\db\login_session_test.ps1                 # all scenarios (needs a binary with the test hooks)
#   powershell -File Tools\db\login_session_test.ps1 -Scenario double  # only the double-login one (works on any binary)
#
# ASCII only - PowerShell 5.1 reads BOM-less files as cp949.
param(
    [ValidateSet('all', 'double', 'fail', 'race')] [string] $Scenario = 'all',
    [string] $Bin = 'C:\Server\MMO\Server\Binaries\Release'
)

$ErrorActionPreference = 'Stop'
$log = Join-Path $env:TEMP 'login_session_test'
New-Item -ItemType Directory -Force $log | Out-Null

$script:failed = 0
$script:passed = 0
function Check([string] $label, [bool] $ok, [string] $detail = '') {
    if ($ok) { $script:passed++; "  PASS  $label" } else { $script:failed++; "  FAIL  $label  $detail" }
}

function Token([string] $account) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($account)) | ForEach-Object { $_.ToString('x2') })
}

# PacketHeader { uint16 size (header included); uint16 id } little-endian + protobuf C_LOGIN { string token = 1; }
function Send-Login($stream, [string] $token) {
    $body = [byte[]](@(0x0A, $token.Length) + [Text.Encoding]::ASCII.GetBytes($token))
    $size = 4 + $body.Length
    # each element in parentheses - inside @(...) the comma binds tighter than -band / -shr
    $packet = [byte[]](@(($size -band 0xFF), ($size -shr 8), (1000 -band 0xFF), (1000 -shr 8)) + $body)
    $stream.Write($packet, 0, $packet.Length)
}

function Read-Exact($stream, [int] $count) {
    $buffer = New-Object byte[] $count
    $read = 0
    while ($read -lt $count) {
        $n = $stream.Read($buffer, $read, $count - $read)
        if ($n -le 0) { throw 'connection closed' }
        $read += $n
    }
    return $buffer
}

# Next S_LOGIN (id 1001) on this connection, skipping anything else the server sends. $true / $false, or 'timeout'.
# S_LOGIN { bool success = 1; ... } - protobuf writes field 1 first, and leaves it out when false
function Read-LoginResult($stream) {
    try {
        while ($true) {
            $header = Read-Exact $stream 4
            # [int] first - -shl on a [byte] stays a byte, so 0x03 -shl 8 is 0
            $size = $header[0] + ([int]$header[1] -shl 8)
            $id = $header[2] + ([int]$header[3] -shl 8)
            $body = if ($size -gt 4) { Read-Exact $stream ($size - 4) } else { [byte[]]@() }
            if ($id -eq 1001) { return ($body.Length -ge 2 -and $body[0] -eq 0x08 -and $body[1] -eq 0x01) }
        }
    }
    catch { return 'timeout' }
}

function Open-Session {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', 7777)
    $client.ReceiveTimeout = 4000
    return $client
}

# One fresh connection, one login, hang up. $true when the server let the account in
function Login-Once([string] $account) {
    $client = Open-Session
    $stream = $client.GetStream()
    Send-Login $stream (Token $account)
    $result = Read-LoginResult $stream
    $client.Close()
    Start-Sleep -Milliseconds 400        # let OnDisconnected release the account before the next attempt
    return $result
}

function Start-Server([string] $tag, [hashtable] $envVars) {
    Get-Process GameServer -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    $env:FBTK_MONSTERS = '0'
    foreach ($k in $envVars.Keys) { Set-Item "env:$k" $envVars[$k] }
    $p = Start-Process -FilePath "$Bin\GameServer.exe" -WorkingDirectory $Bin -RedirectStandardOutput "$log\$tag-server.log" -RedirectStandardError "$log\$tag-server.err" -WindowStyle Hidden -PassThru
    foreach ($k in $envVars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        try { $c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 7777); $c.Close(); return $p } catch { }
    }
    throw "GameServer did not open 7777 ($tag)"
}

function Stop-Server($p) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500 }

$a = 'bot_119'; $b = 'bot_120'

if ($Scenario -in 'all', 'double') {
    "double - a second C_LOGIN on a logged-in connection must not take another account and strand the first"
    $srv = Start-Server 'double' @{}
    try {
        $client = Open-Session
        $stream = $client.GetStream()
        Send-Login $stream (Token $a)
        $first = Read-LoginResult $stream
        Send-Login $stream (Token $b)
        $second = Read-LoginResult $stream
        $client.Close()
        Start-Sleep -Milliseconds 400
        Check "first C_LOGIN ($a) -> success" ($first -eq $true) "got $first"
        Check "second C_LOGIN ($b) on the same connection -> rejected" ($second -eq $false) "got $second"
        $againA = Login-Once $a
        $againB = Login-Once $b
        Check "$a can log in again after that connection closed (not stranded)" ($againA -eq $true) "got $againA"
        Check "$b can log in (never held)" ($againB -eq $true) "got $againB"
    }
    finally { Stop-Server $srv }
}

if ($Scenario -in 'all', 'fail') {
    "fail - a DB error while loading characters must answer failure and release the account (FBTK_FAKE_LOGIN_FAIL=1)"
    $srv = Start-Server 'fail' @{ FBTK_FAKE_LOGIN_FAIL = '1' }
    try {
        $client = Open-Session
        $stream = $client.GetStream()
        Send-Login $stream (Token $a)
        $injected = Read-LoginResult $stream
        Send-Login $stream (Token $a)
        $retry = Read-LoginResult $stream
        $client.Close()
        Start-Sleep -Milliseconds 400
        Check "injected character-query failure -> S_LOGIN false (was: success with no characters)" ($injected -eq $false) "got $injected"
        Check "retry on the same connection -> success (account was released)" ($retry -eq $true) "got $retry"
        Check "fresh connection -> success" ((Login-Once $a) -eq $true)
    }
    finally { Stop-Server $srv }
}

if ($Scenario -in 'all', 'race') {
    "race - hanging up while the login is still in the DB queue must not leave the account online (FBTK_LOGIN_DELAY_MS=1500)"
    $srv = Start-Server 'race' @{ FBTK_LOGIN_DELAY_MS = '1500' }
    try {
        $client = Open-Session
        Send-Login ($client.GetStream()) (Token $a)
        Start-Sleep -Milliseconds 200
        $client.Close()                   # OnDisconnected runs now - the DB job has not stored the account yet
        Start-Sleep -Milliseconds 2500    # job finishes on a dead session
        Check "$a can log in after the abandoned login finished" ((Login-Once $a) -eq $true)
    }
    finally { Stop-Server $srv }
}

""
"passed $script:passed  failed $script:failed   (server logs: $log)"
if ($script:failed -gt 0) { exit 1 }
