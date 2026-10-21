using System.Collections.Concurrent;
using System.IO;
using System.Net.Sockets;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace ServerMonitor;

/// <summary>관리 포트 접속 하나 (PROTOCOL.md). 줄마다 JSON. 받은 줄은 큐에 쌓고 UI 타이머가 꺼내 간다 (UI 스레드를 막지 않게).</summary>
public sealed class AdminClient : IDisposable
{
    static readonly JsonSerializerOptions WriteOptions = new() { Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    TcpClient? _tcp;
    NetworkStream? _stream;
    CancellationTokenSource? _cts;
    readonly object _sendLock = new();

    public ConcurrentQueue<JsonDocument> Inbox { get; } = new();
    public bool Connected => _tcp?.Connected == true && _closedReason == null;
    volatile string? _closedReason;
    public string? ClosedReason => _closedReason;
    public long BytesReceived;

    /// <summary>접속 · hello. 실패하면 예외 (메시지를 그대로 보여 준다).</summary>
    public async Task ConnectAsync(int port, string token, CancellationToken ct)
    {
        _tcp = new TcpClient { NoDelay = true, ReceiveBufferSize = 1 << 20 };
        using (var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            timeout.CancelAfter(TimeSpan.FromSeconds(3));
            await _tcp.ConnectAsync("127.0.0.1", port, timeout.Token);
        }
        _stream = _tcp.GetStream();
        _closedReason = null;
        Send(new { op = "hello", token, client = "ServerMonitor" });
        _cts = new CancellationTokenSource();
        _ = Task.Run(() => ReadLoop(_cts.Token));
    }

    public void Send(object message)
    {
        var stream = _stream;
        if (stream == null)
            return;
        var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(message, WriteOptions) + "\n");
        lock (_sendLock)
        {
            try { stream.Write(bytes, 0, bytes.Length); }
            catch (Exception e) { _closedReason ??= e.Message; }
        }
    }

    async Task ReadLoop(CancellationToken ct)
    {
        var buffer = new byte[1 << 16];
        var line = new MemoryStream();
        try
        {
            while (!ct.IsCancellationRequested)
            {
                int n = await _stream!.ReadAsync(buffer, ct);
                if (n <= 0)
                {
                    _closedReason ??= "서버가 연결을 닫았다";
                    return;
                }
                Interlocked.Add(ref BytesReceived, n);
                int start = 0;
                for (int i = 0; i < n; i++)
                {
                    if (buffer[i] != (byte)'\n')
                        continue;
                    line.Write(buffer, start, i - start);
                    start = i + 1;
                    if (line.Length > 0)
                    {
                        try { Inbox.Enqueue(JsonDocument.Parse(line.ToArray())); }
                        catch (JsonException) { /* 깨진 줄은 버린다 */ }
                    }
                    line.SetLength(0);
                }
                line.Write(buffer, start, n - start);
            }
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or OperationCanceledException or SocketException)
        {
            _closedReason ??= e is OperationCanceledException ? "끊음" : e.Message;
        }
    }

    public void Dispose()
    {
        _closedReason ??= "끊음";
        _cts?.Cancel();
        try { _tcp?.Close(); } catch (Exception) { }
        _tcp = null;
        _stream = null;
    }

    /// <summary>정보 파일 (Logs/admin.json - 서버가 부팅 때 쓴다).</summary>
    public sealed record Info(int Port, string Token, int Pid, int GamePort, string Boot, string DataDir, string EventsFile);

    public static Info ReadInfo(string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(path, Encoding.UTF8));
        var r = doc.RootElement;
        string S(string k) => r.TryGetProperty(k, out var v) ? v.GetString() ?? "" : "";
        int I(string k) => r.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetInt32() : 0;
        return new Info(I("port"), S("token"), I("pid"), I("game_port"), S("boot"), S("dataDir"), S("eventsFile"));
    }

    /// <summary>exe 위로 올라가며 저장소의 Server\Logs\admin.json 을 찾는다 (Tools\ServerMonitor\bin\... 에서 실행해도).</summary>
    public static string? FindDefaultInfoFile()
    {
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "Server", "Logs", "admin.json");
            if (File.Exists(candidate))
                return candidate;
            if (Directory.Exists(Path.Combine(dir.FullName, "Server", "Data")))
                return candidate;       // 저장소 루트 - 아직 서버가 안 떴다
        }
        return null;
    }

    public static string? FindDataDir(string? hint)
    {
        if (!string.IsNullOrEmpty(hint) && Directory.Exists(hint))
            return hint;
        for (var dir = new DirectoryInfo(AppContext.BaseDirectory); dir != null; dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "Server", "Data");
            if (Directory.Exists(candidate))
                return candidate;
        }
        return null;
    }
}
