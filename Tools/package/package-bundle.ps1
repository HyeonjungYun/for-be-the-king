# package-bundle.ps1 - all-in-one local bundle: client package + game server + auth server + MySQL with a ready database (2026-10-19). ASCII only
#
#   .\Tools\package\package-client.ps1               # first: the client package (Win64 Development)
#   .\Tools\package\package-bundle.ps1 -Zip          # then: the bundle (+ zip for GitHub Releases)
#
# The bundle runs on one PC only (127.0.0.1). MySQL listens on 3307 so it never collides with a MySQL the tester already has.
# The root password is a fixed local value - the server binds 127.0.0.1 only.
param(
    [string] $ClientPkg = 'C:\Server\Packages\S1-20261018\Windows',
    [string] $Out = ('C:\Server\Packages\ForBeTheKing-' + (Get-Date -Format 'yyyyMMdd')),
    [string] $MySqlHome = 'C:\mysql-8.0.46-winx64',
    [string] $DbPassword = 'fbtk-local-demo',
    [switch] $Zip
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent		# Tools\package -> repo root
$bin = Join-Path $repo 'Server\Binaries\Release'
foreach ($p in @("$ClientPkg\S1.exe", "$bin\GameServer.exe", "$bin\DummyClient.exe", "$MySqlHome\bin\mysqld.exe")) {
    if (-not (Test-Path $p)) { throw "missing: $p" }
}
if (Test-Path $Out) { throw "already exists: $Out (remove it first)" }
New-Item -ItemType Directory -Force $Out | Out-Null
# From here native tools (mysql warns on stderr) are judged by $LASTEXITCODE, not by stderr - PS 5.1 + Stop turns stderr into a throw
$ErrorActionPreference = 'Continue'

function Copy-Tree([string] $from, [string] $to, [string[]] $excludeFiles = @()) {
    $a = @($from, $to, '/E', '/NFL', '/NDL', '/NJH', '/NJS', '/NP')
    if ($excludeFiles.Count -gt 0) { $a += '/XF'; $a += $excludeFiles }
    & robocopy @a | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed $from -> $to ($LASTEXITCODE)" }
}

# 1. client (no debug symbols) + the server data the client reads next to it (ProjectDir/../Server/Data)
Write-Host '[1/6] client'
Copy-Tree $ClientPkg "$Out\client" @('*.pdb', 'S1_Connect.bat')
Remove-Item "$Out\client\S1\Saved" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Tree "$repo\Server\Data" "$Out\client\Server\Data"

# 2. game server + bots + data
#    The shipped GameServer.exe is built without the test hooks (FBTK_FAIL_* / FBTK_FAKE_* / FBTK_*_DELAY_MS / FBTK_TEST_MODE are compiled out,
#    ServerConfig.h) into a separate output dir so the developer binary in Binaries\Release keeps them (2026-10-21)
Write-Host '[2/6] game server (shipping build - FbtkTestHooks=0)'
$shipDir = Join-Path $env:TEMP 'fbtk-ship'
if (Test-Path $shipDir) { Remove-Item $shipDir -Recurse -Force }
$msbuild = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found (vswhere)' }
& $msbuild "$repo\Server\Server.sln" /t:GameServer /p:Configuration=Release /p:Platform=x64 /p:PreBuildEventUseInBuild=false /p:FbtkTestHooks=0 "/p:OutDir=$shipDir\" /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'shipping GameServer build failed' }
New-Item -ItemType Directory -Force "$Out\server" | Out-Null
Copy-Item "$shipDir\GameServer.exe" "$Out\server"
Copy-Item "$bin\DummyClient.exe" "$Out\server"
Copy-Item "$bin\*.dll" "$Out\server"
Copy-Tree "$repo\Server\Data" "$Out\server\Data"

# 3. auth server - self-contained, no .NET install needed
Write-Host '[3/6] auth server (dotnet publish)'
& dotnet publish "$repo\Server\AuthServer" -c Release -r win-x64 --self-contained true -p:PublishSingleFile=false -o "$Out\auth" --nologo -v quiet
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }

# 4. MySQL - only what the server needs (no pdb / debug / static libs)
Write-Host '[4/6] MySQL'
$my = "$Out\mysql"
New-Item -ItemType Directory -Force "$my\bin" | Out-Null
Copy-Item "$MySqlHome\bin\mysqld.exe", "$MySqlHome\bin\mysql.exe", "$MySqlHome\bin\mysqladmin.exe" "$my\bin"
Get-ChildItem "$MySqlHome\bin\*.dll" | Where-Object { $_.Name -notmatch 'debug' } | Copy-Item -Destination "$my\bin"
Copy-Tree "$MySqlHome\share" "$my\share"
Copy-Tree "$MySqlHome\lib\plugin" "$my\lib\plugin" @('*.pdb', '*.lib')
Copy-Tree "$MySqlHome\lib\private" "$my\lib\private" @('*.pdb', '*.lib')
foreach ($f in 'LICENSE', 'README') { if (Test-Path "$MySqlHome\$f") { Copy-Item "$MySqlHome\$f" $my } }

# 5. database - initialize, apply schema + seeds (Tools/db/setup_db.ps1), shut down
Write-Host '[5/6] database'
$common = @('--no-defaults', "--basedir=`"$my`"", "--datadir=`"$my\data`"")
& cmd.exe /c "`"$my\bin\mysqld.exe`" --no-defaults --basedir=`"$my`" --datadir=`"$my\data`" --initialize-insecure --console > `"$Out\init.log`" 2>&1"
if ($LASTEXITCODE -ne 0) { throw "mysqld --initialize failed - $Out\init.log" }
$d = Start-Process -FilePath "$my\bin\mysqld.exe" -ArgumentList ($common + @('--port=3307', '--bind-address=127.0.0.1', '--mysqlx=OFF')) -WindowStyle Hidden -PassThru
$deadline = (Get-Date).AddSeconds(60)
do { Start-Sleep -Milliseconds 500; $up = (Test-NetConnection 127.0.0.1 -Port 3307 -WarningAction SilentlyContinue -InformationLevel Quiet) } until ($up -or (Get-Date) -gt $deadline)
if (-not $up) { throw 'bundle MySQL did not start' }
& "$my\bin\mysql.exe" -h 127.0.0.1 -P 3307 -u root -e "ALTER USER 'root'@'localhost' IDENTIFIED BY '$DbPassword';"
if ($LASTEXITCODE -ne 0) { throw 'setting the root password failed' }
& "$repo\Tools\db\setup_db.ps1" -Password $DbPassword -Port 3307 -MySql "$my\bin\mysql.exe"
if ($LASTEXITCODE -ne 0) { throw 'setup_db failed' }
& "$my\bin\mysqladmin.exe" -h 127.0.0.1 -P 3307 -u root "-p$DbPassword" shutdown 2>$null
$d.WaitForExit(30000) | Out-Null
Remove-Item "$Out\init.log" -ErrorAction SilentlyContinue

# 6. launchers + readme
Write-Host '[6/6] launchers'
Copy-Item "$PSScriptRoot\bundle-start.ps1" "$Out\start.ps1"
Copy-Item "$PSScriptRoot\bundle-stop.ps1" "$Out\stop.ps1"
Copy-Item "$PSScriptRoot\bundle-bots.ps1" "$Out\bots.ps1"
$readme = [IO.File]::ReadAllText("$PSScriptRoot\bundle-readme.txt", [Text.Encoding]::UTF8)
[IO.File]::WriteAllText("$Out\README.txt", $readme, (New-Object Text.UTF8Encoding($true)))
foreach ($pair in @(@('Start.bat', 'start.ps1'), @('Stop.bat', 'stop.ps1'), @('AddBots.bat', 'bots.ps1'))) {
    $bat = "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0$($pair[1])`" %*`r`nif errorlevel 1 pause`r`n"
    [IO.File]::WriteAllText("$Out\$($pair[0])", $bat, [Text.Encoding]::ASCII)
}

$mb = [Math]::Round(((Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum) / 1MB)
Write-Host "[OK] bundle $Out ($mb MB)"

if ($Zip) {
    $zipPath = "$Out.zip"
    Write-Host "[..] zip -> $zipPath"
    & tar.exe -a -c -f $zipPath -C (Split-Path $Out -Parent) (Split-Path $Out -Leaf)
    if ($LASTEXITCODE -ne 0) { throw 'zip failed' }
    Write-Host ("[OK] zip {0:N0} MB" -f ((Get-Item $zipPath).Length / 1MB))
}
