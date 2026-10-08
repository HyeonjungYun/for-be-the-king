# Starter kit end-to-end on the real server + DB (account-login-mvp-2026-10-09.md Core Rule 8 / AC-AL-06, ADR-0011 Decision 4).
# Raw TCP as bot_119 (seeded SHA256 token). Backs the item tables up first and restores them at the end (row-for-row check).
#   A. no owned rows      -> enter: [STARTER] filled=3  -> /escape: 3 rows owned + EQUIPPED (INSERT of isNew)
#                         -> enter again: no kit (owns 3)  -> /die + leave: rows go to the bag  -> enter: kit again
#   B. one VAULTED row only (inserted while the server is down - item_rows_outside_game_server exception) -> enter: no kit
#
#   powershell -File Server\Tools\starter_kit_test.ps1
# ASCII only - PowerShell 5.1 reads BOM-less files as cp949.
param([string] $Bin = 'C:\Server\MMO\Server\Binaries\Release')

$ErrorActionPreference = 'Stop'
$mysql = 'C:\mysql-8.0.46-winx64\bin\mysql.exe'
$pw = "-p$env:FBTK_DB_PASSWORD"
# mysql.exe warns about the command-line password on stderr - PowerShell 5.1 turns that into an error under 'Stop'
function Sql([string] $q) { $ErrorActionPreference = 'Continue'; & $mysql -uroot $pw -D forbetheking -N -e $q 2>$null }

$log = Join-Path $env:TEMP 'starter_kit_test'
New-Item -ItemType Directory -Force $log | Out-Null

$script:passed = 0
$script:failures = 0
function Check([string] $label, [bool] $ok, [string] $detail = '') {
    if ($ok) { $script:passed++; "  PASS  $label" } else { $script:failures++; "  FAIL  $label  $detail" }
}

function Token([string] $account) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    -join ($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($account)) | ForEach-Object { $_.ToString('x2') })
}

# PacketHeader { uint16 size (header included); uint16 id } little-endian + protobuf body
function Send-Packet($stream, [int] $id, [byte[]] $body) {
    $size = 4 + $body.Length
    # each element in parentheses - inside @(...) the comma binds tighter than -band / -shr
    $packet = [byte[]](@(($size -band 0xFF), ($size -shr 8), ($id -band 0xFF), ($id -shr 8)) + $body)
    $stream.Write($packet, 0, $packet.Length)
}

function String-Field([int] $field, [string] $text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($text)
    return [byte[]](@((($field -shl 3) -bor 2), $bytes.Length) + $bytes)
}

function Read-Exact($stream, [int] $count) {
    $buffer = New-Object byte[] $count
    $read = 0
    while ($read -lt $count) {
        $n = $stream.Read($buffer, $read, $count - $read)
        if ($n -le 0) { throw 'connection closed' }
        $read += $n
    }
    return , $buffer
}

# Skip packets until one with this id arrives. Returns its body, or $null on timeout
function Wait-Packet($stream, [int] $id) {
    try {
        while ($true) {
            $header = Read-Exact $stream 4
            # [int] first - -shl on a [byte] stays a byte
            $size = $header[0] + ([int]$header[1] -shl 8)
            $got = $header[2] + ([int]$header[3] -shl 8)
            $body = if ($size -gt 4) { Read-Exact $stream ($size - 4) } else { , [byte[]]@() }
            if ($got -eq $id) { return , $body }
        }
    }
    catch { return $null }
}

function Succeeded($body) { return ($null -ne $body -and $body.Length -ge 2 -and $body[0] -eq 0x08 -and $body[1] -eq 0x01) }

function Start-Server([string] $tag) {
    Get-Process GameServer -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 500
    $vars = @{ FBTK_MONSTERS = '0'; FBTK_ENTRY_FROM_DB = '0'; FBTK_DEBUG_ESCAPE = '1'; FBTK_DEBUG_COMMANDS = '1'; FBTK_BOT_REGEAR = '0' }
    foreach ($k in $vars.Keys) { Set-Item "env:$k" $vars[$k] }
    $p = Start-Process -FilePath "$Bin\GameServer.exe" -WorkingDirectory $Bin -RedirectStandardOutput "$log\$tag-server.log" -RedirectStandardError "$log\$tag-server.err" -WindowStyle Hidden -PassThru
    foreach ($k in $vars.Keys) { Remove-Item "env:$k" -ErrorAction SilentlyContinue }
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        try { $c = New-Object System.Net.Sockets.TcpClient; $c.Connect('127.0.0.1', 7777); $c.Close(); return $p } catch { }
    }
    throw "GameServer did not open 7777 ($tag)"
}

function Stop-Server($p) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500 }

function Connect-And-Login([string] $account) {
    $client = New-Object System.Net.Sockets.TcpClient
    $client.Connect('127.0.0.1', 7777)
    $client.ReceiveTimeout = 6000
    $stream = $client.GetStream()
    Send-Packet $stream 1000 (String-Field 1 (Token $account))                 # C_LOGIN
    $ok = Succeeded (Wait-Packet $stream 1001)                                # S_LOGIN
    return @{ client = $client; stream = $stream; ok = $ok }
}

function Enter($stream) {
    Send-Packet $stream 1002 ([byte[]]@(0x18, 0x09))                           # C_ENTER_GAME { entry_point_id = 9 }
    $ok = Succeeded (Wait-Packet $stream 1003)                                # S_ENTER_GAME
    Start-Sleep -Milliseconds 400                                             # async log writer
    return $ok
}

function Starters([string] $tag, [string] $charId) {
    @(Select-String -Path "$log\$tag-server.log" -Pattern "^\[STARTER\] char=$charId filled=3" -ErrorAction SilentlyContinue).Count
}

function Owned([string] $charId) { Sql "SELECT item_type_id, state FROM item_instances WHERE owner_character_id = $charId ORDER BY item_type_id" }

$account = 'bot_119'
$charId = Sql "SELECT character_id FROM characters WHERE name = '$account'"
"character $account = $charId"

Get-Process GameServer, DummyClient -ErrorAction SilentlyContinue | Stop-Process -Force
Sql "DROP TABLE IF EXISTS sk_items_bak, sk_chars_bak, sk_bags_bak; CREATE TABLE sk_items_bak AS SELECT * FROM item_instances; CREATE TABLE sk_chars_bak AS SELECT * FROM characters; CREATE TABLE sk_bags_bak AS SELECT * FROM bags;"
$applied = Sql 'SELECT COUNT(*) FROM applied_commits'
"backup: $(Sql 'SELECT (SELECT COUNT(*) FROM sk_items_bak),(SELECT COUNT(*) FROM sk_chars_bak),(SELECT COUNT(*) FROM sk_bags_bak)')  applied_commits=$applied"

$srv = $null
try {
    "A. nothing owned"
    Sql "DELETE FROM item_instances WHERE owner_character_id = $charId"
    $srv = Start-Server 'a'
    $s = Connect-And-Login $account
    Check 'login' $s.ok
    Check 'enter' (Enter $s.stream)
    Check 'bare character gets the kit on enter ([STARTER] filled=3)' ((Starters 'a' $charId) -eq 1) "got $(Starters 'a' $charId)"

    Send-Packet $s.stream 1017 (String-Field 1 '/escape')                    # C_CHAT
    $left = Wait-Packet $s.stream 1005                                        # S_LEAVE_GAME
    Start-Sleep -Milliseconds 800
    $rows = @(Owned $charId)
    Check 'escape commits the kit as owned rows' ($null -ne $left -and $rows.Count -eq 3 -and ($rows -join ';') -match '^101\s+2;401\s+2;501\s+2$') "got $($rows -join ';')"

    Check 'enter again' (Enter $s.stream)
    Check 'no kit while it owns items' ((Starters 'a' $charId) -eq 1) "got $(Starters 'a' $charId)"

    Send-Packet $s.stream 1017 (String-Field 1 '/die')
    Start-Sleep -Milliseconds 800
    Send-Packet $s.stream 1004 ([byte[]]@())                                  # C_LEAVE_GAME (the client does this after death)
    $null = Wait-Packet $s.stream 1005
    Start-Sleep -Milliseconds 800
    $rows = @(Owned $charId)
    Check 'death moves the items into the bag (no owned rows)' ($rows.Count -eq 0) "got $($rows -join ';')"

    Check 'enter after death' (Enter $s.stream)
    Check 'kit again after losing everything' ((Starters 'a' $charId) -eq 2) "got $(Starters 'a' $charId)"
    $s.client.Close()
    Stop-Server $srv; $srv = $null

    "B. one vaulted item only"
    Sql "DELETE FROM item_instances WHERE owner_character_id = $charId"
    # server is down - the in-memory id counter restarts from MAX + 1 at boot (ADR-0004)
    Sql "INSERT INTO item_instances (instance_id, item_type_id, grade, level, state, slot, owner_character_id, bag_id, skill_id_primary, skill_id_secondary) SELECT MAX(instance_id) + 1, 102, 1, 0, 3, 0, $charId, NULL, 1001, 0 FROM item_instances"
    $srv = Start-Server 'b'
    $s = Connect-And-Login $account
    Check 'enter with a vaulted item' ($s.ok -and (Enter $s.stream))
    Check 'no kit when the vault is not empty' ((Starters 'b' $charId) -eq 0) "got $(Starters 'b' $charId)"
    $s.client.Close()
}
finally {
    if ($srv) { Stop-Server $srv }
    Get-Process GameServer -ErrorAction SilentlyContinue | Stop-Process -Force
    Sql "SET FOREIGN_KEY_CHECKS=0; DELETE FROM bags; DELETE FROM item_instances; DELETE FROM characters; INSERT INTO characters SELECT * FROM sk_chars_bak; INSERT INTO item_instances SELECT * FROM sk_items_bak; INSERT INTO bags SELECT * FROM sk_bags_bak; SET FOREIGN_KEY_CHECKS=1;"
    if ($applied -eq '0') { Sql 'DELETE FROM applied_commits' }
    $union = Sql 'SELECT (SELECT COUNT(*) FROM (SELECT * FROM item_instances UNION SELECT * FROM sk_items_bak) u),(SELECT COUNT(*) FROM (SELECT * FROM characters UNION SELECT * FROM sk_chars_bak) u),(SELECT COUNT(*) FROM (SELECT * FROM bags UNION SELECT * FROM sk_bags_bak) u)'
    $now = Sql 'SELECT (SELECT COUNT(*) FROM item_instances),(SELECT COUNT(*) FROM characters),(SELECT COUNT(*) FROM bags)'
    "restore: now $now  union $union  (equal = identical)"
    Sql 'DROP TABLE IF EXISTS sk_items_bak, sk_chars_bak, sk_bags_bak'
}

""
"passed $script:passed  failed $script:failures   (server logs: $log)"
if ($script:failures -gt 0) { exit 1 }
