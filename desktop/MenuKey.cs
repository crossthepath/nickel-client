using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Input;
namespace Nickel.Desktop;

/// <summary>
/// Nickel's shared preferences file, %APPDATA%\Nickel\module-preferences.ini (the overlay reads and writes it too), and
/// the menu key stored in it ([menu] menu-key / menu-key-code, Right Shift by default). The first time, the preferences
/// and configs of the newest data\runtime\&lt;hash&gt; folder (older layout) are copied there.
/// </summary>
public static class SharedPreferences
{
 public const int RightShift = 0xA1;
 public static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "Nickel");
 public static string File
 {
  get
  {
   var folder = Folder; Directory.CreateDirectory(folder);
   var target = Path.Combine(folder, "module-preferences.ini");
   if (!System.IO.File.Exists(target)) Migrate(target);
   return target;
  }
 }
 private static void Migrate(string target)
 {
  try
  {
   var runtime = Path.Combine(AppContext.BaseDirectory, "data", "runtime");
   if (!Directory.Exists(runtime)) return;
   var newest = Directory.GetDirectories(runtime).Select(d => Path.Combine(d, "module-preferences.ini")).Where(System.IO.File.Exists)
    .OrderByDescending(System.IO.File.GetLastWriteTimeUtc).FirstOrDefault();
   if (newest == null) return;
   System.IO.File.Copy(newest, target, false);
   var configs = Path.Combine(Path.GetDirectoryName(newest)!, "configs");
   if (Directory.Exists(configs))
   {
    var to = Path.Combine(Folder, "configs"); Directory.CreateDirectory(to);
    foreach (var file in Directory.GetFiles(configs)) { var dest = Path.Combine(to, Path.GetFileName(file)); if (!System.IO.File.Exists(dest)) System.IO.File.Copy(file, dest); }
   }
  }
  catch { }
 }
 public static void Write(string section, string key, string value) => WritePrivateProfileString(section, key, value, File);
 public static string Read(string section, string key, string fallback)
 {
  var text = new StringBuilder(256); GetPrivateProfileString(section, key, fallback, text, (uint)text.Capacity, File); return text.ToString();
 }

 /// <summary>Menu key: name shown to the player and Windows virtual-key code.</summary>
 public static (string Name, int Code) MenuKey
 {
  get
  {
   var code = int.TryParse(Read("menu", "menu-key-code", RightShift.ToString()), out var c) && c > 0 ? c : RightShift;
   var name = Read("menu", "menu-key", "Right Shift"); if (string.IsNullOrWhiteSpace(name)) name = "Right Shift";
   return (name, code);
  }
  set { Write("menu", "menu-key", value.Name); Write("menu", "menu-key-code", value.Code.ToString()); }
 }

 /// <summary>Name and virtual-key code of a WPF key (left / right modifiers kept apart).</summary>
 public static (string Name, int Code) Describe(Key key)
 {
  var code = KeyInterop.VirtualKeyFromKey(key);
  var name = key switch
  {
   Key.RightShift => "Right Shift", Key.LeftShift => "Left Shift", Key.RightCtrl => "Right Ctrl", Key.LeftCtrl => "Left Ctrl",
   Key.RightAlt => "Right Alt", Key.LeftAlt => "Left Alt", Key.Insert => "Insert", Key.Delete => "Delete", Key.Home => "Home", Key.End => "End",
   Key.PageUp => "Page Up", Key.Next => "Page Down", Key.Capital => "Caps Lock", Key.Tab => "Tab", Key.Space => "Space",
   >= Key.D0 and <= Key.D9 => ((int)(key - Key.D0)).ToString(), >= Key.NumPad0 and <= Key.NumPad9 => "Num " + (int)(key - Key.NumPad0),
   _ => Spaced(key.ToString())
  };
  return (name, code);
 }
 private static string Spaced(string s) { var b = new StringBuilder(); for (int i = 0; i < s.Length; i++) { if (i > 0 && char.IsUpper(s[i]) && !char.IsUpper(s[i - 1])) b.Append(' '); b.Append(s[i]); } return b.ToString(); }

 [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "WritePrivateProfileStringW")] private static extern bool WritePrivateProfileString(string section, string key, string value, string file);
 [DllImport("kernel32.dll", CharSet = CharSet.Unicode, EntryPoint = "GetPrivateProfileStringW")] private static extern uint GetPrivateProfileString(string section, string key, string fallback, StringBuilder text, uint size, string file);
}
