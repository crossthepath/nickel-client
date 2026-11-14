using Microsoft.Win32;
namespace Nickel.Desktop;

/// <summary>"Start with Windows": a "Nickel" value under HKCU\Software\Microsoft\Windows\CurrentVersion\Run (current user only).</summary>
public static class Startup
{
 private const string Key = @"Software\Microsoft\Windows\CurrentVersion\Run", Name = "Nickel";
 private static string Command => $"\"{Environment.ProcessPath}\"";
 public static bool Enabled
 {
  get { try { using var key = Registry.CurrentUser.OpenSubKey(Key); return key?.GetValue(Name) is string value && string.Equals(value, Command, StringComparison.OrdinalIgnoreCase); } catch { return false; } }
 }
 public static bool Set(bool on)
 {
  try
  {
   using var key = Registry.CurrentUser.CreateSubKey(Key, true);
   if (on) key.SetValue(Name, Command); else key.DeleteValue(Name, false);
   return true;
  }
  catch { return false; }
 }
}
