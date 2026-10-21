using System.Globalization;
using System.IO;
using System.Text.Json;
using System.Windows;

namespace ServerMonitor;

/// <summary>층의 지형 - 서버가 읽는 Server/Data 의 CSV 를 그대로 (walls · floor_outline · houses · towers · entry_points). 단위 cm, +X 북 · +Y 동.</summary>
public sealed class FloorGeometry
{
    public sealed record Wall(string Kind, Point[] Corners);

    public List<Point> Outline { get; } = new();
    public List<Wall> Walls { get; } = new();
    public List<Rect> Houses { get; } = new();
    public List<(string Id, Point P)> Towers { get; } = new();
    public List<(int Id, Point P)> Entries { get; } = new();
    public Rect Bounds { get; private set; } = new(-30000, -30000, 60000, 60000);

    static readonly Dictionary<string, FloorGeometry> Cache = new();

    public static FloorGeometry Load(string? dataDir, int floor)
    {
        string key = $"{dataDir}|{floor}";
        if (Cache.TryGetValue(key, out var cached))
            return cached;
        var g = new FloorGeometry();
        if (dataDir != null)
        {
            foreach (var r in Rows(Path.Combine(dataDir, "floor_outline.csv")).Where(r => I(r, "floor_id") == floor).OrderBy(r => I(r, "seq")))
                g.Outline.Add(new Point(F(r, "x"), F(r, "y")));
            foreach (var r in Rows(Path.Combine(dataDir, "walls.csv")).Where(r => I(r, "floor_id") == floor))
            {
                double cx = F(r, "cx"), cy = F(r, "cy"), hl = F(r, "half_length"), ht = F(r, "half_thickness"), yaw = F(r, "yaw") * Math.PI / 180;
                // 서버 WallGeom::ComputeAxis - 길이 축 u = (cos yaw, sin yaw), 두께 축 v = (-sin, cos)
                double ux = Math.Cos(yaw), uy = Math.Sin(yaw), vx = -uy, vy = ux;
                g.Walls.Add(new Wall(r.GetValueOrDefault("kind", "wall"), new[]
                {
                    new Point(cx + ux * hl + vx * ht, cy + uy * hl + vy * ht),
                    new Point(cx - ux * hl + vx * ht, cy - uy * hl + vy * ht),
                    new Point(cx - ux * hl - vx * ht, cy - uy * hl - vy * ht),
                    new Point(cx + ux * hl - vx * ht, cy + uy * hl - vy * ht),
                }));
            }
            foreach (var r in Rows(Path.Combine(dataDir, "houses.csv")).Where(r => I(r, "floor_id") == floor))
                g.Houses.Add(new Rect(new Point(F(r, "x0"), F(r, "y0")), new Point(F(r, "x1"), F(r, "y1"))));
            foreach (var r in Rows(Path.Combine(dataDir, "towers.csv")).Where(r => I(r, "floor_id") == floor))
                g.Towers.Add((r.GetValueOrDefault("tower_id", "?"), new Point(F(r, "x"), F(r, "y"))));
            foreach (var r in Rows(Path.Combine(dataDir, "entry_points.csv")).Where(r => I(r, "floor_id") == floor))
                g.Entries.Add((I(r, "entry_id"), new Point(F(r, "x"), F(r, "y"))));
        }
        var pts = g.Outline.Concat(g.Walls.SelectMany(w => w.Corners)).ToList();
        if (pts.Count > 0)
        {
            double x0 = pts.Min(p => p.X), x1 = pts.Max(p => p.X), y0 = pts.Min(p => p.Y), y1 = pts.Max(p => p.Y);
            g.Bounds = new Rect(x0 - 1000, y0 - 1000, x1 - x0 + 2000, y1 - y0 + 2000);
        }
        Cache[key] = g;
        return g;
    }

    static IEnumerable<Dictionary<string, string>> Rows(string path)
    {
        if (!File.Exists(path))
            yield break;
        string[]? header = null;
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.TrimEnd('\r').TrimStart('﻿');
            if (line.Length == 0)
                continue;
            var cells = line.Split(',');
            if (header == null) { header = cells; continue; }
            var row = new Dictionary<string, string>();
            for (int i = 0; i < header.Length && i < cells.Length; i++)
                row[header[i]] = cells[i];
            yield return row;
        }
    }

    static double F(Dictionary<string, string> r, string k) => r.TryGetValue(k, out var v) && double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : 0;
    static int I(Dictionary<string, string> r, string k) => r.TryGetValue(k, out var v) && int.TryParse(v, out var i) ? i : -1;
}

/// <summary>맵 한 장면 (관리 포트 "map").</summary>
public sealed class MapObject
{
    public long Id; public string K = ""; public double X, Y, Yaw; public int Hp, MaxHp; public string Name = "";
    public bool Bot; public long Party; public double Vision; public string State = ""; public long Target; public long Pack;
    public bool Chest, Open, Looted; public int Items; public long Owner; public double Radius; public double Dx, Dy; public long Caster;
    public int GaugeMs; public bool Filling; public int Occupants;

    public static MapObject From(JsonElement o)
    {
        double D(string k) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : 0;
        long L(string k) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.Number ? (long)v.GetUInt64() : 0;
        string S(string k) => o.TryGetProperty(k, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";
        bool B(string k) => o.TryGetProperty(k, out var v) && (v.ValueKind == JsonValueKind.True || (v.ValueKind == JsonValueKind.Number && v.GetInt32() != 0));
        return new MapObject
        {
            Id = L("id"), K = S("k"), X = D("x"), Y = D("y"), Yaw = D("yaw"), Hp = (int)D("hp"), MaxHp = (int)D("mhp"), Name = S("n"),
            Bot = B("bot"), Party = L("party"), Vision = D("vr"), State = S("st"), Target = L("tgt"), Pack = L("pack"),
            Chest = B("chest"), Open = B("open"), Looted = B("looted"), Items = (int)D("items"), Owner = L("owner"), Radius = D("r"), Dx = D("dx"), Dy = D("dy"),
            Caster = L("caster"), GaugeMs = (int)D("gauge_ms"), Filling = B("filling"), Occupants = (int)D("occupants"),
        };
    }
}
