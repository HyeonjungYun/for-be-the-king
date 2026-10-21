# For be the King - adds load bots to the running local server (copied into the bundle as bots.ps1). ASCII only
#   bots.ps1            30 bots on floor 0 (the floor you play on) - PvP opponents with the AI brain (2026-10-20):
#                       each bot enters with a random skill build (server FBTK_BOT_LOADOUTS, bot_loadouts.csv - 7 weapons x 5)
#                       and plays it with a matching personality (bot_personas.csv - duel / pack / runner / balanced)
#   bots.ps1 -Count 120 -AllFloors   the load test (30 per floor x 4)
#   bots.ps1 -Classic   the old harness walk (wander + chase one bot) - for comparing load numbers with earlier runs
param([int] $Count = 30, [switch] $AllFloors, [switch] $Classic)
$root = $PSScriptRoot
$env:FBTK_DATA_DIR = "$root\server\Data"
$a = @('-bots', "$Count")
if (-not $Classic) { $a += @('-ai') }
if (-not $AllFloors) { $a += @('-floor', '0') }
Start-Process -FilePath "$root\server\DummyClient.exe" -ArgumentList $a -WorkingDirectory "$root\server" -WindowStyle Minimized
Write-Host "[OK] $Count bots joining. Close the DummyClient window (or Stop.bat) to remove them."
