# run-scale.ps1 - concurrent-user load test (2026-11-21, docs/server-scaling-10k.md). ASCII only.
#
#   .\Tools\loadtest\run-scale.ps1 -Bots 2000 -Seconds 180 -Label baseline-2k
#   .\Tools\loadtest\run-scale.ps1 -Bots 10000 -Procs 3 -Seconds 150 -ServerEnv @{ FBTK_TICK_MS = '100' } -BotArgs @('-movehz','4')
#   .\Tools\loadtest\run-scale.ps1 -Bots 10000 -Procs 4 -Servers 2 ...     # step 2: two game servers + the auth server's lobby
#
# Starts -Servers GameServer processes (server_id 1..N, ports Port+10*i) against the fbtk_scale schema (Tools/db/seed_bots_scale.sql -
# bot_1..bot_10000). With more than one server it also starts the auth server, and every bot process asks its lobby
# (GET /lobby/assign) which game server to use. -Bots are spread over -Procs DummyClient processes in -scale mode. Samples CPU and
# memory every 10 s, stops everything (Ctrl+C for the servers) and writes Tools/loadtest/reports/<time>-<label>/summary.json + .md.
param(
    [int] $Bots = 1000,
    [int] $Procs = 1,
    [int] $Servers = 1,
    [int] $Seconds = 120,
    [int] $ConnRate = 400,          # per bot process
    [int] $Ticks = 3,               # tick threads per bot process
    [int] $Io = 3,                  # IOCP threads per bot process
    [string] $Label = 'run',
    [string] $Schema = 'fbtk_scale',
    [int] $Port = 7821,
    [int] $AdminPort = 7822,
    [int] $AuthPort = 8091,
    [switch] $Lobby,                # use the auth server's lobby even with one server
    [hashtable] $ServerEnv = @{},
    [string[]] $BotArgs = @(),
    [switch] $KeepRunning,          # leave servers + bots up at the end (for the ServerMonitor recording)
    [string] $Out = ''
)
$ErrorActionPreference = 'Continue'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$rel = Join-Path $repo 'Server\Binaries\Release'
$authExe = Join-Path $repo 'Server\AuthServer\bin\Release\net10.0\AuthServer.exe'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
if ($Out -eq '') { $Out = Join-Path $PSScriptRoot ("reports\$stamp-$Label") }
New-Item -ItemType Directory -Force $Out | Out-Null
$useLobby = $Lobby -or ($Servers -gt 1)

Get-Process GameServer, DummyClient, AuthServer -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

# test schema only: lift bans / strikes left by an earlier overloaded run (anti-cheat kicks bots whose moves arrive in bursts,
# and a banned bot account keeps failing login - it looked like a connection stall on 2026-11-22), and forget earlier servers
$mysql = 'C:\mysql-8.0.46-winx64\bin\mysql.exe'
if ($Schema -ne 'forbetheking' -and (Test-Path $mysql) -and $env:FBTK_DB_PASSWORD) {
    & $mysql -uroot "-p$($env:FBTK_DB_PASSWORD)" -D $Schema -e "UPDATE accounts SET banned_until = NULL WHERE banned_until IS NOT NULL; DELETE FROM cheat_strikes; DELETE FROM online_accounts; DELETE FROM game_servers;" 2>$null
}

# --- game servers ---
$srvs = @()
for ($i = 0; $i -lt $Servers; $i++) {
    $dir = if ($Servers -eq 1) { $Out } else { Join-Path $Out "server-$($i + 1)" }
    New-Item -ItemType Directory -Force $dir | Out-Null
    $envSet = @{
        FBTK_DATA_DIR = (Join-Path $repo 'Server\Data'); FBTK_DB_SCHEMA = $Schema; FBTK_DB_USER = 'fbtk_app'
        FBTK_PORT = "$($Port + 10 * $i)"; FBTK_ADMIN_PORT = "$($AdminPort + 10 * $i)"; FBTK_ADMIN_FILE = (Join-Path $dir 'admin.json')
        FBTK_SERVER_ID = "$($i + 1)"
        FBTK_EVENTS_DIR = (Join-Path $dir 'events'); FBTK_METRICS_DIR = (Join-Path $dir 'metrics'); FBTK_JOURNAL_DIR = (Join-Path $dir 'journal')
        FBTK_TELEMETRY_DIR = (Join-Path $dir 'telemetry'); FBTK_CRASH_DIR = (Join-Path $dir 'crash')
    }
    foreach ($k in $ServerEnv.Keys) { $envSet[$k] = [string]$ServerEnv[$k] }
    foreach ($k in $envSet.Keys) { [Environment]::SetEnvironmentVariable($k, $envSet[$k]) }
    $p = Start-Process -FilePath (Join-Path $rel 'GameServer.exe') -WorkingDirectory $rel -RedirectStandardOutput (Join-Path $dir 'server.log') `
        -RedirectStandardError (Join-Path $dir 'server.err') -WindowStyle Hidden -PassThru
    $null = $p.Handle
    foreach ($k in $envSet.Keys) { [Environment]::SetEnvironmentVariable($k, $null) }
    $srvs += @{ p = $p; dir = $dir; port = $Port + 10 * $i; id = $i + 1 }
}

foreach ($s in $srvs) {
    $log = Join-Path $s.dir 'server.log'
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 500
        if ($s.p.HasExited) { break }
        if (Select-String -Path $log -Pattern 'accepting clients' -Quiet) { break }
    }
    if ($s.p.HasExited -or -not (Select-String -Path $log -Pattern 'accepting clients' -Quiet)) {
        Write-Host "server $($s.id) did not boot - see $log"
        exit 1
    }
    Write-Host "[LOAD] server $($s.id) up pid=$($s.p.Id) port=$($s.port)"
}

# --- auth server (lobby) ---
$auth = $null
if ($useLobby) {
    $auth = Start-Process -FilePath $authExe -ArgumentList @("--Kestrel:Endpoints:Http:Url=http://127.0.0.1:$AuthPort", "--Db:Schema=$Schema") `
        -WorkingDirectory (Split-Path $authExe) -RedirectStandardOutput (Join-Path $Out 'auth.log') -RedirectStandardError (Join-Path $Out 'auth.err') -WindowStyle Hidden -PassThru
    $null = $auth.Handle
    $ok = $false
    for ($t = 0; $t -lt 40 -and -not $ok; $t++) {
        Start-Sleep -Milliseconds 500
        try { $r = Invoke-RestMethod "http://127.0.0.1:$AuthPort/lobby/servers" -TimeoutSec 2; $ok = ($r.servers.Count -ge $Servers) } catch {}
    }
    Write-Host "[LOAD] auth server up port=$AuthPort lobby_ok=$ok"
}

# --- bots ---
$per = [math]::Ceiling($Bots / $Procs)
$botProcs = @()
for ($p = 0; $p -lt $Procs; $p++) {
    $n = [math]::Min($per, $Bots - $p * $per)
    if ($n -le 0) { break }
    # each bot process connects from its own loopback address (127.0.0.10+p) - a run right after another would otherwise hit the
    # previous run's TIME_WAIT (same address/port pairs, 120 s on Windows) and stall for two minutes
    $a = @('-scale', '-bots', "$n", '-botbase', "$($p * $per)", '-port', "$Port", '-connrate', "$ConnRate", '-ticks', "$Ticks", '-io', "$Io",
        '-srcip', "127.0.0.$(10 + $p + (Get-Random -Minimum 0 -Maximum 20) * 10)") + $BotArgs
    if ($useLobby) { $a += @('-assign', "http://127.0.0.1:$AuthPort") }
    $botProcs += Start-Process -FilePath (Join-Path $rel 'DummyClient.exe') -ArgumentList $a -WorkingDirectory $rel `
        -RedirectStandardOutput (Join-Path $Out "bots-$p.log") -RedirectStandardError (Join-Path $Out "bots-$p.err") -WindowStyle Hidden -PassThru
    if ($useLobby) { Start-Sleep -Milliseconds 300 }     # let the lobby see the previous assignment
}
foreach ($b in $botProcs) { $null = $b.Handle }
Write-Host "[LOAD] $($botProcs.Count) bot process(es), $Bots bots, $Servers server(s)"

# --- sample ---
$samples = @()
$cpuCount = [Environment]::ProcessorCount
$t0 = Get-Date
$prev = @{}
while (((Get-Date) - $t0).TotalSeconds -lt $Seconds) {
    Start-Sleep -Seconds 10
    $now = Get-Date
    $row = [ordered]@{ t = [int](($now - $t0).TotalSeconds) }
    $watch = @()
    foreach ($s in $srvs) { $watch += @{ n = $(if ($Servers -eq 1) { 'server' } else { "server$($s.id)" }); p = $s.p } }
    for ($b = 0; $b -lt $botProcs.Count; $b++) { $watch += @{ n = "bots$b"; p = $botProcs[$b] } }
    foreach ($pr in $watch) {
        try {
            $proc = Get-Process -Id $pr.p.Id -ErrorAction Stop
            $cpu = $proc.TotalProcessorTime.TotalSeconds
            $pct = 0
            if ($prev.ContainsKey($pr.n)) { $pct = [math]::Round(($cpu - $prev[$pr.n].cpu) / (($now - $prev[$pr.n].at).TotalSeconds) / $cpuCount * 100, 1) }
            $prev[$pr.n] = @{ cpu = $cpu; at = $now }
            $row["$($pr.n)_cpu_pct"] = $pct
            $row["$($pr.n)_mem_mb"] = [int]($proc.PrivateMemorySize64 / 1MB)
            $row["$($pr.n)_threads"] = $proc.Threads.Count
        } catch { $row["$($pr.n)_alive"] = $false }
    }
    $os = Get-CimInstance Win32_OperatingSystem
    $row['free_mem_mb'] = [int]($os.FreePhysicalMemory / 1KB)
    $samples += [pscustomobject]$row
    $srvCpu = ($row.Keys | Where-Object { $_ -like 'server*_cpu_pct' } | ForEach-Object { $row[$_] } | Measure-Object -Sum).Sum
    $line = "[LOAD] t=$($row.t)s servers cpu=$srvCpu% free=$($row.free_mem_mb)MB"
    $last = Get-Content (Join-Path $Out 'bots-0.log') -Tail 30 -ErrorAction SilentlyContinue | Where-Object { $_ -match '\[SCALE\] t=' } | Select-Object -Last 1
    if ($last) { $line += "  | " + ($last -replace '^.*\[SCALE\] ', '') }
    Write-Host $line
    if (@($srvs | Where-Object { $_.p.HasExited }).Count -gt 0) { Write-Host '[LOAD] a server exited!'; break }
}

$samples | Export-Csv -NoTypeInformation -Encoding UTF8 (Join-Path $Out 'samples.csv')

if (-not $KeepRunning) {
    foreach ($b in $botProcs) { Stop-Process -Id $b.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
    foreach ($s in $srvs) {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repo 'Tools\verify\send-ctrlc.ps1') -ProcessId $s.p.Id | Out-Null
    }
    foreach ($s in $srvs) { if (-not $s.p.WaitForExit(20000)) { Stop-Process -Id $s.p.Id -Force -ErrorAction SilentlyContinue } }
    if ($auth) { Stop-Process -Id $auth.Id -Force -ErrorAction SilentlyContinue }
}

# --- summary ---
& python (Join-Path $PSScriptRoot 'summarize.py') $Out
