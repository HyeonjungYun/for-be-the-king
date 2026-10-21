using System.Collections.ObjectModel;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Threading;
using Microsoft.Win32;

namespace ServerMonitor;

public partial class MainWindow : Window
{
    const int MaxStored = 300_000;      // 메모리에 두는 이벤트 (넘으면 오래된 것부터)
    const int MaxShown = 100_000;       // 목록에 보이는 이벤트

    AdminClient? _client;
    bool _offline;
    string? _dataDir;
    readonly List<EventRecord> _all = new();
    readonly ObservableCollection<EventRecord> _shown = new();
    readonly ObservableCollection<PlayerRow> _players = new();
    readonly List<PlayerRow> _allPlayers = new();
    readonly Dictionary<long, string> _names = new();
    readonly Dictionary<string, ToggleButton> _tagButtons = new();
    readonly Dictionary<string, int> _tagCounts = new();
    readonly HashSet<string> _enabledTags = Tags.All.Select(t => t.Key).ToHashSet();
    readonly List<RoomRow> _rooms = new();
    long _selectedId;
    long _selectedAccount;
    string _mapRoom = "";
    long _lastSeq;
    readonly DispatcherTimer _pump = new() { Interval = TimeSpan.FromMilliseconds(100) };
    readonly DispatcherTimer _refresh = new() { Interval = TimeSpan.FromSeconds(1) };
    int _tick;
    static readonly Regex LoginName = new(@"\[LOGIN\] ok account=(\d+) char=(\d+) name=(\S+)", RegexOptions.Compiled);

    public MainWindow()
    {
        InitializeComponent();
        EventsList.ItemsSource = _shown;
        PlayersList.ItemsSource = _players;
        foreach (var t in Tags.All)
        {
            var b = new ToggleButton
            {
                IsChecked = true,
                Margin = new Thickness(2),
                Padding = new Thickness(8, 2, 8, 2),
                Style = (Style)FindResource("TagToggle"),
                Foreground = TagBrushes.Of(t.Key),
                BorderBrush = TagBrushes.Of(t.Key),
                Content = t.Label,
                Tag = t.Key,
            };
            b.Click += (_, _) =>
            {
                if (b.IsChecked == true) _enabledTags.Add(t.Key); else _enabledTags.Remove(t.Key);
                Rebuild();
            };
            _tagButtons[t.Key] = b;
            TagPanel.Children.Add(b);
        }
        Map.ObjectClicked += OnMapClicked;
        _pump.Tick += (_, _) => Pump();
        _refresh.Tick += (_, _) => Refresh();
        InfoPathBox.Text = AdminClient.FindDefaultInfoFile() ?? "";
        Closed += (_, _) => _client?.Dispose();
        Loaded += async (_, _) => await RunCommandLine();
    }

    // ───────────── 명령줄 (자동 확인 · 증거 화면) ─────────────
    //   --info <admin.json>  --port N  --connect  --open <events.jsonl>  --tab log|map|metrics  --select <이름|id>  --tag combat,item  --shot <png> [--shot-after 초]
    //   --shot 은 창을 그대로 PNG 로 남기고 끝낸다 (RenderTargetBitmap - 포그라운드가 없어도 된다)
    async Task RunCommandLine()
    {
        var args = Environment.GetCommandLineArgs().Skip(1).ToList();
        string? Arg(string name) { int i = args.IndexOf(name); return i >= 0 && i + 1 < args.Count ? args[i + 1] : null; }
        if (Arg("--info") is { } info) InfoPathBox.Text = info;
        if (Arg("--port") is { } port) PortBox.Text = port;
        if (Arg("--open") is { } file && File.Exists(file)) OpenLogFile(file);
        if (args.Contains("--connect"))
        {
            OnConnect(this, new RoutedEventArgs());
            for (int i = 0; i < 50 && (_client == null || _rooms.Count == 0 || _allPlayers.Count == 0); i++)
                await Task.Delay(200);
        }
        if (Arg("--tag") is { } tags)
        {
            var keep = tags.Split(',').ToHashSet();
            foreach (var (k, b) in _tagButtons) { b.IsChecked = keep.Contains(k); if (keep.Contains(k)) _enabledTags.Add(k); else _enabledTags.Remove(k); }
            Rebuild();
        }
        if (Arg("--select") is { } who)
        {
            // 봇은 차례로 들어온다 - 이름이 목록에 뜰 때까지 기다린 뒤에만 아무나로 물러선다
            bool Named(PlayerRow p) => p.Name == who || p.Id.ToString() == who;
            for (int i = 0; i < 50 && _client != null && !_allPlayers.Any(Named); i++)
                await Task.Delay(200);
            var row = _allPlayers.FirstOrDefault(Named) ?? _allPlayers.FirstOrDefault(p => p.Room.Length > 0);
            if (row != null)
            {
                PlayersList.SelectedItem = _players.FirstOrDefault(p => p.Id == row.Id);
                Select(row.Id);
                OnlySelectedBox.IsChecked = Arg("--tab") == "log";
                Rebuild();
            }
        }
        if (Arg("--tab") is { } tab)
        {
            Tabs.SelectedIndex = tab switch { "map" => 1, "metrics" => 2, _ => 0 };
            if (tab == "map" && _selectedId != 0)
            {
                OnShowOnMap(this, new RoutedEventArgs());
                FollowBox.IsChecked = true;
                Map.Follow = true;
            }
        }
        if (Arg("--shot") is { } shot)
        {
            double after = double.TryParse(Arg("--shot-after"), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var s) ? s : 3;
            await Task.Delay(TimeSpan.FromSeconds(after));
            SaveShot(shot);
            Close();
        }
    }

    void SaveShot(string path)
    {
        var content = (FrameworkElement)Content;
        var dpi = VisualTreeHelper.GetDpi(this);
        var bmp = new System.Windows.Media.Imaging.RenderTargetBitmap((int)(content.ActualWidth * dpi.DpiScaleX), (int)(content.ActualHeight * dpi.DpiScaleY),
            dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        // 창 배경은 Content 밖이라 그대로 찍으면 투명 - 바탕을 먼저 칠한다
        var visual = new DrawingVisual();
        using (var dc = visual.RenderOpen())
        {
            dc.DrawRectangle(Background, null, new Rect(0, 0, content.ActualWidth, content.ActualHeight));
            dc.DrawRectangle(new VisualBrush(content), null, new Rect(0, 0, content.ActualWidth, content.ActualHeight));
        }
        bmp.Render(visual);
        var png = new System.Windows.Media.Imaging.PngBitmapEncoder();
        png.Frames.Add(System.Windows.Media.Imaging.BitmapFrame.Create(bmp));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        using var fs = File.Create(path);
        png.Save(fs);
    }

    // ───────────── 연결 ─────────────

    void OnBrowseInfo(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "admin.json|*.json|모든 파일|*.*", FileName = InfoPathBox.Text };
        if (dlg.ShowDialog(this) == true)
            InfoPathBox.Text = dlg.FileName;
    }

    async void OnConnect(object sender, RoutedEventArgs e)
    {
        if (_client != null)
        {
            Disconnect("끊음");
            return;
        }
        int port = 0;
        string token = TokenBox.Password;
        string dataHint = "";
        try
        {
            if (File.Exists(InfoPathBox.Text))
            {
                var info = AdminClient.ReadInfo(InfoPathBox.Text);
                port = info.Port;
                if (token.Length == 0) token = info.Token;
                dataHint = info.DataDir;
            }
            if (int.TryParse(PortBox.Text, out var manual) && manual > 0)
                port = manual;
            if (port == 0 || token.Length == 0)
                throw new InvalidOperationException("포트 · 토큰을 모른다 - 서버가 떠 있는지(정보 파일이 있는지), 또는 포트 · 토큰을 직접 넣을 것");

            ResetSession(offline: false);
            _dataDir = AdminClient.FindDataDir(dataHint);
            _client = new AdminClient();
            StatusDot.Fill = Brushes.Gold;
            ConnectButton.IsEnabled = false;
            await _client.ConnectAsync(port, token, CancellationToken.None);
            ConnectButton.Content = "끊기";
            StatusText.Text = $"127.0.0.1:{port} 에 연결 - 인증 중";
            _pump.Start();
            _refresh.Start();
        }
        catch (Exception ex)
        {
            _client?.Dispose();
            _client = null;
            StatusDot.Fill = Brushes.IndianRed;
            StatusText.Text = "연결 실패 - " + ex.Message;
        }
        finally
        {
            ConnectButton.IsEnabled = true;
        }
    }

    void Disconnect(string why)
    {
        _client?.Dispose();
        _client = null;
        _pump.Stop();
        _refresh.Stop();
        ConnectButton.Content = "연결";
        StatusDot.Fill = Brushes.Gray;
        StatusText.Text = $"연결 끊김 - {why}  (이벤트 {_all.Count:N0} 개는 남아 있다)";
        Map.Clear("연결이 끊겼다");
    }

    void ResetSession(bool offline)
    {
        _offline = offline;
        _all.Clear();
        _shown.Clear();
        _tagCounts.Clear();
        _players.Clear();
        _allPlayers.Clear();
        _names.Clear();
        _rooms.Clear();
        RoomCombo.Items.Clear();
        _selectedId = 0;
        _selectedAccount = 0;
        _mapRoom = "";
        _lastSeq = 0;
        Map.Clear(offline ? "지난 로그에는 맵 장면이 없다 - 맵 · 지표는 실시간 연결에서만" : "방을 고르면 맵이 나온다");
        foreach (var s in new[] { SparkSessions, SparkP95, SparkP99, SparkMax, SparkMem, SparkSent, SparkRecv, SparkDb })
            s.Reset();
        SparkP95.Limit = 16.6; SparkP99.Limit = 33; SparkMax.Limit = 100; SparkMem.Limit = 512;
        UpdateTagCounts();
    }

    // ───────────── 받은 메시지 ─────────────

    void Pump()
    {
        if (_client == null)
            return;
        var newEvents = new List<EventRecord>();
        for (int i = 0; i < 20_000 && _client.Inbox.TryDequeue(out var doc); i++)
        {
            using (doc)
            {
                var root = doc.RootElement;
                switch (root.TryGetProperty("t", out var t) ? t.GetString() : "")
                {
                    case "ev":
                        if (EventRecord.Parse(root) is { } ev && ev.Seq > _lastSeq)
                        {
                            _lastSeq = ev.Seq;
                            newEvents.Add(ev);
                        }
                        break;
                    case "hello": OnHello(root); break;
                    case "players": OnPlayers(root); break;
                    case "player": OnPlayerDetail(root); break;
                    case "rooms": OnRooms(root); break;
                    case "map": OnMap(root); break;
                    case "metrics": OnMetrics(root); break;
                    case "result": OnResult(root); break;
                    case "error":
                        StatusText.Text = "서버 오류 - " + (root.TryGetProperty("msg", out var m) ? m.GetString() : "?");
                        break;
                }
            }
        }
        if (newEvents.Count > 0)
            AddEvents(newEvents);
        if (_client != null && !_client.Connected && _client.ClosedReason != null)
            Disconnect(_client.ClosedReason);
    }

    void OnHello(JsonElement root)
    {
        StatusDot.Fill = Brushes.LimeGreen;
        string S(string k) => root.TryGetProperty(k, out var v) ? v.ToString() : "";
        ServerInfoText.Text = $"GameServer pid {S("pid")} · 게임 포트 {S("port")} · 부팅 {S("boot")}";
        _dataDir = AdminClient.FindDataDir(S("dataDir"));
        StatusText.Text = "인증됨 - 최근 이벤트를 받는 중";
        _client!.Send(new { op = "events", backlog = 20000 });
        _client.Send(new { op = "players" });
        _client.Send(new { op = "rooms" });
        _client.Send(new { op = "metrics" });
    }

    void Refresh()
    {
        if (_client == null || !_client.Connected)
            return;
        _tick++;
        if (_tick % 2 == 0)
            _client.Send(new { op = "players" });
        if (_tick % 3 == 0)
            _client.Send(new { op = "rooms" });
        if (_selectedId != 0)
            _client.Send(new { op = "player", id = _selectedId });
        StatusText.Text = $"연결됨 · 이벤트 {_all.Count:N0} (보이는 것 {_shown.Count:N0}) · 접속자 {_allPlayers.Count} · 받은 양 {Interlocked.Read(ref _client.BytesReceived) / 1024.0 / 1024.0:0.0} MB";
    }

    // ───────────── 이벤트 목록 · 필터 ─────────────

    void AddEvents(List<EventRecord> events)
    {
        foreach (var e in events)
        {
            Decorate(e);
            _all.Add(e);
            _tagCounts[e.Tag] = _tagCounts.GetValueOrDefault(e.Tag) + 1;
            if (e.Tag == "combat" && e.Sub == "died")
                foreach (var v in e.Targets) Map.MarkDeath(v);
            if (_offline && LoginName.Match(e.Msg) is { Success: true } m)
                _names[long.Parse(m.Groups[2].Value)] = m.Groups[3].Value;
        }
        if (_all.Count > MaxStored)
            _all.RemoveRange(0, _all.Count - MaxStored);

        foreach (var e in events)
            if (Matches(e))
                _shown.Add(e);
        while (_shown.Count > MaxShown)
            _shown.RemoveAt(0);
        UpdateTagCounts();
        if (AutoScrollBox.IsChecked == true && _shown.Count > 0)
            EventsList.ScrollIntoView(_shown[^1]);
    }

    void Decorate(EventRecord e)
    {
        e.ActorText = string.Join(", ", e.Actors.Select(NameOf));
        e.TargetText = string.Join(", ", e.Targets.Select(NameOf));
    }

    string NameOf(long id) => _names.TryGetValue(id, out var n) && n.Length > 0 ? n : id >= 4_000_000_000 && id < 5_000_000_000 ? $"몬스터 {id - 4_000_000_000}" : id.ToString();

    bool Matches(EventRecord e)
    {
        if (!_enabledTags.Contains(e.Tag))
            return false;
        if (ErrorsOnlyBox.IsChecked == true && !e.IsError)
            return false;
        if (OnlySelectedBox.IsChecked == true && _selectedId != 0)
        {
            bool actorOnly = ActorOnlyRadio.IsChecked == true;
            bool hit = actorOnly ? Array.IndexOf(e.Actors, _selectedId) >= 0 : e.Mentions(_selectedId);
            if (!hit && !(_selectedAccount != 0 && e.Account == _selectedAccount))
                return false;
        }
        var sub = SubBox.Text.Trim();
        if (sub.Length > 0 && e.Sub.IndexOf(sub, StringComparison.OrdinalIgnoreCase) < 0)
            return false;
        var q = SearchBox.Text.Trim();
        if (q.Length > 0 && e.Msg.IndexOf(q, StringComparison.OrdinalIgnoreCase) < 0 && e.Room.IndexOf(q, StringComparison.OrdinalIgnoreCase) < 0
            && e.ActorText.IndexOf(q, StringComparison.OrdinalIgnoreCase) < 0 && e.TargetText.IndexOf(q, StringComparison.OrdinalIgnoreCase) < 0)
            return false;
        return true;
    }

    void Rebuild()
    {
        var matched = new List<EventRecord>();
        for (int i = _all.Count - 1; i >= 0 && matched.Count < MaxShown; i--)
        {
            var e = _all[i];
            Decorate(e);
            if (Matches(e))
                matched.Add(e);
        }
        matched.Reverse();
        _shown.Clear();
        foreach (var e in matched)
            _shown.Add(e);
        if (_shown.Count > 0)
            EventsList.ScrollIntoView(_shown[^1]);
    }

    void UpdateTagCounts()
    {
        foreach (var t in Tags.All)
            _tagButtons[t.Key].Content = $"{t.Label} {_tagCounts.GetValueOrDefault(t.Key):N0}";
    }

    void OnFilterChanged(object sender, RoutedEventArgs e) => Rebuild();

    void OnTagsAll(object sender, RoutedEventArgs e)
    {
        foreach (var (k, b) in _tagButtons) { b.IsChecked = true; _enabledTags.Add(k); }
        Rebuild();
    }

    void OnTagsNone(object sender, RoutedEventArgs e)
    {
        foreach (var (k, b) in _tagButtons) { b.IsChecked = false; _enabledTags.Remove(k); }
        Rebuild();
    }

    void OnClearLog(object sender, RoutedEventArgs e)
    {
        _all.Clear();
        _shown.Clear();
        _tagCounts.Clear();
        UpdateTagCounts();
    }

    void OnEventSelected(object sender, SelectionChangedEventArgs e)
    {
        if (EventsList.SelectedItem is not EventRecord ev)
            return;
        EventDetailBox.Text = $"#{ev.Seq}  {ev.Ts}  {ev.Level}  [{ev.TagLabel} / {ev.Sub}]  src={ev.Src}  room={ev.Room}  행위자={ev.ActorText}  대상={ev.TargetText}  계정={ev.Account}\n{ev.Msg}"
            + (ev.Data.Length > 0 ? "\n" + ev.Data : "");
    }

    // ───────────── 접속자 · 상세 ─────────────

    void OnPlayers(JsonElement root)
    {
        if (!root.TryGetProperty("list", out var list))
            return;
        var seen = new HashSet<long>();
        foreach (var p in list.EnumerateArray())
        {
            long id = p.GetProperty("id").GetInt64();
            if (id == 0 || !seen.Add(id))
                continue;
            var row = _allPlayers.FirstOrDefault(r => r.Id == id);
            if (row == null)
            {
                row = new PlayerRow { Id = id };
                _allPlayers.Add(row);
            }
            row.Name = p.GetProperty("name").GetString() ?? "";
            row.Account = p.GetProperty("acc").GetInt64();
            row.Room = p.GetProperty("room").GetString() ?? "";
            row.State = p.GetProperty("state").GetString() ?? "";
            row.Hp = p.GetProperty("hp").GetInt32();
            row.MaxHp = p.GetProperty("mhp").GetInt32();
            row.Bot = p.GetProperty("bot").GetBoolean();
            row.Party = p.GetProperty("party").GetInt64();
            _names[id] = row.Name;
        }
        _allPlayers.RemoveAll(r => !seen.Contains(r.Id));
        ApplyPlayersFilter();
        PlayersTitle.Text = $"접속자 {_allPlayers.Count} (사람 {_allPlayers.Count(p => !p.Bot)} · 봇 {_allPlayers.Count(p => p.Bot)})";

        // 따라가기 - 선택한 플레이어가 다른 방으로 갔다
        if (FollowBox.IsChecked == true && _allPlayers.FirstOrDefault(p => p.Id == _selectedId) is { Room.Length: > 0 } sel && sel.Room != _mapRoom)
            SelectRoom(sel.Room);
    }

    void ApplyPlayersFilter()
    {
        var q = PlayerSearchBox.Text.Trim();
        bool hideBots = HideBotsBox.IsChecked == true;
        var wanted = _allPlayers.Where(p => (!hideBots || !p.Bot) &&
            (q.Length == 0 || p.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || p.Id.ToString().Contains(q) || p.Room.Contains(q)))
            .OrderBy(p => p.Bot).ThenBy(p => p.Name, StringComparer.Ordinal).ToList();
        if (wanted.SequenceEqual(_players))
            return;
        var selected = _selectedId;
        _players.Clear();
        foreach (var p in wanted)
            _players.Add(p);
        if (selected != 0 && _players.FirstOrDefault(p => p.Id == selected) is { } keep)
            PlayersList.SelectedItem = keep;
    }

    void OnPlayersFilter(object sender, RoutedEventArgs e) => ApplyPlayersFilter();

    void OnPlayerSelected(object sender, SelectionChangedEventArgs e)
    {
        if (PlayersList.SelectedItem is PlayerRow row && row.Id != _selectedId)
            Select(row.Id);
    }

    void Select(long id)
    {
        _selectedId = id;
        _selectedAccount = _allPlayers.FirstOrDefault(p => p.Id == id)?.Account ?? 0;
        Map.SelectedId = id;
        DetailTitle.Text = $"{NameOf(id)} ({id})";
        DetailText.Text = _offline ? "(지난 로그 - 상세는 실시간 연결에서만)" : "불러오는 중…";
        if (_client != null)
            _client.Send(new { op = "player", id });
        if (OnlySelectedBox.IsChecked == true)
            Rebuild();
        Map.InvalidateVisual();
    }

    void OnPlayerDetail(JsonElement root)
    {
        if (!root.TryGetProperty("p", out var p) || p.ValueKind != JsonValueKind.Object)
        {
            if (_selectedId != 0)
                DetailText.Text = "방에 몸이 없다 (로비 · 접속 중 · 이미 나감)";
            return;
        }
        long id = p.GetProperty("id").GetInt64();
        if (id != _selectedId)
            return;
        string S(string k) => p.TryGetProperty(k, out var v) ? v.ToString() : "";
        _selectedAccount = p.GetProperty("acc").GetInt64();
        var sb = new StringBuilder();
        sb.AppendLine($"계정 {S("acc")} · 방 {S("room")} ({S("kind")})");
        sb.AppendLine($"위치 ({S("x")}, {S("y")}) · 방향 {S("yaw")}°");
        sb.AppendLine($"체력 {S("hp")}/{S("mhp")} {(S("alive") == "False" || S("alive") == "false" ? "· 사망" : "")}{(S("stunned") is "True" or "true" ? " · 기절" : "")}");
        sb.AppendLine($"런 골드 {S("run_gold")} · 남은 시간 {(int.TryParse(S("run_left_ms"), out var ms) ? ms / 1000 : 0)} s");
        sb.AppendLine($"파티 {S("party")} · 팀 {S("team")} · 시야 {S("vision")} cm");
        sb.AppendLine($"안티치트 점수 {double.Parse(S("cheat"), System.Globalization.CultureInfo.InvariantCulture):0.0} {S("cheat_top")}");
        sb.AppendLine("장비");
        foreach (var g in p.GetProperty("gear").EnumerateArray())
            sb.AppendLine($"  칸 {g.GetProperty("slot")} · 종류 {g.GetProperty("type")} · 등급 {g.GetProperty("grade")} · +{g.GetProperty("level")} · 스킬 {g.GetProperty("skill")}/{g.GetProperty("skill2")}{(g.GetProperty("lone").GetBoolean() ? " · 고독한 길" : "")}");
        sb.AppendLine("스킬 칸");
        foreach (var s in p.GetProperty("skills").EnumerateArray())
        {
            int bind = s.GetProperty("bind").GetInt32();
            sb.AppendLine($"  {SlotKey(s.GetProperty("slot").GetInt32())} {s.GetProperty("skill")} {(bind == 1 ? "" : bind == 2 ? "(가려짐)" : "(묶이지 않음)")}{(s.GetProperty("cd_ms").GetInt32() > 0 ? $" · 쿨 {s.GetProperty("cd_ms").GetInt32() / 1000.0:0.0}s" : "")}");
        }
        var inv = p.GetProperty("inv");
        sb.Append($"소지품 {inv.GetArrayLength()} 칸");
        DetailTitle.Text = $"{S("name")} ({id})";
        DetailText.Text = sb.ToString();
    }

    static string SlotKey(int slot) => slot switch { 1 => "Shift", 2 => "Q", 3 => "E", 4 => "R", 5 => "Z", 6 => "X", _ => slot.ToString() };

    void OnShowOnMap(object sender, RoutedEventArgs e)
    {
        if (_selectedId == 0)
            return;
        Tabs.SelectedIndex = 1;
        if (_allPlayers.FirstOrDefault(p => p.Id == _selectedId) is { Room.Length: > 0 } p && p.Room != _mapRoom)
            SelectRoom(p.Room);
        Map.CenterOn(_selectedId);
    }

    void OnShowPlayerLog(object sender, RoutedEventArgs e)
    {
        Tabs.SelectedIndex = 0;
        OnlySelectedBox.IsChecked = true;
        Rebuild();
    }

    // ───────────── 방 · 맵 ─────────────

    void OnRooms(JsonElement root)
    {
        if (!root.TryGetProperty("list", out var list))
            return;
        _rooms.Clear();
        foreach (var r in list.EnumerateArray())
            _rooms.Add(new RoomRow(r.GetProperty("tag").GetString() ?? "", r.GetProperty("kind").GetString() ?? "", r.GetProperty("floor").GetInt32(),
                r.GetProperty("players").GetInt32(), r.GetProperty("monsters").GetInt32(), r.GetProperty("bags").GetInt32()));
        _rooms.Sort((a, b) => string.CompareOrdinal(a.Tag[0] == 'w' ? "0" + a.Tag : a.Tag, b.Tag[0] == 'w' ? "0" + b.Tag : b.Tag));
        string current = _mapRoom;
        _suppressRoomChange = true;
        RoomCombo.Items.Clear();
        foreach (var r in _rooms)
            RoomCombo.Items.Add(r);
        RoomCombo.SelectedItem = _rooms.FirstOrDefault(r => r.Tag == current);
        _suppressRoomChange = false;
        if (current.Length == 0 && _rooms.Count > 0)
            SelectRoom(_rooms[0].Tag);
    }

    bool _suppressRoomChange;

    void OnRoomSelected(object sender, SelectionChangedEventArgs e)
    {
        if (_suppressRoomChange || RoomCombo.SelectedItem is not RoomRow r)
            return;
        SelectRoom(r.Tag);
    }

    void SelectRoom(string tag)
    {
        var room = _rooms.FirstOrDefault(r => r.Tag == tag);
        if (room == null || _client == null)
            return;
        bool floorChanged = _mapRoom.Length == 0 || _rooms.FirstOrDefault(r => r.Tag == _mapRoom)?.Floor != room.Floor;
        _mapRoom = tag;
        _suppressRoomChange = true;
        RoomCombo.SelectedItem = room;
        _suppressRoomChange = false;
        if (floorChanged)
            Map.SetGeometry(FloorGeometry.Load(_dataDir, room.Floor));
        Map.Clear("장면을 기다리는 중…");
        _client.Send(new { op = "map", room = tag });
    }

    void OnMap(JsonElement root)
    {
        if (root.TryGetProperty("gone", out _))
        {
            Map.Clear("방이 닫혔다 - 다른 방을 고를 것");
            _mapRoom = "";
            return;
        }
        if ((root.TryGetProperty("room", out var r) ? r.GetString() : "") != _mapRoom)
            return;
        var objs = new List<MapObject>();
        foreach (var o in root.GetProperty("objs").EnumerateArray())
            objs.Add(MapObject.From(o));
        List<(long, Point[])>? routes = null;
        if (root.TryGetProperty("routes", out var rs))
        {
            routes = new();
            foreach (var route in rs.EnumerateArray())
                routes.Add((route.GetProperty("pack").GetInt64(), route.GetProperty("pts").EnumerateArray().Select(p => new Point(p[0].GetDouble(), p[1].GetDouble())).ToArray()));
        }
        foreach (var o in objs.Where(o => o.K == "p" && o.Name.Length > 0))
            _names[o.Id] = o.Name;
        Map.SetScene(objs, routes);
    }

    void OnMapClicked(MapObject o)
    {
        if (o.K == "p")
        {
            Select(o.Id);
            if (_players.FirstOrDefault(p => p.Id == o.Id) is { } row)
                PlayersList.SelectedItem = row;
        }
        else
        {
            EventDetailBox.Text = o.K == "m"
                ? $"몬스터 {o.Id} ({o.Name}) · 체력 {o.Hp}/{o.MaxHp} · 상태 {o.State} · 대상 {NameOf(o.Target)} · 무리 {o.Pack}"
                : $"{(o.Chest ? "상자" : "가방")} {o.Id} · 아이템 {o.Items} · {(o.Open ? "열려 있음" : "닫힘")}{(o.Looted ? " · 털림" : "")} · 주인 {NameOf(o.Owner)}";
            Map.SelectedId = o.Id;
            Map.InvalidateVisual();
        }
    }

    void OnLayers(object sender, RoutedEventArgs e)
    {
        Map.ShowPlayers = LayerPlayers.IsChecked == true;
        Map.ShowMonsters = LayerMonsters.IsChecked == true;
        Map.ShowBags = LayerBags.IsChecked == true;
        Map.ShowTraces = LayerTraces.IsChecked == true;
        Map.ShowRoutes = LayerRoutes.IsChecked == true;
        Map.ShowLabels = LayerLabels.IsChecked == true;
        Map.ShowVision = LayerVision.IsChecked == true;
        Map.Follow = FollowBox.IsChecked == true;
        if (Map.Follow && _selectedId != 0)
            OnShowOnMap(sender, e);
        Map.InvalidateVisual();
    }

    void OnMapFit(object sender, RoutedEventArgs e) => Map.Fit();

    // ───────────── 지표 ─────────────

    void OnMetrics(JsonElement root)
    {
        if (!root.TryGetProperty("m", out var m) || m.GetProperty("seq").GetInt64() == 0)
        {
            MetricsText.Text = "아직 10 초 통계 전";
            return;
        }
        double D(string k) => m.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;
        SparkSessions.Add(D("sessions"));
        SparkP95.Add(D("flush_p95_us") / 1000);
        SparkP99.Add(D("flush_p99_us") / 1000);
        SparkMax.Add(D("flush_max_us") / 1000);
        SparkMem.Add(D("commit_mb"));
        SparkSent.Add(D("sent_mbps"));
        SparkRecv.Add(D("recv_mbps"));
        SparkDb.Add(D("db_pending") + D("db_delayed"));
        MetricsText.Text = $"#{D("seq"):0} · 세션 {D("sessions"):0} · 인스턴스 {D("instances"):0} · 플러시 {D("flush_n"):0} 회 · 이동 거부 {D("move_reject"):0} · "
            + $"클라당 {D("per_client_kbps"):0.0} kbps · 워킹셋 {D("workingset_mb"):0} MB · 이벤트 누계 {D("events"):N0}  (10 초마다 갱신, 점선 = 기준)";
    }

    // ───────────── 관리 명령 ─────────────

    void OnKick(object sender, RoutedEventArgs e)
    {
        if (_client == null || _selectedId == 0)
        {
            AdminResultText.Text = "연결된 상태에서 플레이어를 먼저 고를 것";
            return;
        }
        if (MessageBox.Show(this, $"{NameOf(_selectedId)} ({_selectedId}) 을(를) 강제 종료할까?\n사유: {KickReasonBox.Text}", "강제 종료", MessageBoxButton.YesNo, MessageBoxImage.Warning) != MessageBoxResult.Yes)
            return;
        _client.Send(new { op = "kick", id = _selectedId, reason = KickReasonBox.Text });
    }

    void OnUnban(object sender, RoutedEventArgs e)
    {
        if (_client == null)
        {
            AdminResultText.Text = "연결된 상태에서만";
            return;
        }
        var name = UnbanNameBox.Text.Trim();
        if (name.Length > 0)
            _client.Send(new { op = "unban", name });
        else if (_selectedAccount != 0)
            _client.Send(new { op = "unban", account = _selectedAccount });
        else
            AdminResultText.Text = "캐릭터 이름을 넣거나 플레이어를 고를 것 (정지된 계정은 접속자 목록에 없다 - 이름으로)";
    }

    void OnNotice(object sender, RoutedEventArgs e)
    {
        var text = NoticeBox.Text.Trim();
        if (_client == null || text.Length == 0)
            return;
        if (MessageBox.Show(this, $"접속한 모든 사람에게 보낸다:\n\n{text}", "전체 공지", MessageBoxButton.YesNo) != MessageBoxResult.Yes)
            return;
        _client.Send(new { op = "notice", text });
    }

    void OnResult(JsonElement root)
    {
        bool ok = root.GetProperty("ok").GetBoolean();
        AdminResultText.Foreground = ok ? Brushes.LightGreen : Brushes.IndianRed;
        AdminResultText.Text = $"{DateTime.Now:HH:mm:ss} {root.GetProperty("op").GetString()} - {root.GetProperty("msg").GetString()}";
    }

    // ───────────── 지난 로그 ─────────────

    void OnOpenLog(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFileDialog { Filter = "이벤트 로그 (*.jsonl)|*.jsonl|모든 파일|*.*" };
        var guess = _dataDir ?? AdminClient.FindDataDir(null);
        if (guess != null && Directory.Exists(Path.Combine(guess, "..", "Logs", "events")))
            dlg.InitialDirectory = Path.GetFullPath(Path.Combine(guess, "..", "Logs", "events"));
        if (dlg.ShowDialog(this) != true)
            return;
        OpenLogFile(dlg.FileName);
    }

    void OpenLogFile(string fileName)
    {
        var dlg = new { FileName = fileName };
        if (_client != null)
            Disconnect("지난 로그를 열었다");
        ResetSession(offline: true);
        var events = new List<EventRecord>();
        int bad = 0;
        // 도는 중인 서버의 파일도 열 수 있게 공유 읽기
        using (var fs = new FileStream(dlg.FileName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
        using (var reader = new StreamReader(fs, Encoding.UTF8))
        {
            string? line;
            while ((line = reader.ReadLine()) != null)
            {
                if (line.Length == 0)
                    continue;
                try
                {
                    using var doc = JsonDocument.Parse(line);
                    if (EventRecord.Parse(doc.RootElement) is { } ev)
                        events.Add(ev);
                }
                catch (JsonException) { bad++; }
            }
        }
        // 이름 · 접속자는 로그인 줄에서
        foreach (var ev in events)
            if (LoginName.Match(ev.Msg) is { Success: true } m)
            {
                long id = long.Parse(m.Groups[2].Value);
                _names[id] = m.Groups[3].Value;
                if (_allPlayers.All(p => p.Id != id))
                    _allPlayers.Add(new PlayerRow { Id = id, Account = long.Parse(m.Groups[1].Value), Name = m.Groups[3].Value, State = "offline", Bot = m.Groups[3].Value.StartsWith("bot_") });
            }
        AddEvents(events);
        Rebuild();
        ApplyPlayersFilter();
        PlayersTitle.Text = $"로그에 나온 플레이어 {_allPlayers.Count}";
        ServerInfoText.Text = Path.GetFileName(dlg.FileName);
        StatusDot.Fill = Brushes.SteelBlue;
        StatusText.Text = $"지난 로그 - {events.Count:N0} 이벤트 · 깨진 줄 {bad} · {dlg.FileName}";
    }
}
