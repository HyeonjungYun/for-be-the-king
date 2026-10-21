# For be the King - stops everything this bundle started (copied into the bundle as stop.ps1). ASCII only
#
# 2026-10-21: the game server gets a graceful stop first - a CTRL_C on its console. The server handles it (GameServer.cpp
# OnConsoleCtrl): stops accepting, drops sessions, drains the DB queue and exits on its own. Only if it is still alive after
# the wait do we terminate it. The others are terminated directly as before.
$root = $PSScriptRoot
function Ours([string] $name) {
    Get-Process $name -ErrorAction SilentlyContinue | Where-Object { $_.Path -and $_.Path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase) }
}

# Raise CTRL_C on another process' console. Done in a child powershell so this script keeps its own console:
# detach, attach to the target's console, ignore the event ourselves, raise it, detach.
$ctrlc = @'
param([int] $TargetPid)
$sig = @"
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool AttachConsole(uint dwProcessId);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool FreeConsole();
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
[DllImport("kernel32.dll", SetLastError=true)] public static extern bool GenerateConsoleCtrlEvent(uint dwCtrlEvent, uint dwProcessGroupId);
"@
$k = Add-Type -MemberDefinition $sig -Name Kernel -Namespace CtrlC -PassThru
[CtrlC.Kernel]::FreeConsole() | Out-Null
if (-not [CtrlC.Kernel]::AttachConsole([uint32] $TargetPid)) { exit 2 }
[CtrlC.Kernel]::SetConsoleCtrlHandler([IntPtr]::Zero, $true) | Out-Null
$ok = [CtrlC.Kernel]::GenerateConsoleCtrlEvent(0, 0)
Start-Sleep -Milliseconds 200
[CtrlC.Kernel]::FreeConsole() | Out-Null
if ($ok) { exit 0 } else { exit 3 }
'@
$ctrlcPath = Join-Path $env:TEMP 'fbtk-ctrlc.ps1'
[IO.File]::WriteAllText($ctrlcPath, $ctrlc, [Text.Encoding]::ASCII)

foreach ($p in @(Ours 'GameServer')) {
    Write-Host "[..] stop GameServer ($($p.Id)) - ctrl-c, waiting up to 15 s"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ctrlcPath $p.Id
    if ($p.WaitForExit(15000)) {
        Write-Host "[OK] GameServer exited (code $($p.ExitCode))"
    } else {
        Write-Host "[!!] GameServer did not exit in 15 s - terminating"
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
}
foreach ($name in 'S1', 'DummyClient', 'AuthServer') {
    Ours $name | ForEach-Object { Write-Host "[..] stop $name ($($_.Id))"; Stop-Process -Id $_.Id -Force }
}
# MySQL - clean shutdown so the data files stay consistent
$admin = Join-Path $root 'mysql\bin\mysqladmin.exe'
& $admin -h 127.0.0.1 -P 3307 -u root -pfbtk-local-demo shutdown 2>$null
Write-Host '[OK] stopped'
