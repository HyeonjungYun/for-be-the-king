using System.Security.Cryptography;
using System.Text;
using Microsoft.AspNetCore.RateLimiting;
using MySqlConnector;

// ── 인증 서버 ────────────────────────────────────────────────────────────────
//
// 게임 서버(C++ IOCP)는 비밀번호를 모른다. accounts 테이블을 읽는 것은 이 서버뿐이고,
// 검증에 성공하면 login_sessions 에 토큰 한 줄을 남긴다.
// 게임 서버는 C_LOGIN 에서 그 토큰을 SELECT 할 뿐이다 — 코드가 한 줄도 바뀌지 않는다.

var builder = WebApplication.CreateBuilder(args);

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

// 무차별 대입 완화. PBKDF2 자체가 이미 느리므로 이쪽 목적은 서버 보호다
builder.Services.AddRateLimiter(options =>
{
    options.RejectionStatusCode = StatusCodes.Status429TooManyRequests;
    options.AddFixedWindowLimiter("auth", limiter =>
    {
        limiter.Window = TimeSpan.FromMinutes(1);
        limiter.PermitLimit = 10;
        limiter.QueueLimit = 0;
    });
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

    await using (var select = conn.CreateCommand())
    {
        select.CommandText = """
            SELECT account_id, password_hash, password_salt, password_iterations
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
        insert.Parameters.AddWithValue("@token", token);
        insert.Parameters.AddWithValue("@accountId", accountId);
        insert.Parameters.AddWithValue("@minutes", Auth.TokenLifetimeMinutes);
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
        expires_in_seconds = Auth.TokenLifetimeMinutes * 60
    });
}).RequireRateLimiting("auth");

app.Run();

record Credentials(string? Username, string? Password);

static class Auth
{
    public const int SaltBytes = 16;            // schema: password_salt BINARY(16)
    public const int HashBytes = 32;            // schema: password_hash BINARY(32)
    public const int TokenBytes = 32;           // schema: token CHAR(64) = 32바이트 hex
    public const int Iterations = 600_000;      // OWASP 2023 PBKDF2-HMAC-SHA256 권고
    public const int TokenLifetimeMinutes = 60;
    public const int MinPasswordLength = 8;
    public const int MaxUsernameLength = 32;    // schema: username VARCHAR(32)

    public static byte[] Derive(string password, byte[] salt, int iterations) =>
        Rfc2898DeriveBytes.Pbkdf2(
            Encoding.UTF8.GetBytes(password), salt, iterations,
            HashAlgorithmName.SHA256, HashBytes);

    public static string NewToken() =>
        Convert.ToHexString(RandomNumberGenerator.GetBytes(TokenBytes)).ToLowerInvariant();
}
