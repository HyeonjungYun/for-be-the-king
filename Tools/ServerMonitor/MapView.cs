using System.Globalization;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;

namespace ServerMonitor;

/// <summary>
/// 맵 현황 - 지형(벽 · 해안 · 집 · 타워 · 진입 지점) 위에 관리 포트의 장면(초당 5 번)을 그린다.
/// 화면: +X(북) 위 · +Y(동) 오른쪽 (Tools/level/draw_map.py 와 같다). 휠 = 확대, 끌기 = 이동, 클릭 = 고르기.
/// </summary>
public sealed class MapView : FrameworkElement
{
    FloorGeometry? _geometry;
    StreamGeometry? _wallGeo, _coverGeo, _islandGeo;
    List<MapObject> _objects = new();
    List<(long Pack, Point[] Pts)> _routes = new();
    readonly List<(Point P, DateTime At, string Name)> _deaths = new();
    readonly Dictionary<long, MapObject> _last = new();

    double _scale = 0.02;           // px / cm
    Point _center = new(0, 0);      // 화면 가운데의 세계 좌표 (x, y)
    Point? _dragFrom; Point _dragCenter; bool _dragged;

    public long SelectedId { get; set; }
    public bool Follow { get; set; }
    public bool ShowVision { get; set; } = true;
    public bool ShowPlayers { get; set; } = true;
    public bool ShowMonsters { get; set; } = true;
    public bool ShowBags { get; set; } = true;
    public bool ShowTraces { get; set; } = true;
    public bool ShowRoutes { get; set; } = true;
    public bool ShowLabels { get; set; } = true;
    public string Message { get; set; } = "방을 고르면 맵이 나온다";

    public event Action<MapObject>? ObjectClicked;

    public MapView()
    {
        ClipToBounds = true;
        Focusable = true;
    }

    public void SetGeometry(FloorGeometry geometry)
    {
        _geometry = geometry;
        _wallGeo = BuildWalls(geometry, cover: false);
        _coverGeo = BuildWalls(geometry, cover: true);
        _islandGeo = null;
        if (geometry.Outline.Count >= 3)
        {
            _islandGeo = new StreamGeometry();
            using (var ctx = _islandGeo.Open())
            {
                ctx.BeginFigure(geometry.Outline[0], true, true);
                ctx.PolyLineTo(geometry.Outline.Skip(1).ToList(), false, false);
            }
            _islandGeo.Freeze();
        }
        _routes.Clear();
        Fit();
    }

    static StreamGeometry BuildWalls(FloorGeometry g, bool cover)
    {
        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            foreach (var w in g.Walls)
            {
                bool isCover = w.Kind is "cover" or "pillar" or "free_pillar";
                if (isCover != cover)
                    continue;
                ctx.BeginFigure(w.Corners[0], true, true);
                ctx.PolyLineTo(w.Corners.Skip(1).ToList(), false, false);
            }
        }
        geo.Freeze();
        return geo;
    }

    public void Fit()
    {
        if (_geometry == null || ActualWidth < 10 || ActualHeight < 10)
        {
            InvalidateVisual();
            return;
        }
        var b = _geometry.Bounds;
        _center = new Point(b.X + b.Width / 2, b.Y + b.Height / 2);
        // 화면 가로 = 세계 Y, 세로 = 세계 X
        _scale = Math.Min(ActualWidth / b.Height, ActualHeight / b.Width) * 0.95;
        InvalidateVisual();
    }

    public void SetScene(List<MapObject> objects, List<(long, Point[])>? routes)
    {
        // 사라진 생물 중 마지막에 체력이 0 이었거나 플레이어였던 것 - 사망 지점 표시용으로 마지막 자리를 기억한다
        var now = DateTime.Now;
        var ids = objects.Select(o => o.Id).ToHashSet();
        foreach (var (id, old) in _last)
            if (!ids.Contains(id) && (old.K == "p" || old.K == "m") && old.Hp <= old.MaxHp * 0.25)
                _deaths.Add((new Point(old.X, old.Y), now, old.Name));
        _last.Clear();
        foreach (var o in objects)
            if (o.K is "p" or "m")
                _last[o.Id] = o;
        _deaths.RemoveAll(d => (now - d.At).TotalSeconds > 10);

        _objects = objects;
        if (routes != null)
            _routes = routes;
        if (Follow && SelectedId != 0 && objects.FirstOrDefault(o => o.Id == SelectedId) is { } sel)
            _center = new Point(sel.X, sel.Y);
        InvalidateVisual();
    }

    public void MarkDeath(long victimId)
    {
        if (_last.TryGetValue(victimId, out var o))
        {
            _deaths.Add((new Point(o.X, o.Y), DateTime.Now, o.Name));
            InvalidateVisual();
        }
    }

    public void Clear(string message)
    {
        _objects = new();
        _routes = new();
        _deaths.Clear();
        _last.Clear();
        Message = message;
        InvalidateVisual();
    }

    public MapObject? Find(long id) => _objects.FirstOrDefault(o => o.Id == id);

    Point ToScreen(double x, double y) => new(ActualWidth / 2 + (y - _center.Y) * _scale, ActualHeight / 2 - (x - _center.X) * _scale);
    Point ToWorld(Point s) => new(_center.X - (s.Y - ActualHeight / 2) / _scale, _center.Y + (s.X - ActualWidth / 2) / _scale);

    Matrix WorldMatrix() => new(0, -_scale, _scale, 0, ActualWidth / 2 - _scale * _center.Y, ActualHeight / 2 + _scale * _center.X);

    static readonly Brush Sea = Frozen(new SolidColorBrush(Color.FromRgb(0x10, 0x1A, 0x24)));
    static readonly Brush Land = Frozen(new SolidColorBrush(Color.FromRgb(0x23, 0x2A, 0x22)));
    static readonly Brush WallBrush = Frozen(new SolidColorBrush(Color.FromRgb(0x9E, 0x9E, 0x9E)));
    static readonly Brush CoverBrush = Frozen(new SolidColorBrush(Color.FromRgb(0x8D, 0x6E, 0x63)));
    static readonly Pen HousePen = Frozen(new Pen(new SolidColorBrush(Color.FromArgb(0x80, 0xFF, 0xE0, 0x82)), 1) { DashStyle = DashStyles.Dash });
    static readonly Brush TextBrush = Frozen(new SolidColorBrush(Color.FromRgb(0xEE, 0xEE, 0xEE)));
    static readonly Brush DimText = Frozen(new SolidColorBrush(Color.FromRgb(0x9E, 0x9E, 0x9E)));
    static readonly Brush HpBack = Frozen(new SolidColorBrush(Color.FromArgb(0xC0, 0x30, 0x30, 0x30)));
    static readonly Brush HpGood = Frozen(new SolidColorBrush(Color.FromRgb(0x66, 0xBB, 0x6A)));
    static readonly Brush HpBad = Frozen(new SolidColorBrush(Color.FromRgb(0xEF, 0x53, 0x50)));
    static readonly Pen SelPen = Frozen(new Pen(Brushes.Yellow, 2));
    static readonly Pen VisionPen = Frozen(new Pen(new SolidColorBrush(Color.FromArgb(0xB0, 0xFF, 0xEB, 0x3B)), 1.5) { DashStyle = DashStyles.Dot });
    static readonly Brush VisionFill = Frozen(new SolidColorBrush(Color.FromArgb(0x18, 0xFF, 0xEB, 0x3B)));
    static readonly Brush ZoneFill = Frozen(new SolidColorBrush(Color.FromArgb(0x40, 0xBA, 0x68, 0xC8)));
    static readonly Pen ZonePen = Frozen(new Pen(new SolidColorBrush(Color.FromArgb(0xA0, 0xBA, 0x68, 0xC8)), 1));
    static readonly Pen ProjPen = Frozen(new Pen(new SolidColorBrush(Color.FromRgb(0xFF, 0xF1, 0x76)), 1.5));
    static readonly Pen TowerPen = Frozen(new Pen(new SolidColorBrush(Color.FromArgb(0xC0, 0x4D, 0xD0, 0xE1)), 1.5));
    static readonly Pen GaugePen = Frozen(new Pen(new SolidColorBrush(Color.FromRgb(0x4D, 0xD0, 0xE1)), 4));
    static readonly Pen DeathPen = Frozen(new Pen(new SolidColorBrush(Color.FromRgb(0xFF, 0x52, 0x52)), 2.5));
    static readonly Pen YawPen = Frozen(new Pen(new SolidColorBrush(Color.FromArgb(0xC0, 0xFF, 0xFF, 0xFF)), 1.5));
    static readonly Brush HumanBrush = Frozen(new SolidColorBrush(Color.FromRgb(0x29, 0xB6, 0xF6)));
    static readonly Brush BotBrush = Frozen(new SolidColorBrush(Color.FromRgb(0x90, 0xCA, 0xF9)));
    static readonly Brush ChestBrush = Frozen(new SolidColorBrush(Color.FromRgb(0xFF, 0xC1, 0x07)));
    static readonly Brush BagBrush = Frozen(new SolidColorBrush(Color.FromRgb(0xA1, 0x88, 0x7F)));
    static readonly Brush LootedBrush = Frozen(new SolidColorBrush(Color.FromRgb(0x5D, 0x40, 0x37)));
    static readonly Color[] PackColors = { Colors.OrangeRed, Colors.Gold, Colors.LimeGreen, Colors.DeepSkyBlue, Colors.Violet, Colors.Coral, Colors.Turquoise, Colors.Khaki, Colors.Salmon, Colors.PaleGreen };

    static T Frozen<T>(T f) where T : Freezable { f.Freeze(); return f; }

    static Brush MonsterBrush(string state) => state switch
    {
        "Chase" => Brushes.Red,
        "Search" => Brushes.Orange,
        "Return" => Brushes.Gold,
        "Dead" => Brushes.DimGray,
        _ => Brushes.YellowGreen,
    };

    protected override void OnRender(DrawingContext dc)
    {
        dc.DrawRectangle(Sea, null, new Rect(0, 0, ActualWidth, ActualHeight));
        double dip = VisualTreeHelper.GetDpi(this).PixelsPerDip;

        if (_geometry != null)
        {
            dc.PushTransform(new MatrixTransform(WorldMatrix()));
            if (_islandGeo != null)
                dc.DrawGeometry(Land, null, _islandGeo);
            if (_wallGeo != null)
                dc.DrawGeometry(WallBrush, null, _wallGeo);
            if (_coverGeo != null)
                dc.DrawGeometry(CoverBrush, null, _coverGeo);
            dc.Pop();

            foreach (var h in _geometry.Houses)
            {
                var a = ToScreen(h.X, h.Y);
                var b = ToScreen(h.X + h.Width, h.Y + h.Height);
                dc.DrawRectangle(null, HousePen, new Rect(a, b));
            }
            foreach (var (id, p) in _geometry.Entries)
            {
                var s = ToScreen(p.X, p.Y);
                dc.DrawRectangle(DimText, null, new Rect(s.X - 2, s.Y - 2, 4, 4));
            }
        }

        if (ShowRoutes)
        {
            foreach (var (pack, pts) in _routes)
            {
                if (pts.Length < 2)
                    continue;
                var c = PackColors[(int)(pack % PackColors.Length)];
                var pen = new Pen(new SolidColorBrush(Color.FromArgb(0x70, c.R, c.G, c.B)), 1.2) { DashStyle = DashStyles.Dash };
                for (int i = 1; i < pts.Length; i++)
                    dc.DrawLine(pen, ToScreen(pts[i - 1].X, pts[i - 1].Y), ToScreen(pts[i].X, pts[i].Y));
            }
        }

        // 타워 (게이지 호)
        foreach (var t in _objects.Where(o => o.K == "tower"))
        {
            var s = ToScreen(t.X, t.Y);
            double r = 667 * _scale;        // 구역 반경 (escape_config zone_enter_radius_cm)
            dc.DrawEllipse(null, TowerPen, s, r, r);
            if (t.GaugeMs > 0)
            {
                double frac = Math.Min(1.0, t.GaugeMs / 60000.0);
                DrawArc(dc, GaugePen, s, r + 3, frac);
            }
            if (ShowLabels)
                Text(dc, $"{t.Name} {(t.Filling ? $"채우는 중 {t.GaugeMs / 1000}s · {t.Occupants}명" : "")}", new Point(s.X + r + 4, s.Y - 8), DimText, 11, dip);
        }

        if (ShowTraces)
        {
            foreach (var z in _objects.Where(o => o.K == "zone"))
            {
                var s = ToScreen(z.X, z.Y);
                dc.DrawEllipse(ZoneFill, ZonePen, s, Math.Max(2, z.Radius * _scale), Math.Max(2, z.Radius * _scale));
            }
            foreach (var p in _objects.Where(o => o.K == "proj"))
            {
                var s = ToScreen(p.X, p.Y);
                var tail = ToScreen(p.X - p.Dx * 120, p.Y - p.Dy * 120);
                dc.DrawLine(ProjPen, tail, s);
                dc.DrawEllipse(Brushes.White, null, s, 2, 2);
            }
            var now = DateTime.Now;
            foreach (var d in _deaths)
            {
                var s = ToScreen(d.P.X, d.P.Y);
                double age = (now - d.At).TotalSeconds;
                var pen = new Pen(new SolidColorBrush(Color.FromArgb((byte)(255 * Math.Max(0.15, 1 - age / 10)), 0xFF, 0x52, 0x52)), 2.5);
                dc.DrawLine(pen, new Point(s.X - 6, s.Y - 6), new Point(s.X + 6, s.Y + 6));
                dc.DrawLine(pen, new Point(s.X - 6, s.Y + 6), new Point(s.X + 6, s.Y - 6));
            }
        }

        if (ShowBags)
        {
            foreach (var b in _objects.Where(o => o.K == "bag"))
            {
                var s = ToScreen(b.X, b.Y);
                var brush = b.Looted ? LootedBrush : (b.Chest ? ChestBrush : BagBrush);
                dc.DrawRectangle(brush, b.Open ? new Pen(Brushes.White, 1) : null, new Rect(s.X - 3.5, s.Y - 3.5, 7, 7));
            }
        }

        if (ShowMonsters)
        {
            foreach (var m in _objects.Where(o => o.K == "m"))
            {
                var s = ToScreen(m.X, m.Y);
                var geo = new StreamGeometry();
                using (var ctx = geo.Open())
                {
                    ctx.BeginFigure(new Point(s.X, s.Y - 5), true, true);
                    ctx.LineTo(new Point(s.X + 4.5, s.Y + 4), false, false);
                    ctx.LineTo(new Point(s.X - 4.5, s.Y + 4), false, false);
                }
                dc.DrawGeometry(MonsterBrush(m.State), m.Id == SelectedId ? SelPen : null, geo);
                if (m.MaxHp > 0 && m.Hp < m.MaxHp)
                    HpBar(dc, s, m.Hp, m.MaxHp, 12);
            }
        }

        if (ShowPlayers)
        {
            MapObject? selected = null;
            foreach (var p in _objects.Where(o => o.K is "p" or "decoy"))
            {
                var s = ToScreen(p.X, p.Y);
                if (p.Id == SelectedId)
                    selected = p;
                if (p.K == "decoy")
                {
                    dc.DrawEllipse(null, new Pen(BotBrush, 1), s, 4, 4);
                    continue;
                }
                var brush = p.Bot ? BotBrush : HumanBrush;
                Pen? ring = p.Party != 0 ? new Pen(new SolidColorBrush(PackColors[(int)(p.Party % PackColors.Length)]), 2) : null;
                if (p.Bot)
                    dc.DrawRectangle(brush, ring, new Rect(s.X - 4, s.Y - 4, 8, 8));
                else
                    dc.DrawEllipse(brush, ring, s, 5.5, 5.5);
                // 바라보는 쪽 (yaw - 도, +X 에서 +Y 쪽으로)
                double yr = p.Yaw * Math.PI / 180;
                dc.DrawLine(YawPen, s, new Point(s.X + Math.Sin(yr) * 10, s.Y - Math.Cos(yr) * 10));
                HpBar(dc, s, p.Hp, p.MaxHp, 16);
                if (ShowLabels && (_scale > 0.015 || p.Id == SelectedId || !p.Bot))
                    Text(dc, p.Name, new Point(s.X + 7, s.Y - 15), p.Id == SelectedId ? Brushes.Yellow : TextBrush, 11, dip);
            }
            if (selected != null)
            {
                var s = ToScreen(selected.X, selected.Y);
                if (ShowVision && selected.Vision > 0)
                {
                    double r = selected.Vision * _scale;
                    dc.DrawEllipse(VisionFill, VisionPen, s, r, r);
                }
                dc.DrawEllipse(null, SelPen, s, 10, 10);
            }
        }

        // 축척 · 안내
        double meters = 10;
        while (meters * 100 * _scale < 60) meters *= 2;
        double barPx = meters * 100 * _scale;
        var origin = new Point(12, ActualHeight - 16);
        dc.DrawLine(new Pen(TextBrush, 2), origin, new Point(origin.X + barPx, origin.Y));
        Text(dc, $"{meters:0} m", new Point(origin.X, origin.Y - 16), TextBrush, 11, dip);
        Text(dc, "↑ 북 (+X)", new Point(ActualWidth - 70, 8), DimText, 11, dip);
        if (_objects.Count == 0 && !string.IsNullOrEmpty(Message))
            Text(dc, Message, new Point(16, 12), DimText, 13, dip);
    }

    static void HpBar(DrawingContext dc, Point s, int hp, int max, double width)
    {
        if (max <= 0)
            return;
        double frac = Math.Clamp(hp / (double)max, 0, 1);
        var r = new Rect(s.X - width / 2, s.Y + 7, width, 3);
        dc.DrawRectangle(HpBack, null, r);
        dc.DrawRectangle(frac > 0.3 ? HpGood : HpBad, null, new Rect(r.X, r.Y, r.Width * frac, r.Height));
    }

    static void DrawArc(DrawingContext dc, Pen pen, Point c, double r, double frac)
    {
        if (frac >= 0.999)
        {
            dc.DrawEllipse(null, pen, c, r, r);
            return;
        }
        double a = frac * 2 * Math.PI;
        var start = new Point(c.X, c.Y - r);
        var end = new Point(c.X + r * Math.Sin(a), c.Y - r * Math.Cos(a));
        var geo = new StreamGeometry();
        using (var ctx = geo.Open())
        {
            ctx.BeginFigure(start, false, false);
            ctx.ArcTo(end, new Size(r, r), 0, frac > 0.5, SweepDirection.Clockwise, true, false);
        }
        dc.DrawGeometry(null, pen, geo);
    }

    static readonly Typeface Face = new("Malgun Gothic");

    static void Text(DrawingContext dc, string text, Point at, Brush brush, double size, double dip)
    {
        if (string.IsNullOrEmpty(text))
            return;
        var ft = new FormattedText(text, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, Face, size, brush, dip);
        dc.DrawText(ft, at);
    }

    protected override void OnRenderSizeChanged(SizeChangedInfo sizeInfo)
    {
        base.OnRenderSizeChanged(sizeInfo);
        if (sizeInfo.PreviousSize.Width < 10)
            Fit();
    }

    protected override void OnMouseWheel(MouseWheelEventArgs e)
    {
        var mouse = e.GetPosition(this);
        var before = ToWorld(mouse);
        _scale = Math.Clamp(_scale * (e.Delta > 0 ? 1.25 : 0.8), 0.002, 1.0);
        var after = ToWorld(mouse);
        _center = new Point(_center.X + before.X - after.X, _center.Y + before.Y - after.Y);
        InvalidateVisual();
        e.Handled = true;
    }

    protected override void OnMouseLeftButtonDown(MouseButtonEventArgs e)
    {
        Focus();
        _dragFrom = e.GetPosition(this);
        _dragCenter = _center;
        _dragged = false;
        CaptureMouse();
        e.Handled = true;
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        if (_dragFrom is not { } from || e.LeftButton != MouseButtonState.Pressed)
            return;
        var now = e.GetPosition(this);
        if ((now - from).Length > 4)
            _dragged = true;
        if (!_dragged)
            return;
        Follow = false;
        _center = new Point(_dragCenter.X + (now.Y - from.Y) / _scale, _dragCenter.Y - (now.X - from.X) / _scale);
        InvalidateVisual();
    }

    protected override void OnMouseLeftButtonUp(MouseButtonEventArgs e)
    {
        ReleaseMouseCapture();
        var at = e.GetPosition(this);
        if (_dragFrom != null && !_dragged)
        {
            // 클릭 - 가장 가까운 플레이어 · 몬스터 · 가방 (10 px 안)
            MapObject? best = null;
            double bestD = 10;
            foreach (var o in _objects)
            {
                if (o.K is not ("p" or "m" or "bag"))
                    continue;
                double d = (ToScreen(o.X, o.Y) - at).Length;
                if (d < bestD) { bestD = d; best = o; }
            }
            if (best != null)
                ObjectClicked?.Invoke(best);
        }
        _dragFrom = null;
    }

    public void CenterOn(long id)
    {
        if (Find(id) is { } o)
        {
            _center = new Point(o.X, o.Y);
            if (_scale < 0.03)
                _scale = 0.05;
            InvalidateVisual();
        }
    }
}
