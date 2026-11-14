using System.Windows;
using System.Windows.Media;
namespace Nickel.Desktop;

/// <summary>
/// Lays out a single line of text the way RmlUi does with FreeType:
/// - the face is rasterised at the integer size (13.5dp -> 13 px);
/// - each glyph advances by a whole number of pixels (rounded advance), without GPOS kerning;
/// - the line box is line-height x declared size; text is centred by half-leading,
///   with ascent and descent rounded up to the pixel;
/// - the baseline is snapped to the pixel grid.
/// </summary>
internal sealed class RmlText : FrameworkElement
{
 private static readonly Dictionary<int, GlyphTypeface> Faces = new();
 private readonly GlyphTypeface face;
 private readonly ushort[] glyphs;
 private readonly double[] advances;
 private readonly double size, lineHeight, width, baseline;
 private Point origin = new(double.NaN, double.NaN);

 public SolidColorBrush Foreground { get; set; }

 public RmlText(string text, double dp, FontWeight weight, SolidColorBrush foreground, double lineHeightFactor = 1.2)
 {
  face = Face(weight); size = Math.Floor(dp); lineHeight = dp * lineHeightFactor; Foreground = foreground;
  glyphs = new ushort[text.Length]; advances = new double[text.Length];
  for (var i = 0; i < text.Length; i++)
  {
   face.CharacterToGlyphMap.TryGetValue(text[i], out var glyph);
   glyphs[i] = glyph; advances[i] = Math.Round(face.AdvanceWidths[glyph] * size, MidpointRounding.AwayFromZero); width += advances[i];
  }
  double ascent = Math.Ceiling(face.Baseline * size), descent = Math.Ceiling((face.Height - face.Baseline) * size);
  baseline = (lineHeight - (ascent + descent)) / 2 + ascent;
  IsHitTestVisible = false;
  LayoutUpdated += (_, _) => { var current = LayoutOrigin(); if (current != origin) InvalidateVisual(); };
 }

 internal static GlyphTypeface Face(FontWeight weight)
 {
  var key = weight.ToOpenTypeWeight();
  if (Faces.TryGetValue(key, out var cached)) return cached;
  var family = new FontFamily(new Uri("pack://application:,,,/"), "./Assets/Nickel/#Satoshi");
  if (!new Typeface(family, FontStyles.Normal, weight, FontStretches.Normal).TryGetGlyphTypeface(out var resolved))
   throw new InvalidOperationException("Satoshi face missing for weight " + key);
  return Faces[key] = resolved;
 }

 /// <summary>Position from layout alone (animated transforms are ignored).</summary>
 private Point LayoutOrigin()
 {
  double x = 0, y = 0;
  for (Visual? visual = this; visual != null; visual = VisualTreeHelper.GetParent(visual) as Visual) { var offset = VisualTreeHelper.GetOffset(visual); x += offset.X; y += offset.Y; }
  return new Point(x, y);
 }

 protected override Size MeasureOverride(Size availableSize) => new(width, lineHeight);
 protected override Size ArrangeOverride(Size finalSize) => new(width, lineHeight);

 protected override void OnRender(DrawingContext context)
 {
  origin = LayoutOrigin();
  if (glyphs.Length == 0) return;
  var x = Math.Round(origin.X, MidpointRounding.AwayFromZero) - origin.X;
  var y = Math.Round(origin.Y + baseline, MidpointRounding.AwayFromZero) - origin.Y;
  var run = new GlyphRun(face, 0, false, size, (float)VisualTreeHelper.GetDpi(this).PixelsPerDip, glyphs, new Point(x, y), advances, null, null, null, null, null, null);
  context.DrawGlyphRun(Foreground, run);
 }
}
