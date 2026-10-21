using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Text.Json;
using System.Windows.Media;

namespace ServerMonitor;

/// <summary>태그 13 종 - Server/Data/event_tags.csv 의 tag 열과 같다 (PROTOCOL.md).</summary>
public static class Tags
{
    public sealed record Info(string Key, string Label, Color Color);

    public static readonly Info[] All =
    {
        new("connect", "접속", Color.FromRgb(0x4F, 0xC3, 0xF7)),
        new("move", "이동", Color.FromRgb(0x90, 0xA4, 0xAE)),
        new("combat", "전투", Color.FromRgb(0xEF, 0x53, 0x50)),
        new("status", "상태이상", Color.FromRgb(0xBA, 0x68, 0xC8)),
        new("item", "아이템", Color.FromRgb(0xFF, 0xB7, 0x4D)),
        new("storage", "창고", Color.FromRgb(0xA1, 0x88, 0x7F)),
        new("economy", "경제", Color.FromRgb(0xFF, 0xD5, 0x4F)),
        new("party", "파티", Color.FromRgb(0x81, 0xC7, 0x84)),
        new("escape", "탈출", Color.FromRgb(0x4D, 0xD0, 0xE1)),
        new("cheat", "안티치트", Color.FromRgb(0xFF, 0x70, 0x43)),
        new("monster", "몬스터", Color.FromRgb(0xAE, 0xD5, 0x81)),
        new("db", "DB 커밋", Color.FromRgb(0x79, 0x86, 0xCB)),
        new("system", "시스템", Color.FromRgb(0xBD, 0xBD, 0xBD)),
    };

    static readonly Dictionary<string, Info> ByKey = All.ToDictionary(t => t.Key);

    public static Info Of(string key) => ByKey.TryGetValue(key, out var info) ? info : All[^1];
}

/// <summary>이벤트 한 줄 (관리 포트 "ev" · events-*.jsonl 한 줄).</summary>
public sealed class EventRecord
{
    public long Seq { get; init; }
    public string Ts { get; init; } = "";
    public string Time => Ts.Length >= 23 ? Ts.Substring(11, 12) : Ts;
    public char Level { get; init; } = 'I';
    public string Tag { get; init; } = "system";
    public string Sub { get; init; } = "";
    public long[] Actors { get; init; } = Array.Empty<long>();
    public long[] Targets { get; init; } = Array.Empty<long>();
    public long Account { get; init; }
    public string Room { get; init; } = "";
    public string Src { get; init; } = "log";
    public string Msg { get; init; } = "";
    public string Data { get; init; } = "";

    // 표시용 - 이름은 MainWindow 가 접속자 목록에서 채운다
    public string TagLabel => Tags.Of(Tag).Label;
    public Brush TagBrush => TagBrushes.Of(Tag);
    public string ActorText { get; set; } = "";
    public string TargetText { get; set; } = "";
    public bool IsError => Level == 'E';

    public bool Mentions(long id) => Array.IndexOf(Actors, id) >= 0 || Array.IndexOf(Targets, id) >= 0;

    public static EventRecord? Parse(JsonElement e)
    {
        try
        {
            return new EventRecord
            {
                Seq = e.TryGetProperty("seq", out var s) ? s.GetInt64() : 0,
                Ts = e.TryGetProperty("ts", out var ts) ? ts.GetString() ?? "" : "",
                Level = e.TryGetProperty("lv", out var lv) && lv.GetString() is { Length: > 0 } l ? l[0] : 'I',
                Tag = e.TryGetProperty("tag", out var tag) ? tag.GetString() ?? "system" : "system",
                Sub = e.TryGetProperty("sub", out var sub) ? sub.GetString() ?? "" : "",
                Actors = Ids(e, "a"),
                Targets = Ids(e, "tg"),
                Account = e.TryGetProperty("acc", out var acc) && acc.ValueKind == JsonValueKind.Number ? acc.GetInt64() : 0,
                Room = e.TryGetProperty("room", out var room) ? room.GetString() ?? "" : "",
                Src = e.TryGetProperty("src", out var src) ? src.GetString() ?? "log" : "log",
                Msg = e.TryGetProperty("msg", out var msg) ? msg.GetString() ?? "" : "",
                Data = e.TryGetProperty("d", out var d) ? d.GetRawText() : "",
            };
        }
        catch (Exception)
        {
            return null;
        }
    }

    static long[] Ids(JsonElement e, string key)
    {
        if (!e.TryGetProperty(key, out var arr) || arr.ValueKind != JsonValueKind.Array)
            return Array.Empty<long>();
        var ids = new long[arr.GetArrayLength()];
        int i = 0;
        foreach (var v in arr.EnumerateArray())
            ids[i++] = v.ValueKind == JsonValueKind.Number ? (long)v.GetUInt64() : 0;
        return ids;
    }
}

public static class TagBrushes
{
    static readonly Dictionary<string, SolidColorBrush> Cache = Tags.All.ToDictionary(t => t.Key, t =>
    {
        var b = new SolidColorBrush(t.Color);
        b.Freeze();
        return b;
    });

    public static Brush Of(string key) => Cache.TryGetValue(key, out var b) ? b : Brushes.Gray;
}

public abstract class Observable : INotifyPropertyChanged
{
    public event PropertyChangedEventHandler? PropertyChanged;

    protected void Set<T>(ref T field, T value, [CallerMemberName] string? name = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
            return;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
    }

    protected void Raise(string name) => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(name));
}

/// <summary>접속자 목록 한 줄.</summary>
public sealed class PlayerRow : Observable
{
    long _id; string _name = ""; long _account; string _room = ""; string _state = ""; int _hp; int _maxHp; bool _bot; long _party;

    public long Id { get => _id; set => Set(ref _id, value); }
    public string Name { get => _name; set => Set(ref _name, value); }
    public long Account { get => _account; set => Set(ref _account, value); }
    public string Room { get => _room; set => Set(ref _room, value); }
    public string State { get => _state; set { Set(ref _state, value); Raise(nameof(StateText)); } }
    public int Hp { get => _hp; set { Set(ref _hp, value); Raise(nameof(HpText)); } }
    public int MaxHp { get => _maxHp; set { Set(ref _maxHp, value); Raise(nameof(HpText)); } }
    public bool Bot { get => _bot; set { Set(ref _bot, value); Raise(nameof(Kind)); } }
    public long Party { get => _party; set => Set(ref _party, value); }
    public string HpText => MaxHp > 0 ? $"{Hp}/{MaxHp}" : "-";
    public string Kind => Bot ? "봇" : "사람";
    public string StateText => State switch { "room" => "던전", "camp" => "베이스캠프", "lobby" => "로비", "connecting" => "접속 중", "offline" => "(로그)", _ => State };
}

public sealed record RoomRow(string Tag, string Kind, int Floor, int Players, int Monsters, int Bags)
{
    public string KindText => Kind switch { "pvp" => "PvP", "pve_solo" => "PVE 솔로", "pve_coop" => "PVE 협동", "camp" => "베이스캠프", _ => Kind };
    public override string ToString() => $"{Tag}  {KindText} {Floor}층  · 플레이어 {Players} · 몬스터 {Monsters}";
}
