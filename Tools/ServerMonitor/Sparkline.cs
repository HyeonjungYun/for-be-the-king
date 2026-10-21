using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace ServerMonitor;

/// <summary>지표 하나의 최근 값 꺾은선 (10 초 통계마다 한 점, 최근 90 점 = 15 분).</summary>
public sealed class Sparkline : FrameworkElement
{
    readonly List<double> _values = new();
    public string Title { get; set; } = "";
    public string Unit { get; set; } = "";
    public double? Limit { get; set; }         // 기준선 (예: P95 16.6 ms)
    public Color LineColor { get; set; } = Color.FromRgb(0x4F, 0xC3, 0xF7);
    public int Capacity { get; set; } = 90;

    public void Add(double v)
    {
        _values.Add(v);
        if (_values.Count > Capacity)
            _values.RemoveAt(0);
        InvalidateVisual();
    }

    public void Reset()
    {
        _values.Clear();
        InvalidateVisual();
    }

    static readonly Typeface Face = new("Malgun Gothic");

    protected override void OnRender(DrawingContext dc)
    {
        var bg = new SolidColorBrush(Color.FromRgb(0x1E, 0x24, 0x2B));
        dc.DrawRoundedRectangle(bg, null, new Rect(0, 0, ActualWidth, ActualHeight), 4, 4);
        double dip = VisualTreeHelper.GetDpi(this).PixelsPerDip;
        string last = _values.Count > 0 ? _values[^1].ToString(_values[^1] >= 100 ? "0" : "0.##", CultureInfo.InvariantCulture) : "-";
        var title = new FormattedText($"{Title}  {last} {Unit}", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, Face, 12, Brushes.WhiteSmoke, dip);
        dc.DrawText(title, new Point(8, 4));
        if (_values.Count < 2)
            return;

        double top = 24, bottom = ActualHeight - 6, left = 6, right = ActualWidth - 6;
        double max = Math.Max(_values.Max(), Limit ?? 0) * 1.15;
        if (max <= 0) max = 1;
        double X(int i) => left + (right - left) * i / (Capacity - 1.0);
        double Y(double v) => bottom - (bottom - top) * v / max;
        if (Limit is { } lim)
        {
            var dash = new Pen(new SolidColorBrush(Color.FromArgb(0x90, 0xFF, 0x70, 0x43)), 1) { DashStyle = DashStyles.Dash };
            dc.DrawLine(dash, new Point(left, Y(lim)), new Point(right, Y(lim)));
        }
        var geo = new StreamGeometry();
        int offset = Capacity - _values.Count;
        using (var ctx = geo.Open())
        {
            ctx.BeginFigure(new Point(X(offset), Y(_values[0])), false, false);
            for (int i = 1; i < _values.Count; i++)
                ctx.LineTo(new Point(X(offset + i), Y(_values[i])), true, false);
        }
        dc.DrawGeometry(null, new Pen(new SolidColorBrush(LineColor), 1.6), geo);
        var peak = new FormattedText($"최대 {_values.Max().ToString("0.##", CultureInfo.InvariantCulture)}", CultureInfo.CurrentUICulture, FlowDirection.LeftToRight, Face, 10, Brushes.Gray, dip);
        dc.DrawText(peak, new Point(ActualWidth - peak.Width - 8, 6));
    }
}
