using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.RateLimiting;
using Microsoft.AspNetCore.RateLimiting;
using MySqlConnector;

// ── 인증 서버 ────────────────────────────────────────────────────────────────
//
// 게임 서버(C++ IOCP)는 비밀번호를 모른다. accounts 테이블을 읽는 것은 이 서버뿐이고,
// 검증에 성공하면 login_sessions 에 토큰 한 줄을 남긴다.
// 게임 서버는 C_LOGIN 에서 그 토큰을 SELECT 할 뿐이다 — 코드가 한 줄도 바뀌지 않는다.
//
// 캐릭터도 여기서 만든다 (ADR-0011). 캐릭터 행만 만들고 아이템은 만들지 않는다 —
// 아이템 id 는 게임 서버가 메모리에서 발급하므로 다른 프로세스가 행을 만들면 부딪힌다 (ADR-0004).

var builder = WebApplication.CreateBuilder(args);

// 수치는 appsettings.json "Auth" 에서 (account-login-mvp-2026-10-09.md § Tuning Knobs). 범위 밖이면 기동하지 않는다
var settings = builder.Configuration.GetSection("Auth").Get<AuthSettings>() ?? new AuthSettings();
settings.Validate();

// 🔴 비밀번호는 파일에 두지 않는다. 게임 서버(GameServer.cpp:49)와 같은 환경변수를 쓴다
var dbPassword = Environment.GetEnvironmentVariable("FBTK_DB_PASSWORD")
    ?? throw new InvalidOperationException("FBTK_DB_PASSWORD 가 설정되지 않았다");

var connectionString = new MySqlConnectionStringBuilder
{
    Server = builder.Configuration["Db:Host"] ?? "127.0.0.1",
    Port = builder.Configuration.GetValue<uint?>("Db:Port") ?? 3306,
    UserID = builder.Configuration["Db:User"] ?? "root",
    Password = dbPassword,
    Database = builder.Configuration["Db:Schema"] ?? "forbetheking",
}.ConnectionString;

builder.Services.AddSingleton(_ => new MySqlDataSource(connectionString));

// 무차별 대입 완화. PBKDF2 자체가 이미 느리므로 이쪽 목적은 서버 보호다.
// 🔴 IP 별로 나눈다 — 창 하나를 모두가 나눠 쓰면 테스터 8명이 가입 + 로그인만 해도 16회로 막힌다 (ADR-0011 R-2)
builder.Services.AddRateLimiter(options =>
{
    options.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
    options.AddPolicy("auth", context => Auth.PerIpWindow(context, settings.CredentialPermitPerMinute));
    options.AddPolicy("characters", context => Auth.PerIpWindow(context, settings.CharacterPermitPerMinute));
});

var app = builder.Build();
app.UseRateLimiter();

app.MapGet("/health", () => Results.Ok(new { status = "ok" }));

app.MapPost("/auth/register", async (Credentials body, MySqlDataSource db) =>
{
    if (string.IsNullOrWhiteSpace(body.Username) || body.Username.Length > Auth.MaxUsernameLength)
        return Results.BadRequest(new { error = "invalid_username" });

    if (body.Password is null || body.Password.Length < Auth.MinPasswordLength)
        return Results.BadRequest(new { error = "password_too_short" });

    byte[] salt = RandomNumberGenerator.GetBytes(Auth.SaltBytes);
    byte[] hash = Auth.Derive(body.Password, salt, Auth.Iterations);

    await using var conn = await db.OpenConnectionAsync();
    await using var cmd = conn.CreateCommand();

    cmd.CommandText = """
        INSERT INTO accounts (username, password_hash, password_salt, password_iterations)
        VALUES (@username, @hash, @salt, @iterations)
        """;
    cmd.Parameters.AddWithValue("@username", body.Username);
    cmd.Parameters.AddWithValue("@hash", hash);
    cmd.Parameters.AddWithValue("@salt", salt);
    cmd.Parameters.AddWithValue("@iterations", Auth.Iterations);

    try
    {
        await cmd.ExecuteNonQueryAsync();
    }
    catch (MySqlException e) when (e.ErrorCode == MySqlErrorCode.DuplicateKeyEntry)
    {
        // 아이디 중복은 UNIQUE KEY 가 잡는다. 미리 SELECT 로 확인하면
        // 그 사이에 다른 요청이 끼어들 수 있다
        return Results.Conflict(new { error = "username_taken" });
    }

    return Results.Created($"/auth/accounts/{body.Username}", new { username = body.Username });
}).RequireRateLimiting("auth");

app.MapPost("/auth/login", async (Credentials body, MySqlDataSource db) =>
{
    await using var conn = await db.OpenConnectionAsync();

    ulong accountId = 0;
    byte[] storedHash = [];
    byte[] salt = [];
    int iterations = Auth.Iterations;
    long bannedUntil = 0;     // unix s - 지금보다 뒤면 정지 중 (안티치트 2026-10-19, accounts.banned_until)

    await using (var select = conn.CreateCommand())
    {
        select.CommandText = """
            SELECT account_id, password_hash, password_salt, password_iterations,
                   IF(banned_until > NOW(), UNIX_TIMESTAMP(banned_until), 0)
            FROM accounts WHERE username = @username
            """;
        select.Parameters.AddWithValue("@username", body.Username ?? "");

        await using var reader = await select.ExecuteReaderAsync();
        if (await reader.ReadAsync())
        {
            accountId = reader.GetUInt64(0);
            storedHash = (byte[])reader[1];
            salt = (byte[])reader[2];
            iterations = reader.GetInt32(3);
            bannedUntil = reader.IsDBNull(4) ? 0 : Convert.ToInt64(reader.GetValue(4));
        }
    }

    // 🔴 계정이 없어도 같은 비용을 치른다.
    //    응답 시간 차이로 "그 아이디가 존재하는가" 가 새면 안 된다
    if (accountId == 0)
    {
        Auth.Derive(body.Password ?? "", new byte[Auth.SaltBytes], Auth.Iterations);
        return Results.Json(new { error = "invalid_credentials" }, statusCode: 401);
    }

    byte[] computed = Auth.Derive(body.Password ?? "", salt, iterations);

    // 바이트 단위 조기 반환이 없는 비교. == 로 비교하면 앞자리부터 맞춰 나갈 수 있다
    if (CryptographicOperations.FixedTimeEquals(computed, storedHash) == false)
        return Results.Json(new { error = "invalid_credentials" }, statusCode: 401);

    // 정지 중 - 비밀번호가 맞은 뒤에만 알린다 (틀린 비밀번호로 계정의 정지 여부를 캐지 못하게). 토큰은 주지 않는다
    if (bannedUntil > 0)
        return Results.Json(new { error = "account_suspended", until = bannedUntil }, statusCode: 403);

    string token = Auth.NewToken();

    await using var tx = await conn.BeginTransactionAsync();

    // 이전 토큰은 버린다. 게임 서버가 계정 단위로 중복 접속을 막으므로
    // 살려둬도 쓸 수 없고, 두면 행만 쌓인다
    await using (var purge = conn.CreateCommand())
    {
        purge.Transaction = tx;
        purge.CommandText = "DELETE FROM login_sessions WHERE account_id = @accountId";
        purge.Parameters.AddWithValue("@accountId", accountId);
        await purge.ExecuteNonQueryAsync();
    }

    // 🔴 만료 시각은 MySQL 의 NOW() 로 만든다.
    //    게임 서버가 expires_at > NOW() 로 검사하므로 같은 시계를 써야 한다
    await using (var insert = conn.CreateCommand())
    {
        insert.Transaction = tx;
        insert.CommandText = """
            INSERT INTO login_sessions (token, account_id, expires_at)
            VALUES (@token, @accountId, DATE_ADD(NOW(), INTERVAL @minutes MINUTE))
            """;
        insert.Parameters.AddWithValue("@token", Auth.HashToken(token));     // 저장은 해시 - 클라에는 원래 토큰을 준다 (2026-10-17)
        insert.Parameters.AddWithValue("@accountId", accountId);
        insert.Parameters.AddWithValue("@minutes", settings.TokenLifetimeMinutes);
        await insert.ExecuteNonQueryAsync();
    }

    await using (var touch = conn.CreateCommand())
    {
        touch.Transaction = tx;
        touch.CommandText = "UPDATE accounts SET last_login_at = NOW() WHERE account_id = @accountId";
        touch.Parameters.AddWithValue("@accountId", accountId);
        await touch.ExecuteNonQueryAsync();
    }

    await tx.CommitAsync();

    return Results.Ok(new
    {
        token,
        expires_in_seconds = settings.TokenLifetimeMinutes * 60
    });
}).RequireRateLimiting("auth");

// 이 계정의 캐릭터 목록. 0개면 클라가 이름 입력 화면(L2)을 띄운다
app.MapGet("/auth/characters", async (HttpRequest request, MySqlDataSource db) =>
{
    await using var conn = await db.OpenConnectionAsync();

    ulong accountId = await Auth.AccountFromToken(conn, Auth.BearerToken(request));
    if (accountId == 0)
        return Auth.InvalidToken();

    var characters = new List<object>();

    await using var select = conn.CreateCommand();
    select.CommandText = "SELECT character_id, name FROM characters WHERE account_id = @accountId ORDER BY character_id";
    select.Parameters.AddWithValue("@accountId", accountId);

    await using var reader = await select.ExecuteReaderAsync();
    while (await reader.ReadAsync())
        characters.Add(new { character_id = reader.GetUInt64(0), name = reader.GetString(1) });

    return Results.Ok(new { characters });
}).RequireRateLimiting("characters");

// 캐릭터 생성 - 이름만 받는다. hp · 위치 · 골드는 테이블 기본값, 시작 장비는 게임 서버가 입장 때 준다
app.MapPost("/auth/characters", async (HttpRequest request, NewCharacter body, MySqlDataSource db) =>
{
    await using var conn = await db.OpenConnectionAsync();

    ulong accountId = await Auth.AccountFromToken(conn, Auth.BearerToken(request));
    if (accountId == 0)
        return Auth.InvalidToken();

    string? name = body.Name;
    if (Auth.IsValidName(name, settings) == false)
        return Results.BadRequest(new { error = "invalid_name" });

    await using var tx = await conn.BeginTransactionAsync();

    // 🔴 계정 행을 먼저 잠근다. 같은 계정의 요청 두 개가 동시에 와서 둘 다 "0개" 를 보고
    //    하나씩 만드는 것을 막는다 (ADR-0011 R-5). 두 번째 요청은 첫 번째가 커밋할 때까지 여기서 기다린다
    await using (var lockAccount = conn.CreateCommand())
    {
        lockAccount.Transaction = tx;
        lockAccount.CommandText = "SELECT account_id FROM accounts WHERE account_id = @accountId FOR UPDATE";
        lockAccount.Parameters.AddWithValue("@accountId", accountId);
        await lockAccount.ExecuteScalarAsync();
    }

    long owned;
    await using (var count = conn.CreateCommand())
    {
        count.Transaction = tx;
        count.CommandText = "SELECT COUNT(*) FROM characters WHERE account_id = @accountId";
        count.Parameters.AddWithValue("@accountId", accountId);
        owned = Convert.ToInt64(await count.ExecuteScalarAsync());
    }

    if (owned >= settings.CharactersPerAccount)
    {
        await tx.RollbackAsync();
        return Results.Conflict(new { error = "character_limit" });
    }

    ulong characterId;
    await using (var insert = conn.CreateCommand())
    {
        insert.Transaction = tx;
        insert.CommandText = "INSERT INTO characters (account_id, name) VALUES (@accountId, @name)";
        insert.Parameters.AddWithValue("@accountId", accountId);
        insert.Parameters.AddWithValue("@name", name);

        try
        {
            await insert.ExecuteNonQueryAsync();
        }
        catch (MySqlException e) when (e.ErrorCode == MySqlErrorCode.DuplicateKeyEntry)
        {
            // 이름은 전역 유일 (uk_name, 대소문자 구분 없음). 아이디와 같은 이유로 미리 SELECT 하지 않는다
            await tx.RollbackAsync();
            return Results.Conflict(new { error = "name_taken" });
        }

        characterId = (ulong)insert.LastInsertedId;
    }

    await tx.CommitAsync();

    return Results.Created($"/auth/characters/{characterId}", new { character_id = characterId, name });
}).RequireRateLimiting("characters");

app.Run();

record Credentials(string? Username, string? Password);
record NewCharacter(string? Name);

// appsettings.json "Auth" - 기본값이 곧 quick-spec 의 기본값이다
sealed class AuthSettings
{
    public int TokenLifetimeMinutes { get; set; } = 60;
    public int CredentialPermitPerMinute { get; set; } = 10;    // 가입 · 로그인, IP 당
    public int CharacterPermitPerMinute { get; set; } = 30;     // 캐릭터 조회 · 생성, IP 당
    public int NameMinLength { get; set; } = 2;
    public int NameMaxLength { get; set; } = 12;
    public int CharactersPerAccount { get; set; } = 1;

    public void Validate()
    {
        Require(TokenLifetimeMinutes is >= 15 and <= 1440, "TokenLifetimeMinutes 는 15–1440");
        Require(CredentialPermitPerMinute is >= 5 and <= 60, "CredentialPermitPerMinute 는 5–60");
        Require(CharacterPermitPerMinute is >= 10 and <= 120, "CharacterPermitPerMinute 는 10–120");
        Require(NameMinLength >= 2 && NameMaxLength <= 16 && NameMinLength <= NameMaxLength, "NameMinLength 2 이상 · NameMaxLength 16 이하 · 최소 <= 최대");

        // 클라는 S_LOGIN 의 첫 캐릭터로, 게임 서버는 0 번으로 입장한다 - 늘리려면 선택 화면과 ADR-0011 개정이 먼저다
        Require(CharactersPerAccount == 1, "CharactersPerAccount 는 MVP 에서 1 고정 (ADR-0011 Decision 6)");
    }

    static void Require(bool ok, string message)
    {
        if (ok == false)
            throw new InvalidOperationException($"appsettings.json Auth: {message}");
    }
}

static class Auth
{
    public const int SaltBytes = 16;            // schema: password_salt BINARY(16)
    public const int HashBytes = 32;            // schema: password_hash BINARY(32)
    public const int TokenBytes = 32;           // schema: token CHAR(64) = 32바이트 hex
    public const int Iterations = 600_000;      // OWASP 2023 PBKDF2-HMAC-SHA256 권고
    public const int MinPasswordLength = 8;
    public const int MaxUsernameLength = 32;    // schema: username VARCHAR(32)

    // 한글 완성형 · 영문 · 숫자만. 🔴 '_' 는 금지 - 봇 재지급(RegearBot)이 이름의 "bot_" 접두사로 봇을 가린다.
    // 사람이 bot_x 를 만들면 매 입장 무료 장비를 받는다. 끝은 $ 가 아니라 \z - $ 는 마지막 줄바꿈 앞에서도 맞는다
    static readonly Regex NamePattern = new(@"^[가-힣A-Za-z0-9]+\z", RegexOptions.CultureInvariant);

    public static byte[] Derive(string password, byte[] salt, int iterations) =>
        Rfc2898DeriveBytes.Pbkdf2(
            Encoding.UTF8.GetBytes(password), salt, iterations,
            HashAlgorithmName.SHA256, HashBytes);

    public static string NewToken() =>
        Convert.ToHexString(RandomNumberGenerator.GetBytes(TokenBytes)).ToLowerInvariant();

    // login_sessions.token 에는 이 값만 들어간다 - DB 가 새도 그 값으로는 로그인할 수 없다 (2026-10-17 사용자 결정 · 로그인 OQ-2).
    // 게임 서버 Utils::Sha256Hex 와 같은 값: 원래 토큰(ASCII hex)의 SHA-256, 소문자 hex 64 자 = CHAR(64)
    public static string HashToken(string token) =>
        Convert.ToHexString(SHA256.HashData(Encoding.ASCII.GetBytes(token))).ToLowerInvariant();

    public static bool IsValidName(string? name, AuthSettings settings) =>
        name is not null
        && name.Length >= settings.NameMinLength
        && name.Length <= settings.NameMaxLength
        && NamePattern.IsMatch(name);

    // "Authorization: Bearer <64 hex>". 형식이 아니면 DB 에 묻지도 않는다
    public static string? BearerToken(HttpRequest request)
    {
        const string prefix = "Bearer ";

        string header = request.Headers.Authorization.ToString();
        if (header.StartsWith(prefix, StringComparison.OrdinalIgnoreCase) == false)
            return null;

        string token = header[prefix.Length..].Trim();
        return token.Length == TokenBytes * 2 && token.All(Uri.IsHexDigit) ? token : null;
    }

    // 게임 서버의 C_LOGIN 과 같은 질의 - 같은 토큰이 두 곳에서 같은 계정을 가리킨다 (ServerPacketHandler.cpp Handle_C_LOGIN)
    public static async Task<ulong> AccountFromToken(MySqlConnection conn, string? token)
    {
        if (token is null)
            return 0;

        await using var select = conn.CreateCommand();
        select.CommandText = "SELECT account_id FROM login_sessions WHERE token = @token AND expires_at > NOW()";
        select.Parameters.AddWithValue("@token", HashToken(token));

        object? result = await select.ExecuteScalarAsync();
        return result is null or DBNull ? 0 : Convert.ToUInt64(result);
    }

    public static IResult InvalidToken() =>
        Results.Json(new { error = "invalid_token" }, statusCode: StatusCodes.Status401Unauthorized);

    public static RateLimitPartition<string> PerIpWindow(HttpContext context, int permitPerMinute) =>
        RateLimitPartition.GetFixedWindowLimiter(
            context.Connection.RemoteIpAddress?.ToString() ?? "unknown",
            _ => new FixedWindowRateLimiterOptions
            {
                Window = TimeSpan.FromMinutes(1),
                PermitLimit = permitPerMinute,
                QueueLimit = 0,
            });
}
