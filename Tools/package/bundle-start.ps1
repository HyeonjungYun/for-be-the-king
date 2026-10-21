# For be the King - all-in-one local launcher (copied into the bundle as start.ps1 by Tools/package/package-bundle.ps1). ASCII only
# MySQL (port 3307, this folder only) -> auth server (8080) -> game server (7777) -> client. Everything stays on 127.0.0.1.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$pw = 'fbtk-local-demo'
$logs = Join-Path $root 'logs'
New-Item -ItemType Directory -Force $logs | Out-Null

function Test-Port([int] $port) {
    $c = New-Object Net.Sockets.TcpClient
    try { $c.Connect('127.0.0.1', $port); return $true } catch { return $false } finally { $c.Close() }
}
function Wait-Port([int] $port, [int] $seconds, [string] $name, $proc) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-Port $port) { Write-Host "[OK] $name ready (port $port)"; return }
        if ($proc -and $proc.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }
    throw "$name did not start (port $port). See $logs"
}
function Ours([string] $name) {
    Get-Process $name -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) }
}

# Something else already on 7777 / 8080 would steal the client's connection
foreach ($p in @(@{ Port = 7777; Name = 'GameServer' }, @{ Port = 8080; Name = 'AuthServer' })) {
    if ((Test-Port $p.Port) -and -not (Ours $p.Name)) { throw "Port $($p.Port) is already used by another program. Close it and run Start.bat again." }
}

# 1. MySQL
$my = Join-Path $root 'mysql'
if (-not (Test-Port 3307)) {
    Write-Host '[..] starting MySQL'
    $a = @('--no-defaults', "--basedir=`"$my`"", "--datadir=`"$my\data`"", '--port=3307', '--bind-address=127.0.0.1', '--mysqlx=OFF', "--log-error=`"$logs\mysql.err`"")
    $mysqld = Start-Process -FilePath "$my\bin\mysqld.exe" -ArgumentList $a -WindowStyle Hidden -PassThru
    Wait-Port 3307 90 'MySQL' $mysqld
}
# AddBots bots are local test accounts - forgive their anti-cheat strikes on every start so a long session never leaves them banned.
# Real accounts are untouched (their names cannot hold '_'). mysql warns about the password on stderr - PS 5.1 + Stop would throw on it
$ErrorActionPreference = 'Continue'
& "$my\bin\mysql.exe" -h 127.0.0.1 -P 3307 -u root "-p$pw" -D forbetheking -e "UPDATE accounts SET banned_until = NULL WHERE username LIKE 'bot\_%'; DELETE s FROM cheat_strikes s JOIN accounts a ON a.account_id = s.account_id WHERE a.username LIKE 'bot\_%';" 2>&1 | Out-Null
$ErrorActionPreference = 'Stop'

# 2. auth server
if (-not (Ours 'AuthServer')) {
    Write-Host '[..] starting auth server'
    $env:FBTK_DB_PASSWORD = $pw
    $env:Db__Port = '3307'
    $auth = Start-Process -FilePath "$root\auth\AuthServer.exe" -WorkingDirectory "$root\auth" -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$logs\auth.log" -RedirectStandardError "$logs\auth.err"
    Wait-Port 8080 60 'AuthServer' $auth
}

# 3. game server
if (-not (Ours 'GameServer')) {
    Write-Host '[..] starting game server'
    $env:FBTK_DB_PASSWORD = $pw
    $env:FBTK_DB_PORT = '3307'
    $env:FBTK_DATA_DIR = "$root\server\Data"
    $env:FBTK_BOT_LOADOUTS = '1'     # bot_ characters enter with a random skill build (AddBots.bat) - real players are never bot_ (names cannot hold '_')
    $game = Start-Process -FilePath "$root\server\GameServer.exe" -WorkingDirectory "$root\server" -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$logs\gameserver.log" -RedirectStandardError "$logs\gameserver.err"
    Wait-Port 7777 120 'GameServer' $game
}

# 4. client - login screen first (sign up there)
Write-Host '[..] starting the client'
# DisableAllScreenMessages - a Development build prints debug lines at the top left
Start-Process -FilePath "$root\client\S1.exe" -ArgumentList @('-windowed', '-ResX=1600', '-ResY=900', '-ExecCmds="DisableAllScreenMessages"')
Write-Host '[OK] done. Sign up on the login screen. Stop.bat shuts everything down.'
