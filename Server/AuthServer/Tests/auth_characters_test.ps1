# AuthServer integration test - account & login MVP (design/quick-specs/account-login-mvp-2026-10-09.md AC-AL-01..04, ADR-0011).
# Needs a running AuthServer (http://127.0.0.1:8080) on the real DB. Creates accounts named <prefix>* and deletes them at the end
# (ON DELETE CASCADE removes their characters and login_sessions). ASCII only - PowerShell 5.1 reads BOM-less files as cp949,
# so Korean test names are built from code points. Uses curl.exe: UTF-8 bodies, and --interface 127.0.0.2 for the per-IP limit.
#
#   powershell -File Server\AuthServer\Tests\auth_characters_test.ps1
#
# Run it at most once a minute: the last case (AC-AL-04) spends the whole credential window of 127.0.0.1.
param([string] $BaseUrl = 'http://127.0.0.1:8080')

$ErrorActionPreference = 'Stop'
$mysql = 'C:\mysql-8.0.46-winx64\bin\mysql.exe'
$pw = "-p$env:FBTK_DB_PASSWORD"
# mysql.exe warns about the command-line password on stderr - PowerShell 5.1 turns that into an error under 'Stop'
function Sql([string] $q) { $ErrorActionPreference = 'Continue'; & $mysql -uroot $pw -D forbetheking -N --default-character-set=utf8mb4 -e $q 2>$null }

$tmp = Join-Path $env:TEMP 'auth_characters_test'
New-Item -ItemType Directory -Force $tmp | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding $false

$script:failed = 0
$script:passed = 0
function Check([string] $label, [bool] $ok, [string] $detail = '') {
    if ($ok) { $script:passed++; "  PASS  $label" }
    else { $script:failed++; "  FAIL  $label  $detail" }
}

# One HTTP call. Returns @{ code = 201; json = <parsed or $null>; raw = '...' }
function Call([string] $method, [string] $path, $body = $null, [string] $token = '', [string] $iface = '') {
    $out = Join-Path $tmp ([guid]::NewGuid().ToString() + '.json')
    $cargs = @('-s', '-o', $out, '-w', '%{http_code}', '-X', $method, "$BaseUrl$path")
    if ($token) { $cargs += @('-H', "Authorization: Bearer $token") }
    if ($iface) { $cargs += @('--interface', $iface) }
    if ($null -ne $body) {
        $in = Join-Path $tmp ([guid]::NewGuid().ToString() + '.req')
        [IO.File]::WriteAllText($in, (ConvertTo-Json $body -Compress), $utf8)
        $cargs += @('-H', 'Content-Type: application/json', '--data-binary', "@$in")
    }
    $code = [int](& curl.exe @cargs)
    $raw = if (Test-Path $out) { [IO.File]::ReadAllText($out, $utf8) } else { '' }
    $json = $null
    if ($raw) { try { $json = $raw | ConvertFrom-Json } catch { } }
    return @{ code = $code; json = $json; raw = $raw }
}

function K([int[]] $codes) { -join ($codes | ForEach-Object { [char]$_ }) }

$prefix = 'aatest' + (Get-Random -Minimum 100000 -Maximum 999999)
$userA = "${prefix}a"; $userB = "${prefix}b"
$password = 'correct-horse-9'
$nameA = 'Tn' + (Get-Random -Minimum 100000 -Maximum 999999)                       # ASCII, 8 chars
$nameKo1 = (K 0xC6A9, 0xC0AC) + (Get-Random -Minimum 1000 -Maximum 9999)            # Korean 2 + digits 4
$nameKo2 = (K 0xBC29, 0xB791, 0xC790) + (Get-Random -Minimum 100 -Maximum 999)     # Korean 3 + digits 3
"prefix=$prefix"

if ((Call 'GET' '/health').code -ne 200) { "AuthServer is not answering at $BaseUrl"; exit 2 }

try {
    "AC-AL-01 register"
    $r = Call 'POST' '/auth/register' @{ username = $userA; password = $password }
    Check 'new account -> 201' ($r.code -eq 201) "got $($r.code) $($r.raw)"
    $r = Call 'POST' '/auth/register' @{ username = $userA; password = $password }
    Check 'same username -> 409 username_taken' ($r.code -eq 409 -and $r.json.error -eq 'username_taken') "got $($r.code) $($r.raw)"
    $r = Call 'POST' '/auth/register' @{ username = "${prefix}c"; password = '1234567' }
    Check '7-char password -> 400 password_too_short' ($r.code -eq 400 -and $r.json.error -eq 'password_too_short') "got $($r.code) $($r.raw)"

    "AC-AL-02 login and token"
    $r = Call 'POST' '/auth/login' @{ username = $userA; password = 'wrong-password' }
    Check 'wrong password -> 401 invalid_credentials' ($r.code -eq 401 -and $r.json.error -eq 'invalid_credentials') "got $($r.code)"
    $r = Call 'POST' '/auth/login' @{ username = $userA; password = $password }
    Check 'login -> 200 + 64-hex token' ($r.code -eq 200 -and $r.json.token -match '^[0-9a-f]{64}$') "got $($r.code) $($r.raw)"
    $tokenA = $r.json.token
    $r = Call 'GET' '/auth/characters' $null $tokenA
    Check 'new account has no characters -> 200 []' ($r.code -eq 200 -and @($r.json.characters).Count -eq 0) "got $($r.code) $($r.raw)"
    $forged = -join ((1..64) | ForEach-Object { '0123456789abcdef'[(Get-Random -Maximum 16)] })
    $r = Call 'GET' '/auth/characters' $null $forged
    Check 'forged token -> 401 invalid_token' ($r.code -eq 401 -and $r.json.error -eq 'invalid_token') "got $($r.code)"
    $r = Call 'GET' '/auth/characters'
    Check 'no Authorization header -> 401' ($r.code -eq 401) "got $($r.code)"
    $expired = -join ((1..64) | ForEach-Object { '0123456789abcdef'[(Get-Random -Maximum 16)] })
    Sql "INSERT INTO login_sessions (token, account_id, expires_at) SELECT '$expired', account_id, DATE_SUB(NOW(), INTERVAL 1 MINUTE) FROM accounts WHERE username = '$userA'"
    $r = Call 'GET' '/auth/characters' $null $expired
    Check 'expired token -> 401' ($r.code -eq 401) "got $($r.code)"

    "AC-AL-03 character name and limit"
    foreach ($bad in @('a', ('x' * 13), 'bot_1', 'ab cd', "abc`n", (K 0x3131, 0x3134))) {   # last: bare jamo, not a syllable
        $r = Call 'POST' '/auth/characters' @{ name = $bad } $tokenA
        Check ("invalid name '{0}' -> 400" -f ($bad -replace "`n", '\n')) ($r.code -eq 400 -and $r.json.error -eq 'invalid_name') "got $($r.code) $($r.raw)"
    }
    $r = Call 'POST' '/auth/characters' @{ name = $nameA } $tokenA
    Check 'valid name -> 201' ($r.code -eq 201 -and $r.json.name -eq $nameA -and $r.json.character_id -gt 0) "got $($r.code) $($r.raw)"
    $charA = $r.json.character_id
    $r = Call 'POST' '/auth/characters' @{ name = ($nameA + 'x').Substring(1) } $tokenA
    Check 'second character on the same account -> 409 character_limit' ($r.code -eq 409 -and $r.json.error -eq 'character_limit') "got $($r.code) $($r.raw)"
    $r = Call 'GET' '/auth/characters' $null $tokenA
    Check 'list -> the one character' ($r.code -eq 200 -and @($r.json.characters).Count -eq 1 -and $r.json.characters[0].character_id -eq $charA) "got $($r.raw)"

    $null = Call 'POST' '/auth/register' @{ username = $userB; password = $password }
    $r = Call 'POST' '/auth/login' @{ username = $userB; password = $password }
    $tokenB = $r.json.token
    $r = Call 'POST' '/auth/characters' @{ name = $nameA } $tokenB
    Check "other account, same name -> 409 name_taken" ($r.code -eq 409 -and $r.json.error -eq 'name_taken') "got $($r.code) $($r.raw)"
    $r = Call 'POST' '/auth/characters' @{ name = $nameA.ToLowerInvariant() } $tokenB
    Check "other account, same name in lower case -> 409 name_taken" ($r.code -eq 409 -and $r.json.error -eq 'name_taken') "got $($r.code) $($r.raw)"

    # Two creates for the same account at once - the account row lock lets exactly one through
    $jobs = foreach ($n in @($nameKo1, $nameKo2)) {
        $in = Join-Path $tmp ([guid]::NewGuid().ToString() + '.req')
        [IO.File]::WriteAllText($in, (ConvertTo-Json @{ name = $n } -Compress), $utf8)
        Start-Job -ScriptBlock { param($u, $t, $f) & curl.exe -s -o NUL -w '%{http_code}' -X POST $u -H "Authorization: Bearer $t" -H 'Content-Type: application/json' --data-binary "@$f" } `
            -ArgumentList "$BaseUrl/auth/characters", $tokenB, $in
    }
    $codes = @($jobs | Wait-Job | Receive-Job | ForEach-Object { [int]$_ } | Sort-Object)
    $jobs | Remove-Job
    Check "two concurrent creates -> 201 + 409" (($codes -join ',') -eq '201,409') "got $($codes -join ',')"
    $countB = Sql "SELECT COUNT(*) FROM characters c JOIN accounts a ON a.account_id = c.account_id WHERE a.username = '$userB'"
    Check "DB has exactly one character for B" ($countB -eq '1') "got $countB"
    $r = Call 'GET' '/auth/characters' $null $tokenB
    $got = if ($r.json) { $r.json.characters[0].name } else { '' }
    Check "Korean name round-trips through HTTP + DB" ($got -eq $nameKo1 -or $got -eq $nameKo2) "got '$got' raw $($r.raw)"

    "AC-AL-04 per-IP credential limit"
    # This run has spent 7 credential requests from 127.0.0.1 (4 register + 3 login); the window is 10 a minute.
    # Five more: requests 8-10 still answer 401, the 11th and 12th are 429
    $codes = @()
    for ($i = 0; $i -lt 5; $i++) { $codes += (Call 'POST' '/auth/login' @{ username = $userA; password = 'wrong-password' }).code }
    Check "127.0.0.1: 8th-10th -> 401, 11th-12th -> 429" (($codes -join ',') -eq '401,401,401,429,429') "got $($codes -join ',')"
    $r = Call 'POST' '/auth/login' @{ username = $userA; password = 'wrong-password' } '' '127.0.0.2'
    Check "127.0.0.2 is a separate window -> 401, not 429" ($r.code -eq 401) "got $($r.code)"
}
finally {
    Sql "DELETE FROM accounts WHERE username LIKE '$prefix%'"
    $left = Sql "SELECT (SELECT COUNT(*) FROM accounts WHERE username LIKE '$prefix%'), (SELECT COUNT(*) FROM characters c LEFT JOIN accounts a ON a.account_id = c.account_id WHERE a.account_id IS NULL)"
    "cleanup: accounts left / orphan characters = $left"
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

""
"passed $script:passed  failed $script:failed"
if ($script:failed -gt 0) { exit 1 }
