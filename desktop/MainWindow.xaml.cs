using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Shapes = System.Windows.Shapes;
namespace Nickel.Desktop;

// Launcher window: Home / Settings / Credits.
// Left: a borderless 208 px sidebar on #08080B with 36 px entries (square, active #14141B). Right: the
// theme's animated backdrop (Theme.cs) with a header (breadcrumb, large title, highlighted accent word) and
// a floating #0F0F14 panel (square, #1A1A21 border). Lists use hairline separators and numbered section
// labels ("01 · LOADING"). Stroke icons are drawn here on a 24-unit grid; text uses Satoshi.
public partial class MainWindow : Window
{
 private const double SidebarWidth = 208, PageWidth = 880 - SidebarWidth, WindowHeight = 540;
 private static readonly IEasingFunction CubicOut = Frozen(new CubicEase { EasingMode = EasingMode.EaseOut });
 private static readonly IEasingFunction QuadraticIn = Frozen(new QuadraticEase { EasingMode = EasingMode.EaseIn });
 private static readonly string[] PageNames = { "Home", "Settings", "Credits" };
 internal const string GitHubUrl = "https://github.com/crossthepath/nickel-client", DiscordUrl = "https://discord.gg/pn2FdFhY9j", WebsiteUrl = "https://nickelclient.com";
 private static void OpenLink(string url) { try { Process.Start(new ProcessStartInfo(url) { UseShellExecute = true })?.Dispose(); } catch { } }

 private readonly bool review;
 private readonly Preferences preferences = Preferences.Load();
 private readonly CancellationTokenSource lifetime = new();
 private readonly DispatcherTimer monitor = new() { Interval = TimeSpan.FromSeconds(1) };
 private MinecraftConnection? connection;
 private bool busy, closed, loaded, failed;
 private int autoAttemptPid;
 private string message = "", page = "";

 private readonly Dictionary<string, FrameworkElement> pages = new();
 private readonly Dictionary<string, NavItem> navigation = new();
 private readonly TranslateTransform highlightShift = new();
 private readonly List<(SolidColorBrush Track, Func<bool> On)> switches = new();
 private readonly List<Border> swatches = new();
 private Backdrop backdrop = null!;
 private LoadButton load = null!;
 private Border homeNotice = null!, settingsNotice = null!;
 private TextBlock homeNoticeText = null!, settingsNoticeText = null!;
 private SolidColorBrush gameDot = null!, highlightInk = null!;
 private TextBlock gameText = null!, statusText = null!;

 private sealed record NavItem(SolidColorBrush Icon, SolidColorBrush Label);
 private sealed class LoadButton
 {
  public Grid Surface = null!; public FrameworkElement Glow = null!; public SolidColorBrush Back = null!, Outline = null!, Label = null!;
  public Shapes.Rectangle Fill = null!; public StackPanel Content = null!; public TranslateTransform Press = null!;
 }

 public MainWindow(bool review = false)
 {
  this.review = review;
  if (!review) Updater.Cleanup();
  Themes.Index = Math.Clamp(preferences.Theme, 0, Themes.All.Length - 1);
  InitializeComponent();
  BuildBackdrop();
  BuildSidebar();
  BuildPages();
  BuildWindowControls();
  Navigate("Home", false);
  if (!review) minecraftRunning = MinecraftIsRunning();
  Refresh();
  monitor.Tick += Monitor_Tick;
  if (!review) monitor.Start();
  if (!review) LauncherPresence.SetEnabled(preferences.DiscordPresence);
  if (!review) { if (string.IsNullOrEmpty(preferences.InstallId)) { preferences.InstallId = UsagePing.NewId(); Save(); } UsagePing.SetEnabled(preferences.UsageStatistics, preferences.InstallId); }
  SourceInitialized += (_, _) => { ApplyWindowFrame(); if (!review) ApplyStreamHide(); if (PresentationSource.FromVisual(this) is HwndSource source) source.AddHook(MoveHook); };
  Root.MouseLeftButtonDown += (_, e) => { if (e.GetPosition(Root).Y < 40 && e.ButtonState == MouseButtonState.Pressed) DragMove(); };
  Closed += (_, _) => { closed = true; lifetime.Cancel(); monitor.Stop(); connection?.Dispose(); connection = null; lifetime.Dispose(); };
 }

 // ---------------------------------------------------------------- helpers
 private static T Frozen<T>(T value) where T : Freezable { value.Freeze(); return value; }
 /// <summary>Parses a CSS colour #rrggbb or #rrggbbaa (alpha last).</summary>
 private static Color C(string css)
 {
  var v = Convert.ToUInt32(css[1..], 16);
  return css.Length == 9 ? Color.FromArgb((byte)v, (byte)(v >> 24), (byte)(v >> 16), (byte)(v >> 8)) : Color.FromRgb((byte)(v >> 16), (byte)(v >> 8), (byte)v);
 }
 private static SolidColorBrush B(string css) => new(C(css));
 private static string Hex(Color c) => $"#{c.R:x2}{c.G:x2}{c.B:x2}";
 private static string Accent => Themes.Current.Accent;
 private static string AccentDeep => Themes.Current.AccentDeep;
 private static BitmapImage Bitmap(string name) => Frozen(new BitmapImage(new Uri("pack://application:,,,/Assets/Nickel/" + name)));
 private static readonly FontWeight Regular = FontWeights.Normal, Medium = FontWeights.Medium, SemiBold = FontWeights.SemiBold, Bold = FontWeights.Bold;

 private static RmlText T(string value, double dp, FontWeight weight, string color)
  => new(value, dp, weight, B(color)) { VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Left };
 private static TextBlock Wrapped(double dp, FontWeight weight, string color, double lineHeight)
 {
  var text = new TextBlock
  {
   FontSize = dp, FontWeight = weight, Foreground = B(color), TextWrapping = TextWrapping.Wrap,
   LineHeight = dp * lineHeight, LineStackingStrategy = LineStackingStrategy.BlockLineHeight, VerticalAlignment = VerticalAlignment.Top,
  };
  return text;
 }
 private static Image Picture(string name, double width, double height)
 {
  var image = new Image { Source = Bitmap(name), Width = width, Height = height, Stretch = Stretch.Uniform };
  RenderOptions.SetBitmapScalingMode(image, BitmapScalingMode.HighQuality);
  return image;
 }
 private static T At<T>(T element, double x, double y) where T : FrameworkElement
 {
  element.HorizontalAlignment = HorizontalAlignment.Left; element.VerticalAlignment = VerticalAlignment.Top; element.Margin = new Thickness(x, y, 0, 0);
  return element;
 }
 private void Fade(SolidColorBrush brush, string to, double seconds)
 {
  var color = C(to);
  if (Still || seconds <= 0) { brush.BeginAnimation(SolidColorBrush.ColorProperty, null); brush.Color = color; }
  else brush.BeginAnimation(SolidColorBrush.ColorProperty, new ColorAnimation(color, TimeSpan.FromSeconds(seconds)) { EasingFunction = CubicOut });
 }
 private void Slide(IAnimatable target, DependencyProperty property, double to, double seconds, IEasingFunction? easing = null, double delay = 0)
 {
  if (Still || seconds <= 0) { target.BeginAnimation(property, null); ((DependencyObject)target).SetValue(property, to); }
  else target.BeginAnimation(property, new DoubleAnimation(to, TimeSpan.FromSeconds(seconds)) { EasingFunction = easing ?? CubicOut, BeginTime = TimeSpan.FromSeconds(delay) });
 }
 private static void OnClick(UIElement element, Action action)
 {
  element.MouseLeftButtonDown += (_, e) => e.Handled = true;
  element.MouseLeftButtonUp += (_, e) => { e.Handled = true; action(); };
 }

 // Stroke icons (24-unit grid, 1.7 stroke, round caps).
 // Lucide icons (https://lucide.dev, ISC licence): path data of the 24 x 24 stroked SVGs, drawn with stroke width 2.
 // Brand logos come from Simple Icons (https://simpleicons.org, CC0) and are filled.
 private static readonly Dictionary<string, string> IconPaths = new()
 {
  ["home"] = "M 15 21 v -8 a 1 1 0 0 0 -1 -1 h -4 a 1 1 0 0 0 -1 1 v 8 M 3 10 a 2 2 0 0 1 .709 -1.528 l 7 -6 a 2 2 0 0 1 2.582 0 l 7 6 A 2 2 0 0 1 21 10 v 9 a 2 2 0 0 1 -2 2 H 5 a 2 2 0 0 1 -2 -2 z",   // house
  ["settings"] = "M 10 5 H 3 M 12 19 H 3 M 14 3 v 4 M 16 17 v 4 M 21 12 h -9 M 21 19 h -5 M 21 5 h -7 M 8 10 v 4 M 8 12 H 3",   // sliders-horizontal
  ["credits"] = "M 2 12 A 10 10 0 1 0 22 12 A 10 10 0 1 0 2 12 Z M 12 16 v -4 M 12 8 h .01",   // info
  ["minimize"] = "M 5 12 h 14",   // minus
  ["close"] = "M 18 6 L 6 18 M 6 6 l 12 12",   // x
  ["check"] = "M 20 6 L 9 17 l -5 -5",   // check
  ["retry"] = "M 21 12 a 9 9 0 1 1 -9 -9 c 2.52 0 4.93 1 6.74 2.74 L 21 8 M 21 3 v 5 h -5",   // rotate-cw
  ["spinner"] = "M 21 12 a 9 9 0 1 1 -6.219 -8.56",   // loader-circle
  ["alert"] = "M 21.73 18 l -8 -14 a 2 2 0 0 0 -3.48 0 l -8 14 A 2 2 0 0 0 4 21 h 16 a 2 2 0 0 0 1.73 -3 M 12 9 v 4 M 12 17 h .01",   // triangle-alert
  ["link"] = "M 15 3 h 6 v 6 M 10 14 L 21 3 M 18 13 v 6 a 2 2 0 0 1 -2 2 H 5 a 2 2 0 0 1 -2 -2 V 8 a 2 2 0 0 1 2 -2 h 6",   // external-link
  ["user"] = "M 19 21 v -2 a 4 4 0 0 0 -4 -4 H 9 a 4 4 0 0 0 -4 4 v 2 M 8 7 A 4 4 0 1 0 16 7 A 4 4 0 1 0 8 7 Z",   // user
  ["gem"] = "M 10.5 3 L 8 9 l 4 13 l 4 -13 l -2.5 -6 M 17 3 a 2 2 0 0 1 1.6 .8 l 3 4 a 2 2 0 0 1 .013 2.382 l -7.99 10.986 a 2 2 0 0 1 -3.247 0 l -7.99 -10.986 A 2 2 0 0 1 2.4 7.8 l 2.998 -3.997 A 2 2 0 0 1 7 3 z M 2 9 h 20",   // gem
  ["play"] = "M 5 5 a 2 2 0 0 1 3.008 -1.728 l 11.997 6.998 a 2 2 0 0 1 .003 3.458 l -12 7 A 2 2 0 0 1 5 19 z",   // play
  ["github"] = "M 12 .297 c -6.63 0 -12 5.373 -12 12 c 0 5.303 3.438 9.8 8.205 11.385 c .6 .113 .82 -.258 .82 -.577 c 0 -.285 -.01 -1.04 -.015 -2.04 c -3.338 .724 -4.042 -1.61 -4.042 -1.61 C 4.422 18.07 3.633 17.7 3.633 17.7 c -1.087 -.744 .084 -.729 .084 -.729 c 1.205 .084 1.838 1.236 1.838 1.236 c 1.07 1.835 2.809 1.305 3.495 .998 c .108 -.776 .417 -1.305 .76 -1.605 c -2.665 -.3 -5.466 -1.332 -5.466 -5.93 c 0 -1.31 .465 -2.38 1.235 -3.22 c -.135 -.303 -.54 -1.523 .105 -3.176 c 0 0 1.005 -.322 3.3 1.23 c .96 -.267 1.98 -.399 3 -.405 c 1.02 .006 2.04 .138 3 .405 c 2.28 -1.552 3.285 -1.23 3.285 -1.23 c .645 1.653 .24 2.873 .12 3.176 c .765 .84 1.23 1.91 1.23 3.22 c 0 4.61 -2.805 5.625 -5.475 5.92 c .42 .36 .81 1.096 .81 2.22 c 0 1.606 -.015 2.896 -.015 3.286 c 0 .315 .21 .69 .825 .57 C 20.565 22.092 24 17.592 24 12.297 c 0 -6.627 -5.373 -12 -12 -12",   // GitHub logo (Simple Icons, CC0), filled
  ["discord"] = "M 20.317 4.3698 a 19.7913 19.7913 0 0 0 -4.8851 -1.5152 a .0741 .0741 0 0 0 -.0785 .0371 c -.211 .3753 -.4447 .8648 -.6083 1.2495 c -1.8447 -.2762 -3.68 -.2762 -5.4868 0 c -.1636 -.3933 -.4058 -.8742 -.6177 -1.2495 a .077 .077 0 0 0 -.0785 -.037 a 19.7363 19.7363 0 0 0 -4.8852 1.515 a .0699 .0699 0 0 0 -.0321 .0277 C .5334 9.0458 -.319 13.5799 .0992 18.0578 a .0824 .0824 0 0 0 .0312 .0561 c 2.0528 1.5076 4.0413 2.4228 5.9929 3.0294 a .0777 .0777 0 0 0 .0842 -.0276 c .4616 -.6304 .8731 -1.2952 1.226 -1.9942 a .076 .076 0 0 0 -.0416 -.1057 c -.6528 -.2476 -1.2743 -.5495 -1.8722 -.8923 a .077 .077 0 0 1 -.0076 -.1277 c .1258 -.0943 .2517 -.1923 .3718 -.2914 a .0743 .0743 0 0 1 .0776 -.0105 c 3.9278 1.7933 8.18 1.7933 12.0614 0 a .0739 .0739 0 0 1 .0785 .0095 c .1202 .099 .246 .1981 .3728 .2924 a .077 .077 0 0 1 -.0066 .1276 a 12.2986 12.2986 0 0 1 -1.873 .8914 a .0766 .0766 0 0 0 -.0407 .1067 c .3604 .698 .7719 1.3628 1.225 1.9932 a .076 .076 0 0 0 .0842 .0286 c 1.961 -.6067 3.9495 -1.5219 6.0023 -3.0294 a .077 .077 0 0 0 .0313 -.0552 c .5004 -5.177 -.8382 -9.6739 -3.5485 -13.6604 a .061 .061 0 0 0 -.0312 -.0286 z M 8.02 15.3312 c -1.1825 0 -2.1569 -1.0857 -2.1569 -2.419 c 0 -1.3332 .9555 -2.4189 2.157 -2.4189 c 1.2108 0 2.1757 1.0952 2.1568 2.419 c 0 1.3332 -.9555 2.4189 -2.1569 2.4189 z m 7.9748 0 c -1.1825 0 -2.1569 -1.0857 -2.1569 -2.419 c 0 -1.3332 .9554 -2.4189 2.1569 -2.4189 c 1.2108 0 2.1757 1.0952 2.1568 2.419 c 0 1.3332 -.946 2.4189 -2.1568 2.4189 Z",   // Discord logo (Simple Icons, CC0), filled
  ["folder"] = "M 20 20 a 2 2 0 0 0 2 -2 V 8 a 2 2 0 0 0 -2 -2 h -7.9 a 2 2 0 0 1 -1.69 -.9 L 9.6 3.9 A 2 2 0 0 0 7.93 3 H 4 a 2 2 0 0 0 -2 2 v 13 a 2 2 0 0 0 2 2 Z",   // folder
 };
 private static readonly HashSet<string> Brands = new() { "github", "discord" };
 private static Viewbox Icon(string kind, double size, SolidColorBrush brush)
 {
  var path = new Shapes.Path
  {
   Data = Geometry.Parse(IconPaths[kind]), Stroke = Brands.Contains(kind) ? null : brush, Fill = Brands.Contains(kind) ? brush : null, StrokeThickness = 2, StrokeStartLineCap = PenLineCap.Round,
   StrokeEndLineCap = PenLineCap.Round, StrokeLineJoin = PenLineJoin.Round,
  };
  var canvas = new Canvas { Width = 24, Height = 24 }; canvas.Children.Add(path);
  return new Viewbox { Width = size, Height = size, Child = canvas, VerticalAlignment = VerticalAlignment.Center };
 }

 // ---------------------------------------------------------------- animated backdrop (right)
 private void BuildBackdrop()
 {
  backdrop = At(new Backdrop { Width = PageWidth, Height = WindowHeight, Animated = !review }, SidebarWidth, 0);
  Root.Children.Add(backdrop);
 }

 // ---------------------------------------------------------------- sidebar
 private void BuildSidebar()
 {
  var layer = new Grid { Width = SidebarWidth, HorizontalAlignment = HorizontalAlignment.Left, Background = B("#08080b") };
  var brand = new StackPanel { Orientation = Orientation.Horizontal };
  brand.Children.Add(Picture("nickel-mark-1024.png", 26, 26));
  var name = T("Nickel", 17, SemiBold, "#ffffff"); name.Margin = new Thickness(10, 0, 0, 0); brand.Children.Add(name);
  var version = new Border { Margin = new Thickness(9, 0, 0, 0), Padding = new Thickness(7, 2, 7, 2), CornerRadius = new CornerRadius(0), Background = B("#23232c"), Child = T(Updater.Label, 10.5, Medium, "#9494a0"), VerticalAlignment = VerticalAlignment.Center };
  brand.Children.Add(version);
  layer.Children.Add(At(brand, 22, 26));
  layer.Children.Add(At(T("LAUNCHER", 10.5, Medium, "#60606a"), 24, 84));
  // active entry: a #14141B background that slides between entries
  layer.Children.Add(At(new Border { Width = SidebarWidth - 24, Height = 36, CornerRadius = new CornerRadius(0), Background = B("#14141b"), RenderTransform = highlightShift }, 12, 104));
  var y = 104.0;
  foreach (var (label, icon) in new[] { ("Home", "home"), ("Settings", "settings"), ("Credits", "credits") }) { layer.Children.Add(At(NavButton(label, icon), 12, y)); y += 40; }
  // status card at the bottom: whether Minecraft is running
  var status = new StackPanel();
  var line = new StackPanel { Orientation = Orientation.Horizontal };
  gameDot = B("#60606a");
  line.Children.Add(new Shapes.Ellipse { Width = 8, Height = 8, Fill = gameDot, VerticalAlignment = VerticalAlignment.Center });
  gameText = Wrapped(12.5, Medium, "#ffffff", 1.3); gameText.Margin = new Thickness(8, 0, 0, 0); gameText.Text = "Minecraft"; line.Children.Add(gameText);
  status.Children.Add(line);
  var sub = Wrapped(11.5, Regular, "#787884", 1.3); sub.Text = "Bedrock 1.26.62"; sub.Margin = new Thickness(16, 3, 0, 0); status.Children.Add(sub);
  layer.Children.Add(At(new Border { Width = SidebarWidth - 24, Padding = new Thickness(12, 11, 12, 11), CornerRadius = new CornerRadius(0), Background = B("#0f0f14"), Child = status }, 12, WindowHeight - 76));
  Panel.SetZIndex(layer, 1);
  Root.Children.Add(layer);
 }

 private Border NavButton(string name, string icon)
 {
  var surface = B("#ffffff00"); var iconBrush = B("#9494a0"); var labelBrush = B("#9494a0");
  var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(12, 0, 0, 0) };
  row.Children.Add(Icon(icon, 17, iconBrush));
  var text = T(name, 13.5, Medium, "#9494a0"); text.Foreground = labelBrush; text.Margin = new Thickness(11, 0, 0, 0); row.Children.Add(text);
  var item = new Border { Width = SidebarWidth - 24, Height = 36, CornerRadius = new CornerRadius(0), Background = surface, Child = row, Cursor = Cursors.Hand };
  navigation[name] = new NavItem(iconBrush, labelBrush);
  item.MouseEnter += (_, _) => { if (page != name) Fade(surface, "#0f0f14", .15); };
  item.MouseLeave += (_, _) => Fade(surface, "#0f0f1400", .15);
  OnClick(item, () => Navigate(name));
  return item;
 }

 // ---------------------------------------------------------------- pages
 private void BuildPages()
 {
  var host = new Grid { Width = PageWidth, Height = WindowHeight, ClipToBounds = true };
  At(host, SidebarWidth, 0);
  pages["Home"] = BuildHome(); pages["Settings"] = BuildSettings(); pages["Credits"] = BuildCredits();
  foreach (var name in PageNames) { pages[name].RenderTransform = new TranslateTransform(0, 10); pages[name].Opacity = 0; host.Children.Add(pages[name]); }
  Root.Children.Add(host);
 }

 private void Navigate(string name, bool animate = true)
 {
  if (page == name) return;
  page = name;
  foreach (var key in PageNames)
  {
   var active = key == name; var element = pages[key]; var shift = (TranslateTransform)element.RenderTransform;
   element.IsHitTestVisible = active;
   var seconds = !animate ? 0 : active ? .3 : .16;
   Slide(element, OpacityProperty, active ? 1 : 0, seconds, active ? CubicOut : QuadraticIn, active ? .1 : 0);
   Slide(shift, TranslateTransform.YProperty, active ? 0 : 10, seconds, active ? CubicOut : QuadraticIn, active ? .1 : 0);
   var item = navigation[key];
   Fade(item.Label, active ? "#ffffff" : "#9494a0", animate ? .2 : 0);
   Fade(item.Icon, active ? Accent : "#9494a0", animate ? .2 : 0);
  }
  Slide(highlightShift, TranslateTransform.YProperty, Array.IndexOf(PageNames, name) * 40, animate ? .3 : 0);
 }

 // --review-motion: review window with live animations; navigation is played at fixed times.
 private bool motion;
 private bool Still => review && !motion;
 internal async Task PlayMotionAsync()
 {
  motion = true;
  await Task.Delay(1500);
  foreach (var name in new[] { "Settings", "Credits", "Home" }) { await Task.Delay(1000); Navigate(name); }
  await Task.Delay(1000);
 }

 /// <summary>Header over the backdrop: breadcrumb, large title, subtitle with one highlighted word.</summary>
 private Grid Header(string crumb, string title, string lead, string mark, out TextBlock markText)
 {
  var grid = new Grid();
  grid.Children.Add(At(T("Nickel  /  " + crumb, 12, Regular, "#ffffffbf"), 32, 28));
  grid.Children.Add(At(T(title, 34, SemiBold, "#ffffff"), 30, 44));
  var line = new StackPanel { Orientation = Orientation.Horizontal };
  var leadText = Wrapped(14, Regular, "#ffffffd9", 1.3); leadText.Text = lead; line.Children.Add(leadText);
  markText = Wrapped(14, SemiBold, AccentDeep, 1.3); markText.Text = mark;
  highlightInk ??= (SolidColorBrush)markText.Foreground; markText.Foreground = highlightInk;
  line.Children.Add(new Border { Background = Brushes.White, Padding = new Thickness(2, 0, 2, 0), Margin = new Thickness(4, 0, 0, 0), Child = markText });
  grid.Children.Add(At(line, 32, 92));
  return grid;
 }
 private static Border Panel_(double top, UIElement child) => new()
 {
  Margin = new Thickness(16, top, 16, 16), CornerRadius = new CornerRadius(0), Background = B("#0f0f14f8"), BorderBrush = B("#1a1a21"), BorderThickness = new Thickness(1), Child = child,
 };

 private Grid BuildHome()
 {
  var home = new Grid();
  home.Children.Add(Header("Home", "Nickel Client", "Minecraft Bedrock 1.26.62  ·", "Ready", out statusText));
  // floating panel: symbol, title and description on the left, Load button on the right; separator; two notes
  var panel = new Grid { Margin = new Thickness(22, 20, 22, 20) };
  panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); panel.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
  var top = new DockPanel();
  var chip = new Border { Width = 48, Height = 48, CornerRadius = new CornerRadius(0), Background = B("#17171d"), Child = Picture("nickel-mark-1024.png", 26, 26) };
  hero = chip;
  DockPanel.SetDock(chip, Dock.Left); top.Children.Add(chip);
  load = new LoadButton();
  load.Press = new TranslateTransform();
  load.Back = B("#ffffff"); load.Outline = B("#ffffff00"); load.Label = B("#0d0d0f");
  load.Surface = new Grid { Width = 168, Height = 44, Clip = Frozen(new RectangleGeometry(new Rect(0, 0, 168, 44))), RenderTransform = load.Press, Cursor = Cursors.Hand };
  load.Surface.Children.Add(new Shapes.Rectangle { Fill = load.Back });
  load.Fill = new Shapes.Rectangle { Fill = B(Accent), HorizontalAlignment = HorizontalAlignment.Left, Width = 0, Opacity = .35 };
  load.Surface.Children.Add(load.Fill);
  load.Content = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
  load.Surface.Children.Add(load.Content);
  load.Surface.Children.Add(new Border { CornerRadius = new CornerRadius(0), BorderThickness = new Thickness(1.5), BorderBrush = load.Outline });
  load.Glow = new Border { Width = 168, Height = 44, CornerRadius = new CornerRadius(0), Effect = new System.Windows.Media.Effects.DropShadowEffect { Color = Colors.White, BlurRadius = 22, ShadowDepth = 0, Opacity = .35 }, Background = B("#ffffff01"), IsHitTestVisible = false };
  var button = new Grid { VerticalAlignment = VerticalAlignment.Center }; button.Children.Add(load.Glow); button.Children.Add(load.Surface);
  load.Surface.MouseEnter += (_, _) => { if (LoadIsClickable) Fade(load.Back, minecraftRunning ? "#ebebf0" : "#2d2d39", .15); };
  load.Surface.MouseLeave += (_, _) => { Slide(load.Press, TranslateTransform.YProperty, 0, .2); Refresh(); };
  load.Surface.MouseLeftButtonDown += (_, e) => { e.Handled = true; if (LoadIsClickable) Slide(load.Press, TranslateTransform.YProperty, 1, .15); };
  load.Surface.MouseLeftButtonUp += (_, e) => { e.Handled = true; Slide(load.Press, TranslateTransform.YProperty, 0, .2); if (LoadIsClickable) Primary_Click(); };
  DockPanel.SetDock(button, Dock.Right); top.Children.Add(button);
  var words = new StackPanel { Margin = new Thickness(14, 0, 14, 0), VerticalAlignment = VerticalAlignment.Center };
  words.Children.Add(T("Load into Minecraft", 15.5, SemiBold, "#ffffff"));
  var describe = Wrapped(12.5, Regular, "#9494a0", 1.35); describe.Margin = new Thickness(0, 3, 0, 0); loadDescription = describe; ShowMenuKey();
  words.Children.Add(describe);
  top.Children.Add(words);
  panel.Children.Add(top);
  var rule = new Border { Height = 1, Background = B("#1a1a21"), Margin = new Thickness(0, 18, 0, 16) }; Grid.SetRow(rule, 1); panel.Children.Add(rule);
  var facts = Facts(("GAME", "Minecraft Bedrock"), ("VERSION", "1.26.62"), ("MENU KEY", SharedPreferences.MenuKey.Name)); Grid.SetRow(facts, 2); panel.Children.Add(facts);
  // Menu key: click, then press the key that opens Nickel in game (Escape cancels, Backspace goes back to Right Shift).
  if (facts.Children[2] is StackPanel keyFact)
  {
   var keyValue = Wrapped(13.5, Medium, "#ffffff", 1.2); keyValue.Margin = new Thickness(0, 4, 0, 0); keyFact.Children.RemoveAt(1); keyFact.Children.Add(keyValue);
   menuKeyValue = keyValue; keyFact.Cursor = Cursors.Hand; keyFact.Background = Brushes.Transparent; keyFact.ToolTip = "Click to change the key that opens the menu";
   keyValue.Text = SharedPreferences.MenuKey.Name + "  ·  change";
   keyFact.MouseLeftButtonUp += (_, e) => { e.Handled = true; listeningKey = true; keyValue.Text = "Press a key…"; keyValue.Foreground = B(Accent); Focus(); };
  }
  var floating = Panel_(0, panel); floating.VerticalAlignment = VerticalAlignment.Bottom;
  home.Children.Add(floating);
  (homeNotice, homeNoticeText) = Notice();
  homeNotice.Margin = new Thickness(16, 0, 16, 196); homeNotice.VerticalAlignment = VerticalAlignment.Bottom; homeNotice.Visibility = Visibility.Collapsed;
  home.Children.Add(homeNotice);
  return home;
 }

 private (Border, TextBlock) Notice()
 {
  var text = Wrapped(12.5, Medium, "#f87171", 1.45);
  var icon = Icon("alert", 16, B("#f87171")); icon.VerticalAlignment = VerticalAlignment.Top; icon.Margin = new Thickness(0, 1, 10, 0);
  var row = new DockPanel(); DockPanel.SetDock(icon, Dock.Left); row.Children.Add(icon); row.Children.Add(text);
  return (new Border { Padding = new Thickness(12, 10, 12, 10), CornerRadius = new CornerRadius(0), Background = B("#2a1518"), Child = row }, text);
 }

 private static Grid Facts(params (string Term, string Value)[] items)
 {
  var facts = new Grid(); var slot = 0;
  foreach (var (term, value) in items)
  {
   facts.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
   var fact = new StackPanel();
   fact.Children.Add(T(term, 10.5, Medium, "#60606a"));
   var v = T(value, 13.5, Medium, "#ffffff"); v.Margin = new Thickness(0, 4, 0, 0); fact.Children.Add(v);
   Grid.SetColumn(fact, slot); facts.Children.Add(fact); slot++;
  }
  return facts;
 }

 // list: numbered label, then rows separated by hairlines
 private int sectionNumber;
 private FrameworkElement Section(string label)
 {
  var row = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(18, sectionNumber == 0 ? 16 : 18, 18, 6) };
  var number = T($"{++sectionNumber:00} ·", 11, SemiBold, Accent); row.Children.Add(number);
  var text = T(label.ToUpperInvariant(), 11, Medium, "#787884"); text.Margin = new Thickness(6, 0, 0, 0); row.Children.Add(text);
  return row;
 }
 private static Border Row(string title, string description, FrameworkElement? control, bool last = false)
 {
  var labels = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
  labels.Children.Add(T(title, 14, Medium, "#ffffff"));
  var d = T(description, 12, Regular, "#9494a0"); d.Margin = new Thickness(0, 2, 0, 0); labels.Children.Add(d);
  var dock = new DockPanel();
  if (control != null) { DockPanel.SetDock(control, Dock.Right); control.Margin = new Thickness(14, 0, 0, 0); dock.Children.Add(control); }
  dock.Children.Add(labels);
  var surface = B("#14141b00");
  var row = new Border { MinHeight = 56, Padding = new Thickness(14, 9, 14, 9), Margin = new Thickness(6, 0, 6, 0), CornerRadius = new CornerRadius(0), Background = surface, Child = dock };
  row.MouseEnter += (_, _) => surface.BeginAnimation(SolidColorBrush.ColorProperty, new ColorAnimation(C("#14141b"), TimeSpan.FromSeconds(.15)));
  row.MouseLeave += (_, _) => surface.BeginAnimation(SolidColorBrush.ColorProperty, new ColorAnimation(C("#14141b00"), TimeSpan.FromSeconds(.15)));
  var holder = new StackPanel(); holder.Children.Add(row);
  if (!last) holder.Children.Add(new Border { Height = 1, Background = B("#1a1a21"), Margin = new Thickness(20, 0, 20, 0) });
  return new Border { Child = holder };
 }

 private FrameworkElement BuildSettings()
 {
  var page = new Grid();
  page.Children.Add(Header("Settings", "Settings", "Loading, privacy and", "theme", out _));
  var list = new StackPanel();
  sectionNumber = 0;
  list.Children.Add(Section("Loading"));
  list.Children.Add(Row("Automatic load", "Load Nickel as soon as Minecraft opens.", Switch(preferences.AutomaticLoad, value => { preferences.AutomaticLoad = value; Save(); return value; })));
  list.Children.Add(Row("Close after loading", "Close the launcher once Nickel is loaded.", Switch(preferences.CloseAfterLoading, value => { preferences.CloseAfterLoading = value; Save(); return value; })));
  list.Children.Add(Row("Start with Windows", "Open the launcher when you sign in to Windows.", Switch(Startup.Enabled, value => { if (review) return value; var ok = Startup.Set(value); if (!ok) ShowSettingsError("Windows refused to change the startup entry."); return Startup.Enabled; }), true));
  list.Children.Add(Section("Look"));
  list.Children.Add(Row("Theme", "Colors and animated background of the launcher.", ThemePicker(), true));
  list.Children.Add(Section("Privacy"));
  list.Children.Add(Row("Discord Presence", "Show Nickel on your Discord profile: in the launcher, then in game.", Switch(preferences.DiscordPresence, value => { preferences.DiscordPresence = value; LauncherPresence.SetEnabled(value); Save(); return value; })));
  list.Children.Add(Row("Usage statistics", "Send an anonymous ping so nickelclient.com can count players. No name, account or hardware data.", Switch(preferences.UsageStatistics, value => { preferences.UsageStatistics = value; UsagePing.SetEnabled(value, preferences.InstallId); Save(); return value; })));
  list.Children.Add(Row("Stream Hide", "Hide the launcher and the in-game menu from screenshots, recordings and screen sharing.", Switch(preferences.StreamHide, value => { preferences.StreamHide = value; ApplyStreamHide(); Save(); return preferences.StreamHide; }), true));
  (settingsNotice, settingsNoticeText) = Notice(); settingsNotice.Visibility = Visibility.Collapsed; settingsNotice.Margin = new Thickness(18, 8, 18, 4);
  list.Children.Add(settingsNotice);
  list.Children.Add(Section("About"));
  var actions = new StackPanel { Orientation = Orientation.Horizontal };
  actions.Children.Add(UpdateButton());
  var data = ActionButton("Data folder", "folder", () => { var folder = System.IO.Path.Combine(AppContext.BaseDirectory, "data"); Directory.CreateDirectory(folder); OpenLink(folder); }); data.Margin = new Thickness(8, 0, 0, 0); actions.Children.Add(data);
  list.Children.Add(Row($"Launcher {Updater.Version}", "Updates come from nickelclient.com.", actions));
  var about = new StackPanel { Orientation = Orientation.Horizontal };
  about.Children.Add(ActionButton("GitHub", "github", () => OpenLink(GitHubUrl))); var join = ActionButton("Discord", "discord", () => OpenLink(DiscordUrl)); join.Margin = new Thickness(8, 0, 0, 0); about.Children.Add(join);
  list.Children.Add(Row("Community", "Source code, news and help.", about, true));
  var scroll = new ScrollViewer { Content = list, VerticalScrollBarVisibility = ScrollBarVisibility.Hidden, Padding = new Thickness(0, 0, 0, 10) };
  page.Children.Add(Panel_(132, scroll));
  return page;
 }

 private FrameworkElement ThemePicker()
 {
  var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
  for (var i = 0; i < Themes.All.Length; i++)
  {
   var index = i; var t = Themes.All[i];
   var fill = new LinearGradientBrush(Themes.Parse(t.Glow), Themes.Parse(t.Deep), 90);
   var swatch = new Border { Width = 22, Height = 22, CornerRadius = new CornerRadius(0), Background = fill, Margin = new Thickness(i == 0 ? 0 : 7, 0, 0, 0), BorderThickness = new Thickness(2), Cursor = Cursors.Hand, ToolTip = t.Name };
   swatches.Add(swatch);
   OnClick(swatch, () => { Themes.Index = index; preferences.Theme = index; Save(); ApplyTheme(); });
   row.Children.Add(swatch);
  }
  PaintSwatches();
  return row;
 }
 private void PaintSwatches() { for (var i = 0; i < swatches.Count; i++) swatches[i].BorderBrush = i == Themes.Index ? Brushes.White : Brushes.Transparent; }
 private void ApplyTheme()
 {
  PaintSwatches();
  foreach (var key in PageNames) Fade(navigation[key].Icon, key == page ? Accent : "#9494a0", .3);
  foreach (var (track, on) in switches) Fade(track, on() ? AccentDeep : "#2d2d39", .3);
  if (highlightInk != null) Fade(highlightInk, AccentDeep, .3);
  Fade((SolidColorBrush)load.Fill.Fill, Accent, .3);
 }

 private FrameworkElement BuildCredits()
 {
  var page = new Grid();
  page.Children.Add(Header("Credits", "Credits", "Join us on", "Discord", out _));
  var list = new StackPanel();
  sectionNumber = 0;
  list.Children.Add(Section("Project"));
  list.Children.Add(Row("Nickel Client", "Launcher and in-game menu for Minecraft Bedrock.", null, true));
  list.Children.Add(Section("Made by"));
  list.Children.Add(Row("Created by", "crossthepath", ActionButton("GitHub", "github", () => OpenLink(GitHubUrl))));
  list.Children.Add(Row("Contact", "Questions, bugs and ideas on the Discord server.", ActionButton("Discord", "discord", () => OpenLink(DiscordUrl)), true));
  page.Children.Add(Panel_(132, new ScrollViewer { Content = list, VerticalScrollBarVisibility = ScrollBarVisibility.Hidden }));
  return page;
 }
 private Border UpdateButton()
 {
  var surface = B("#14141b00"); var label = Wrapped(12.5, Medium, "#ffffff", 1.2); label.Text = "Check for updates"; label.VerticalAlignment = VerticalAlignment.Center;
  var glyph = Icon("retry", 14, B("#ffffff")); glyph.Margin = new Thickness(0, 0, 7, 0);
  var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center }; row.Children.Add(glyph); row.Children.Add(label);
  var button = new Border { Height = 32, Padding = new Thickness(11, 0, 12, 0), CornerRadius = new CornerRadius(0), BorderBrush = B("#2d2d39"), BorderThickness = new Thickness(1), Background = surface, Child = row, Cursor = Cursors.Hand, VerticalAlignment = VerticalAlignment.Center };
  button.MouseEnter += (_, _) => Fade(surface, "#14141b", .15);
  button.MouseLeave += (_, _) => Fade(surface, "#14141b00", .15);
  Updater.Release? found = null; var working = false; var installed = false;
  OnClick(button, async () =>
  {
   if (working || review) return; working = true; ShowSettingsError("");
   try
   {
    // 1: check; 2: "Update to vX" downloads and replaces the launcher; 3: "Restart to finish" starts the new version.
    if (installed) { Updater.Restart(); Close(); return; }
    if (found == null)
    {
     label.Text = "Checking...";
     found = await Updater.LatestAsync(lifetime.Token);
     if (found == null) { label.Text = $"Up to date ({Updater.Version})"; return; }
     label.Text = $"Update to {found.Tag.TrimStart('v', 'V')}"; button.BorderBrush = B(Accent); return;
    }
    label.Text = "Downloading...";
    await Updater.InstallAsync(found, percent => Dispatcher.Invoke(() => label.Text = $"Downloading {percent} %"), lifetime.Token, start: false);
    installed = true; label.Text = "Restart to finish"; button.BorderBrush = B(Accent);
   }
   catch (OperationCanceledException) { }
   catch (Exception error) { label.Text = "Check for updates"; found = null; button.BorderBrush = B("#2d2d39"); ShowSettingsError(error.Message); }
   finally { working = false; }
  });
  return button;
 }
 /// <summary>Secondary button: #2D2D39 outline, square, stroke icon, #14141B on hover.</summary>
 private Border ActionButton(string label, string icon, Action action)
 {
  var surface = B("#14141b00");
  var row = new StackPanel { Orientation = Orientation.Horizontal, VerticalAlignment = VerticalAlignment.Center };
  var glyph = Icon(icon, 14, B("#ffffff")); glyph.Margin = new Thickness(0, 0, 7, 0); row.Children.Add(glyph);
  row.Children.Add(T(label, 12.5, Medium, "#ffffff"));
  var button = new Border { Height = 32, Padding = new Thickness(11, 0, 12, 0), CornerRadius = new CornerRadius(0), BorderBrush = B("#2d2d39"), BorderThickness = new Thickness(1), Background = surface, Child = row, Cursor = Cursors.Hand, VerticalAlignment = VerticalAlignment.Center };
  button.MouseEnter += (_, _) => Fade(surface, "#14141b", .15);
  button.MouseLeave += (_, _) => Fade(surface, "#14141b00", .15);
  OnClick(button, action);
  return button;
 }

 private FrameworkElement Switch(bool value, Func<bool, bool> changed)
 {
  var on = value; var track = B(on ? AccentDeep : "#2d2d39"); var shift = new TranslateTransform(on ? 16 : 0, 0);
  var knob = At(new Shapes.Rectangle { Width = 14, Height = 14, Fill = B("#f5f5f8"), RenderTransform = shift }, 2, 2);
  var control = new Border { Width = 34, Height = 18, CornerRadius = new CornerRadius(0), Background = track, Child = knob, Cursor = Cursors.Hand, VerticalAlignment = VerticalAlignment.Center };
  switches.Add((track, () => on));
  OnClick(control, () => { on = changed(!on); Fade(track, on ? AccentDeep : "#2d2d39", .2); Slide(shift, TranslateTransform.XProperty, on ? 16 : 0, .2); });
  return control;
 }

 // ---------------------------------------------------------------- window
 private void BuildWindowControls()
 {
  var controls = new StackPanel { Orientation = Orientation.Horizontal, HorizontalAlignment = HorizontalAlignment.Right, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 10, 10, 0) };
  controls.Children.Add(WindowButton("minimize", "#ffffff26", () => WindowState = WindowState.Minimized));
  var close = WindowButton("close", "#ef5252", Close); close.Margin = new Thickness(6, 0, 0, 0); controls.Children.Add(close);
  Panel.SetZIndex(controls, 3);
  Root.Children.Add(controls);
 }
 private Border WindowButton(string icon, string hoverSurface, Action action)
 {
  var surface = B("#0f0f1499"); var brush = B("#ffffffcc");
  var button = new Border { Width = 32, Height = 32, CornerRadius = new CornerRadius(0), Background = surface, Child = Icon(icon, 15, brush), Cursor = Cursors.Hand };
  ((FrameworkElement)button.Child).HorizontalAlignment = HorizontalAlignment.Center;
  button.MouseEnter += (_, _) => { Fade(surface, hoverSurface, .15); Fade(brush, "#ffffff", .15); };
  button.MouseLeave += (_, _) => { Fade(surface, "#0f0f1499", .15); Fade(brush, "#ffffffcc", .15); };
  OnClick(button, action);
  return button;
 }
 private void ApplyWindowFrame()
 {
  var handle = new WindowInteropHelper(this).Handle; var round = 1;   // DWMWCP_DONOTROUND: square window corners like the in-game menu
  if (handle != IntPtr.Zero) DwmSetWindowAttribute(handle, 33, ref round, sizeof(int));
 }
 private static bool tooltipHook;
 private void ApplyStreamHide()
 {
  NativeLoader.StreamHide = preferences.StreamHide;
  if (!tooltipHook)
  {
   tooltipHook = true;
   // WPF tooltips are separate windows: give them the same capture exclusion
   EventManager.RegisterClassHandler(typeof(System.Windows.Controls.ToolTip), System.Windows.Controls.ToolTip.OpenedEvent, new RoutedEventHandler((sender, _) =>
   {
    if (!preferences.StreamHide || PresentationSource.FromVisual((Visual)sender) is not HwndSource source) return;
    var popup = source.Handle; while (GetParent(popup) != IntPtr.Zero) popup = GetParent(popup);
    SetWindowDisplayAffinity(popup, 0x11u);
   }));
  }
  var handle = new WindowInteropHelper(this).Handle;
  if (handle == IntPtr.Zero || SetWindowDisplayAffinity(handle, preferences.StreamHide ? 0x11u : 0u)) { ShowSettingsError(""); return; }
  preferences.StreamHide = false; ShowSettingsError("Stream Hide is unavailable on this Windows session.");
 }
 private void ShowSettingsError(string text) { if (settingsNotice == null) return; settingsNoticeText.Text = text; settingsNotice.Visibility = text == "" ? Visibility.Collapsed : Visibility.Visible; }
 private void Save() { if (review) return; try { preferences.Save(); } catch { ShowSettingsError("Settings could not be saved in the application folder."); } }

 // ---------------------------------------------------------------- loading
 private bool LoadIsClickable => !loaded && !busy && connection == null && (minecraftRunning || !review);
 private bool minecraftRunning = true;
 private FrameworkElement hero = null!;
 private bool IdleMode => !minecraftRunning && !loaded && !busy && connection == null;
 private static bool MinecraftIsRunning()
 {
  var processes = Process.GetProcessesByName("Minecraft.Windows");
  try { return processes.Length > 0; } finally { foreach (var process in processes) process.Dispose(); }
 }
 private void Primary_Click()
 {
  if (!minecraftRunning) { OpenLink("minecraft:"); statusText.Text = "Starting Minecraft"; return; }   // the monitor switches back to "Load" once the game is running
  Load_Click(this, new RoutedEventArgs());
 }
 private void Refresh()
 {
  var waiting = busy || connection != null;
  LauncherPresence.SetDetails(loaded ? null : "In the launcher");   // once Nickel is loaded the game shows its own activity
  var idle = IdleMode;
  var (icon, label) = loaded ? ("check", "Loaded") : waiting ? ("spinner", "Loading") : idle ? ("play", "Start Minecraft") : failed ? ("retry", "Retry") : ("play", "Load");
  Slide(hero, OpacityProperty, minecraftRunning || loaded ? 1 : .5, .3);
  if (statusText != null) statusText.Text = loaded ? "Loaded" : waiting ? "Loading" : idle ? "Start Minecraft" : failed ? "Load failed" : "Ready";
  if (gameDot != null) { Fade(gameDot, loaded ? "#34d399" : minecraftRunning ? Accent : "#60606a", .25); gameText.Text = minecraftRunning ? "Minecraft running" : "Minecraft closed"; }
  load.Content.Children.Clear();
  Fade(load.Label, loaded ? "#34d399" : idle ? "#ffffff" : "#0d0d0f", .25);
  Fade(load.Back, loaded ? "#0f2a20" : idle ? (load.Surface.IsMouseOver ? "#2d2d39" : "#23232c") : load.Surface.IsMouseOver && LoadIsClickable ? "#ebebf0" : "#ffffff", .25);
  Fade(load.Outline, loaded ? "#34d39966" : "#ffffff00", .25);
  Slide(load.Glow, OpacityProperty, loaded || idle ? 0 : 1, .3);
  Slide(load.Fill, WidthProperty, loaded ? 0 : connection != null ? 118 : busy ? 60 : 0, .35);
  if (icon != "")
  {
   var glyph = Icon(icon, 16, loaded ? B("#34d399") : idle ? B("#ffffff") : B("#0d0d0f")); glyph.Margin = new Thickness(0, 0, 8, 0);
   if (icon == "spinner" && !Still)
   {
    var spin = new RotateTransform(0, 8, 8); glyph.RenderTransform = spin;
    spin.BeginAnimation(RotateTransform.AngleProperty, new DoubleAnimation(0, 360, TimeSpan.FromSeconds(.8)) { RepeatBehavior = RepeatBehavior.Forever });
   }
   load.Content.Children.Add(glyph);
  }
  var text = T(label, 14.5, SemiBold, "#0d0d0f"); text.Foreground = load.Label; load.Content.Children.Add(text);
  load.Surface.Cursor = LoadIsClickable ? Cursors.Hand : Cursors.Arrow;
  load.Surface.ToolTip = loaded && message != "" ? message : null;
  var error = failed && !waiting && !loaded ? message : "";
  homeNoticeText.Text = error; homeNotice.Visibility = error == "" ? Visibility.Collapsed : Visibility.Visible;
 }
 private void FinishLoaded() { loaded = true; failed = false; message = $"Press {SharedPreferences.MenuKey.Name} in Minecraft to open the menu."; Refresh(); if (preferences.CloseAfterLoading) Close(); }
 private TextBlock? menuKeyValue, loadDescription; private bool listeningKey;
 private void ShowMenuKey() { if (loadDescription != null) loadDescription.Text = $"Injects Nickel into the running game. Press {SharedPreferences.MenuKey.Name} in game to open the menu."; }
 protected override void OnPreviewKeyDown(KeyEventArgs e)
 {
  if (!listeningKey || menuKeyValue == null) { base.OnPreviewKeyDown(e); return; }
  e.Handled = true; listeningKey = false;
  var key = e.Key == Key.System ? e.SystemKey : e.Key;
  if (key == Key.Back || key == Key.Delete) SharedPreferences.MenuKey = ("Right Shift", SharedPreferences.RightShift);
  else if (key != Key.Escape)
  {
   var picked = SharedPreferences.Describe(key);
   if (picked.Code > 0) SharedPreferences.MenuKey = picked;
  }
  menuKeyValue.Text = SharedPreferences.MenuKey.Name + "  ·  change"; menuKeyValue.Foreground = B("#ffffff"); ShowMenuKey();
 }
 private async void Load_Click(object sender, RoutedEventArgs e)
 {
  if (busy || connection != null) return;
  busy = true; failed = false; message = ""; Refresh(); NativeLoadResult? result = null;
  try
  {
   result = await NativeLoader.LoadAsync(lifetime.Token);
   if (closed) { result.Connection.Dispose(); return; }
   connection = result.Connection;
   if (result.RendererReady) FinishLoaded();
  }
  catch (OperationCanceledException) { }
  catch (Exception error) { if (!closed) { failed = true; message = error.Message; } }
  finally { busy = false; if (!closed) Refresh(); }
 }
 /// <summary>Freezes the animated backdrop between WM_ENTERSIZEMOVE and WM_EXITSIZEMOVE so dragging the window is smooth.</summary>
 private static IntPtr MoveHook(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
 {
  if (message == 0x0231) Backdrop.Frozen = true;
  else if (message == 0x0232) Backdrop.Frozen = false;
  return IntPtr.Zero;
 }

 private bool polling;
 /// <summary>Looks for Minecraft on a worker thread once per second, then updates the window on the UI thread.</summary>
 private async void Monitor_Tick(object? sender, EventArgs e)
 {
  if (polling) return;
  polling = true;
  bool running; int single;
  try
  {
   (running, single) = await Task.Run(() =>
   {
    var processes = Process.GetProcessesByName("Minecraft.Windows");
    try { return (processes.Length > 0, processes.Length == 1 ? processes[0].Id : 0); }
    finally { foreach (var process in processes) process.Dispose(); }
   });
  }
  finally { polling = false; }
  if (closed) return;
  if (running != minecraftRunning) { minecraftRunning = running; if (running && failed && connection == null) { failed = false; message = ""; } Refresh(); }
  if (connection != null && !connection.IsAlive) { connection.Dispose(); connection = null; loaded = false; failed = true; message = "Minecraft closed. Start it again to load Nickel."; Refresh(); }
  if (connection != null && !loaded && NativeLoader.WaitForRenderer(connection.Pid, 0)) FinishLoaded();
  if (preferences.AutomaticLoad && !busy && connection == null)
  {
   if (single != 0 && single != autoAttemptPid) { autoAttemptPid = single; Load_Click(this, new RoutedEventArgs()); }
  }
 }

 // ---------------------------------------------------------------- review export
 /// <summary>Exports the pages from the live window (no monitor, no loading, no preferences saved).</summary>
 internal async Task ExportReviewAsync(string directory)
 {
  Directory.CreateDirectory(directory);
  var handle = new WindowInteropHelper(this).Handle;
  var dpi = VisualTreeHelper.GetDpi(this);
  File.WriteAllLines(System.IO.Path.Combine(directory, "fonts.txt"), new[] { Regular, Medium, SemiBold, Bold }.Select(weight => $"{weight.ToOpenTypeWeight()} -> {RmlText.Face(weight).FontUri}"));
  var reviewTheme = Environment.GetEnvironmentVariable("NICKEL_REVIEW_THEME");
  if (int.TryParse(reviewTheme, out var themeIndex)) { Themes.Index = Math.Clamp(themeIndex, 0, Themes.All.Length - 1); ApplyTheme(); }
  foreach (var name in PageNames.Append("Home-open"))
  {
   minecraftRunning = name != "Home-open"; Refresh();
   Navigate(name == "Home-open" ? "Home" : name, false);
   Root.UpdateLayout();
   await Dispatcher.InvokeAsync(() => { }, DispatcherPriority.ApplicationIdle);
   await Task.Delay(400);
   var file = System.IO.Path.Combine(directory, name.ToLowerInvariant());
   var bitmap = new RenderTargetBitmap((int)Math.Round(Root.ActualWidth * dpi.DpiScaleX), (int)Math.Round(Root.ActualHeight * dpi.DpiScaleY), dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
   bitmap.Render(Root);
   SavePng(bitmap, file + ".png");
   var capture = CaptureWindow(handle);
   if (capture != null) SavePng(capture, file + "-window.png");
  }
 }
 private static void SavePng(BitmapSource source, string path)
 {
  var encoder = new PngBitmapEncoder(); encoder.Frames.Add(BitmapFrame.Create(source));
  using var stream = File.Create(path); encoder.Save(stream);
 }
 private static BitmapSource? CaptureWindow(IntPtr handle)
 {
  if (handle == IntPtr.Zero || !GetWindowRect(handle, out var bounds)) return null;
  int width = bounds.Right - bounds.Left, height = bounds.Bottom - bounds.Top;
  var screen = GetDC(IntPtr.Zero); var memory = CreateCompatibleDC(screen); var surface = CreateCompatibleBitmap(screen, width, height);
  var previous = SelectObject(memory, surface);
  try
  {
   if (!PrintWindow(handle, memory, 2)) return null;
   SelectObject(memory, previous);
   var source = Imaging.CreateBitmapSourceFromHBitmap(surface, IntPtr.Zero, Int32Rect.Empty, BitmapSizeOptions.FromEmptyOptions());
   source.Freeze(); return source;
  }
  finally { SelectObject(memory, previous); DeleteObject(surface); DeleteDC(memory); ReleaseDC(IntPtr.Zero, screen); }
 }

 [StructLayout(LayoutKind.Sequential)] private struct NativeRect { public int Left, Top, Right, Bottom; }
 [DllImport("user32.dll", SetLastError = true)] private static extern bool SetWindowDisplayAffinity(IntPtr hwnd, uint affinity);
 [DllImport("user32.dll")] private static extern IntPtr GetParent(IntPtr hwnd);
 [DllImport("dwmapi.dll")] private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
 [DllImport("user32.dll")] private static extern bool GetWindowRect(IntPtr hwnd, out NativeRect bounds);
 [DllImport("user32.dll")] private static extern bool PrintWindow(IntPtr hwnd, IntPtr dc, uint flags);
 [DllImport("user32.dll")] private static extern IntPtr GetDC(IntPtr hwnd);
 [DllImport("user32.dll")] private static extern int ReleaseDC(IntPtr hwnd, IntPtr dc);
 [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleDC(IntPtr dc);
 [DllImport("gdi32.dll")] private static extern IntPtr CreateCompatibleBitmap(IntPtr dc, int width, int height);
 [DllImport("gdi32.dll")] private static extern IntPtr SelectObject(IntPtr dc, IntPtr value);
 [DllImport("gdi32.dll")] private static extern bool DeleteObject(IntPtr value);
 [DllImport("gdi32.dll")] private static extern bool DeleteDC(IntPtr dc);
}
