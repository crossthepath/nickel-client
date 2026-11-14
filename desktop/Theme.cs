using System.Globalization;
using System.Windows;
using System.Windows.Media;

namespace Nickel.Desktop;

// Nickel themes; same values as native/theme.hpp (in-game menu).
public sealed record ThemeDef(string Name, int Pattern, string Glow, string Base, string Deep, string Ink, string Accent, string AccentDeep);

public static class Themes
{
 public static readonly ThemeDef[] All =
 {
  new("Orange", 0, "#FF9348", "#E8590F", "#B13704", "#7E2602", "#FF8A3D", "#E8590F"),
  new("Blueprint", 1, "#35AEFF", "#0A72D6", "#0753A3", "#022B5E", "#4DABFF", "#0A72D6"),
  new("Cutting Mat", 2, "#31C99A", "#0E9A6F", "#08734F", "#033F2A", "#34D399", "#0E9A6F"),
  new("Violet", 0, "#B07CFF", "#6E5BFF", "#4B3BB0", "#1B1450", "#B47BFF", "#8B41F0"),
  new("Crimson", 0, "#FF7B6B", "#E23A3A", "#A61E24", "#520A0E", "#FF6363", "#E23A3A"),
  new("Graphite", 1, "#8A8A96", "#45454F", "#2A2A33", "#0D0D0F", "#EBEBF0", "#6E6E7A"),
 };
 public static int Index { get; set; }
 public static ThemeDef Current => All[Math.Clamp(Index, 0, All.Length - 1)];
 public static Color Parse(string hex) { var v = Convert.ToUInt32(hex[1..], 16); return Color.FromRgb((byte)(v >> 16), (byte)(v >> 8), (byte)v); }
 public static Color Mix(Color a, Color b, double t) => Color.FromArgb((byte)(a.A + (b.A - a.A) * t), (byte)(a.R + (b.R - a.R) * t), (byte)(a.G + (b.G - a.G) * t), (byte)(a.B + (b.B - a.B) * t));
}

/// <summary>
/// Animated backdrop: plan (Orange / Violet / Crimson), blue grid (Blueprint / Graphite) or cutting mat (Cutting Mat).
/// Theme radial gradient, construction lines, arcs drawn in on appearance, sweeping rays, ruler reticle, sliding
/// highlights; colours and patterns cross-fade when the theme changes.
/// </summary>
public sealed class Backdrop : FrameworkElement
{
 private readonly DateTime born = DateTime.Now;
 private Color glow, @base, deep, ink;
 private readonly double[] pattern = new double[3];
 private bool ready;
 private DateTime last = DateTime.Now;
 private readonly Dictionary<string, FormattedText> labels = new();
 private static readonly Typeface Face = new(new FontFamily(new Uri("pack://application:,,,/"), "./Assets/Nickel/#Satoshi"), FontStyles.Normal, FontWeights.Normal, FontStretches.Normal);
 public bool Animated { get; set; } = true;
 /// <summary>Set while the window is being moved or resized: the backdrop keeps its last frame so the move stays smooth.</summary>
 public static bool Frozen { get; set; }

 public Backdrop()
 {
  IsHitTestVisible = false;
  Loaded += (_, _) => CompositionTarget.Rendering += Tick;
  Unloaded += (_, _) => CompositionTarget.Rendering -= Tick;
 }
 private void Tick(object? sender, EventArgs e) { if (IsVisible && !Frozen) InvalidateVisual(); }

 private void Step()
 {
  var t = Themes.Current; var now = DateTime.Now; var dt = Math.Min(.1, (now - last).TotalSeconds); last = now;
  if (!ready) { glow = Themes.Parse(t.Glow); @base = Themes.Parse(t.Base); deep = Themes.Parse(t.Deep); ink = Themes.Parse(t.Ink); for (var i = 0; i < 3; i++) pattern[i] = i == t.Pattern ? 1 : 0; ready = true; return; }
  var k = 1 - Math.Exp(-6 * dt);
  glow = Themes.Mix(glow, Themes.Parse(t.Glow), k); @base = Themes.Mix(@base, Themes.Parse(t.Base), k); deep = Themes.Mix(deep, Themes.Parse(t.Deep), k); ink = Themes.Mix(ink, Themes.Parse(t.Ink), k);
  for (var i = 0; i < 3; i++) pattern[i] += ((i == t.Pattern ? 1 : 0) - pattern[i]) * k;
 }

 protected override void OnRender(DrawingContext dc)
 {
  Step();
  double w = ActualWidth, h = ActualHeight; if (w < 2 || h < 2) return;
  var time = Animated ? (DateTime.Now - born).TotalSeconds : 4;
  var traced = Math.Clamp(time / 1.4, 0, 1); traced = 1 - Math.Pow(1 - traced, 3);
  var breathe = 1 + .05 * Math.Sin(time * Math.Tau / 18);
  var gc = new Point(w * (.5 * pattern[0] + .64 * pattern[1] + .5 * pattern[2]) + w * .03 * Math.Sin(time * Math.Tau / 22), h * (-.06 * pattern[0] + .5 * pattern[1] + .45 * pattern[2]));
  var radius = Math.Max(w, h) * (1.0 * pattern[0] + .62 * pattern[1] + .95 * pattern[2]) * breathe;
  var brush = new RadialGradientBrush { MappingMode = BrushMappingMode.Absolute, Center = gc, GradientOrigin = gc, RadiusX = radius, RadiusY = radius };
  brush.GradientStops.Add(new GradientStop(glow, 0)); brush.GradientStops.Add(new GradientStop(@base, .42)); brush.GradientStops.Add(new GradientStop(deep, .78)); brush.GradientStops.Add(new GradientStop(ink, 1));
  dc.DrawRectangle(brush, null, new Rect(0, 0, w, h));
  dc.PushClip(new RectangleGeometry(new Rect(0, 0, w, h)));
  if (pattern[0] > .01) Plan(dc, w, h, pattern[0] * .8, time, traced);
  if (pattern[1] > .01) Grid(dc, w, h, pattern[1] * .8, time, traced);
  if (pattern[2] > .01) Mat(dc, w, h, pattern[2] * .8, time, traced);
  dc.Pop();
 }

 private static Pen P(double alpha, double thickness = 1, bool dashed = false)
 {
  var pen = new Pen(new SolidColorBrush(Color.FromArgb((byte)Math.Clamp(alpha * 255, 0, 255), 255, 255, 255)), thickness);
  if (dashed) pen.DashStyle = new DashStyle(new[] { 3.0, 4.0 }, 0);
  return pen;
 }
 private void Text(DrawingContext dc, string text, Point at, double alpha, double size, bool center)
 {
  var key = text + "|" + size;
  if (!labels.TryGetValue(key, out var formatted)) { formatted = new FormattedText(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Face, size, Brushes.White, 1.0); labels[key] = formatted; }
  dc.PushOpacity(Math.Clamp(alpha, 0, 1)); dc.DrawText(formatted, new Point(center ? at.X - formatted.Width / 2 : at.X, at.Y)); dc.Pop();
 }
 private static string Num(double v) => Math.Abs(v - Math.Round(v)) < .01 ? ((int)Math.Round(v)).ToString(CultureInfo.InvariantCulture) : v.ToString("0.0", CultureInfo.InvariantCulture);
 private static double RayLength(Point o, double ux, double uy, double w, double h)
 {
  var t = 1e9; if (ux > 1e-4) t = Math.Min(t, (w - o.X) / ux); if (ux < -1e-4) t = Math.Min(t, -o.X / ux); if (uy > 1e-4) t = Math.Min(t, (h - o.Y) / uy); if (uy < -1e-4) t = Math.Min(t, -o.Y / uy);
  return Math.Max(0, t);
 }
 private static void Arc(DrawingContext dc, Point o, double r, double traced, Pen pen)
 {
  if (traced <= 0) return; var end = -Math.PI / 2 * traced;
  var geometry = new StreamGeometry();
  using (var g = geometry.Open()) { g.BeginFigure(new Point(o.X + r, o.Y), false, false); g.ArcTo(new Point(o.X + Math.Cos(end) * r, o.Y + Math.Sin(end) * r), new Size(r, r), 0, false, SweepDirection.Counterclockwise, true, false); }
  dc.DrawGeometry(null, pen, geometry);
 }

 // Orange plan: origin at the bottom left, unit = height / 10.5
 private void Plan(DrawingContext dc, double w, double h, double a, double time, double traced)
 {
  double m = 40, u = (h - 2 * m) / 10.5; var o = new Point(m, h - m); double right = w - m, top = m * .4;
  for (var x = o.X + 5 * u; x < right - 2; x += 5 * u) dc.DrawLine(P(.42 * a), new Point(x, 0), new Point(x, o.Y));
  for (var y = o.Y - 5 * u; y > 2; y -= 5 * u) dc.DrawLine(P(.42 * a), new Point(0, y), new Point(w, y));
  dc.DrawLine(P(.6 * a), new Point(o.X, top), o); dc.DrawLine(P(.6 * a), o, new Point(right, o.Y)); dc.DrawLine(P(.6 * a), new Point(right, top), new Point(right, o.Y));
  for (var i = 0; o.X + i * .5 * u <= right + 1; i++) { var x = o.X + i * .5 * u; var major = i % 5 == 0; dc.DrawLine(P(.55 * a), new Point(x, o.Y), new Point(x, o.Y + (major ? 7 : 4))); if (major) Text(dc, Num(i * .5), new Point(x, o.Y + 9), .72 * a, 9, true); }
  for (var i = 1; o.Y - i * .5 * u >= top; i++)
  {
   var y = o.Y - i * .5 * u; var major = i % 5 == 0; dc.DrawLine(P(.55 * a), new Point(o.X - (major ? 7 : 4), y), new Point(o.X, y)); dc.DrawLine(P(.55 * a), new Point(right, y), new Point(right + (major ? 7 : 4), y));
   if (major) { Text(dc, Num(i * .5), new Point(o.X - 26, y - 6), .72 * a, 9, false); Text(dc, Num(i * .5), new Point(right + 10, y - 6), .72 * a, 9, false); }
  }
  for (var r = 4; r <= 12; r += 4)
  {
   var R = r * u; Arc(dc, o, R, traced, P(.5 * a));
   for (var g = 1; g < 6; g++) { var ang = -g * Math.PI / 12; if (ang < -Math.PI / 2 * traced) break; dc.DrawLine(P(.5 * a), new Point(o.X + Math.Cos(ang) * (R - 3), o.Y + Math.Sin(ang) * (R - 3)), new Point(o.X + Math.Cos(ang) * (R + 3), o.Y + Math.Sin(ang) * (R + 3))); }
  }
  for (var i = 1; i <= 4; i++)
  {
   var deg = 15.0 * i + 1.6 * Math.Sin(time * Math.Tau / 15 + i * 1.3); var ang = -deg * Math.PI / 180; double ux = Math.Cos(ang), uy = Math.Sin(ang);
   double lab = 4.6 * u, len = RayLength(o, ux, uy, w, h);
   dc.DrawLine(P(.48 * a, 1, true), o, new Point(o.X + ux * (lab - 12), o.Y + uy * (lab - 12))); dc.DrawLine(P(.48 * a, 1, true), new Point(o.X + ux * (lab + 12), o.Y + uy * (lab + 12)), new Point(o.X + ux * len, o.Y + uy * len));
   Text(dc, $"{15 * i}°", new Point(o.X + ux * lab, o.Y + uy * lab - 6), .78 * a, 9, true);
  }
  var f = time / 14 % 1; var rx = o.X + (right - o.X) * (.5 - .5 * Math.Cos(f * Math.Tau));
  dc.DrawLine(P(.7 * a), new Point(rx, o.Y - 14), new Point(rx, o.Y + 14)); dc.DrawEllipse(null, P(.75 * a), new Point(rx, o.Y), 4, 4);
 }

 // Blue plan: fine grid, major squares, arcs 10/20/30, rays, rulers
 private void Grid(DrawingContext dc, double w, double h, double a, double time, double traced)
 {
  double m = 32, u = (h - 2 * m) / 26; var o = new Point(m, h - m); var right = w - m;
  var i = 0; for (var x = o.X; x <= right + 1; x += u, i++) dc.DrawLine(P((i % 5 == 0 ? .4 : .1) * a), new Point(x, 0), new Point(x, o.Y));
  i = 0; for (var y = o.Y; y >= 0; y -= u, i++) dc.DrawLine(P((i % 5 == 0 ? .4 : .1) * a), new Point(0, y), new Point(w, y));
  dc.DrawLine(P(.6 * a), o, new Point(right, o.Y));
  for (var k = 0; o.X + k * u <= right + 1; k++) { var x = o.X + k * u; dc.DrawLine(P(.5 * a), new Point(x, o.Y), new Point(x, o.Y + (k % 5 == 0 ? 7 : 4))); if (k % 5 == 0) Text(dc, Num(k), new Point(x, o.Y + 9), .68 * a, 9, true); }
  for (var k = 5; o.Y - k * u >= 8; k += 5) { var y = o.Y - k * u; Text(dc, Num(k), new Point(o.X - 20, y - 6), .68 * a, 9, false); Text(dc, Num(k), new Point(right + 8, y - 6), .68 * a, 9, false); }
  for (var r = 10; r <= 30; r += 10) Arc(dc, o, r * u, traced, P(.42 * a));
  for (var k = 1; k <= 4; k++)
  {
   var deg = 15.0 * k + 1.2 * Math.Sin(time * Math.Tau / 17 + k); var ang = -deg * Math.PI / 180; double ux = Math.Cos(ang), uy = Math.Sin(ang); var len = RayLength(o, ux, uy, w, h);
   dc.DrawLine(P(.36 * a, 1, true), o, new Point(o.X + ux * len, o.Y + uy * len)); var lab = (k == 1 ? 10.5 : 6.5) * u; Text(dc, $"{15 * k}°", new Point(o.X + ux * lab, o.Y + uy * lab - 12), .72 * a, 9, true);
  }
  var f = time / 16 % 1; var rx = o.X + (right - o.X) * (.5 - .5 * Math.Cos(f * Math.Tau));
  dc.DrawLine(P(.16 * a), new Point(rx, 0), new Point(rx, o.Y)); dc.DrawEllipse(null, P(.7 * a), new Point(rx, o.Y), 4, 4);
 }

 // Cutting mat: diagonal highlights, fine grid, thick squares every 5, diagonal, numbered rulers
 private void Mat(DrawingContext dc, double w, double h, double a, double time, double traced)
 {
  double m = 18, u = (h - 2 * m) / 28; var o = new Point(m, h - m); double right = w - m, top = m;
  double band = w * .07, shift = time * 14 % (w * .36);
  var veil = new SolidColorBrush(Color.FromArgb((byte)(.05 * a * 255), 255, 255, 255));
  for (var k = -2; k < 6; k++)
  {
   var x0 = k * w * .36 + shift; var g = new StreamGeometry();
   using (var c = g.Open()) { c.BeginFigure(new Point(x0, 0), true, true); c.PolyLineTo(new[] { new Point(x0 + band, 0), new Point(x0 + band - h * .8, h), new Point(x0 - h * .8, h) }, false, false); }
   dc.DrawGeometry(veil, null, g);
  }
  var i = 0; for (var x = o.X; x <= right + 1; x += u, i++) dc.DrawLine(P((i % 5 == 0 ? .9 : .16) * a, i % 5 == 0 ? 2 : 1), new Point(x, top), new Point(x, o.Y));
  i = 0; for (var y = o.Y; y >= top - 1; y -= u, i++) dc.DrawLine(P((i % 5 == 0 ? .9 : .16) * a, i % 5 == 0 ? 2 : 1), new Point(o.X, y), new Point(right, y));
  dc.DrawRectangle(null, P(.9 * a, 2), new Rect(o.X, top, right - o.X, o.Y - top));
  var len = Math.Min(right - o.X, o.Y - top) * traced; dc.DrawLine(P(.85 * a, 1.5), o, new Point(o.X + len, o.Y - len));
  for (var k = 0; o.X + k * u <= right + 1; k++) { var x = o.X + k * u; Text(dc, Num(k), new Point(x, h - m + 2), .7 * a, 8, true); Text(dc, Num(k), new Point(x, 2), .7 * a, 8, true); }
  for (var k = 0; o.Y - k * u >= top - 1; k++) { var y = o.Y - k * u; Text(dc, Num(k), new Point(m / 2, y - 5), .7 * a, 8, true); Text(dc, Num(k), new Point(w - m / 2, y - 5), .7 * a, 8, true); }
 }
}
