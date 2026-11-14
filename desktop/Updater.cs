using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Text.Json;
namespace Nickel.Desktop;

/// <summary>
/// Updates the launcher: the latest version is asked to nickelclient.com (/api/release), with the latest GitHub release
/// of crossthepath/nickel-client as a fallback.
/// The release must have a "vX.Y.Z" tag (optionally "-beta" or another pre-release suffix) and a "Nickel.exe" asset. The new EXE is downloaded, checked (PE header),
/// swapped with the running EXE (renamed to .old, deleted on next launch) and started.
/// The repository must be public for the API to answer without authentication.
/// </summary>
public static class Updater
{
 public const string Version = "1.0.0-beta";
 /// <summary>Shown next to the name in the launcher and in the in-game menu.</summary>
 public const string Label = "V1 Beta";
 private const string Api = "https://api.github.com/repos/crossthepath/nickel-client/releases/latest";
 private const string SiteApi = "https://nickelclient.com/api/release";
 public sealed record Release(string Tag, string Url, long Size);

 private static HttpClient Client()
 {
  var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
  client.DefaultRequestHeaders.UserAgent.ParseAdd("Nickel-Launcher/" + Version);
  client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
  return client;
 }
 // "v1.2.3" or "v1.2.3-beta": a pre-release sorts before the release with the same number.
 private static (System.Version Number, bool Pre) Parse(string tag)
 {
  var text = tag.Trim().TrimStart('v', 'V'); var dash = text.IndexOf('-');
  var number = System.Version.TryParse(dash < 0 ? text : text[..dash], out var v) ? v : new System.Version(0, 0);
  return (number, dash >= 0);
 }
 private static bool Newer(string tag, string than)
 {
  var (a, aPre) = Parse(tag); var (b, bPre) = Parse(than);
  return a != b ? a > b : bPre && !aPre;
 }

 /// <summary>The release newer than this launcher, or null when up to date.</summary>
 public static async Task<Release?> LatestAsync(CancellationToken cancellation)
 {
  using var client = Client();
  // nickelclient.com first: { tag, url, size } of the launcher it serves.
  try
  {
   using var site = await client.GetAsync(SiteApi, cancellation);
   if (site.IsSuccessStatusCode)
   {
    using var info = JsonDocument.Parse(await site.Content.ReadAsStringAsync(cancellation));
    var siteTag = info.RootElement.GetProperty("tag").GetString() ?? "";
    if (!Newer(siteTag, Version)) return null;
    return new Release(siteTag, info.RootElement.GetProperty("url").GetString()!, info.RootElement.GetProperty("size").GetInt64());
   }
  }
  catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
  catch { }   // site unreachable: GitHub below
  using var response = await client.GetAsync(Api, cancellation);
  if (!response.IsSuccessStatusCode) throw new InvalidOperationException("The update server could not be reached. Check your connection and try again.");
  using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync(cancellation));
  var tag = json.RootElement.GetProperty("tag_name").GetString() ?? "";
  if (!Newer(tag, Version)) return null;
  foreach (var asset in json.RootElement.GetProperty("assets").EnumerateArray())
   if (string.Equals(asset.GetProperty("name").GetString(), "Nickel.exe", StringComparison.OrdinalIgnoreCase))
    return new Release(tag, asset.GetProperty("browser_download_url").GetString()!, asset.GetProperty("size").GetInt64());
  throw new InvalidOperationException($"Release {tag} has no Nickel.exe attached.");
 }

 /// <summary>Downloads the release and replaces the running EXE; with <paramref name="start"/>, starts the new version
 /// (the caller then closes the window).</summary>
 public static async Task InstallAsync(Release release, Action<int> progress, CancellationToken cancellation, bool start = true)
 {
  var current = Environment.ProcessPath ?? throw new InvalidOperationException("The launcher location is unknown.");
  var folder = Path.Combine(Path.GetTempPath(), "Nickel-update"); Directory.CreateDirectory(folder);
  var download = Path.Combine(folder, $"Nickel-{Guid.NewGuid():N}.exe");
  using (var client = Client())
  using (var response = await client.GetAsync(release.Url, HttpCompletionOption.ResponseHeadersRead, cancellation))
  {
   response.EnsureSuccessStatusCode();
   var total = response.Content.Headers.ContentLength ?? release.Size;
   await using var source = await response.Content.ReadAsStreamAsync(cancellation);
   await using var target = File.Create(download);
   var buffer = new byte[81920]; long done = 0; int read, last = -1;
   while ((read = await source.ReadAsync(buffer, cancellation)) > 0)
   {
    await target.WriteAsync(buffer.AsMemory(0, read), cancellation); done += read;
    var percent = total > 0 ? (int)(done * 100 / total) : 0; if (percent != last) { last = percent; progress(percent); }
   }
  }
  var head = new byte[2]; await using (var check = File.OpenRead(download)) { if (check.Length < 1024 * 1024 || await check.ReadAsync(head, cancellation) != 2 || head[0] != 'M' || head[1] != 'Z') throw new InvalidOperationException("The downloaded file is not a valid launcher."); }
  var old = current + ".old";
  if (File.Exists(old)) File.Delete(old);
  File.Move(current, old);   // Windows allows renaming a running EXE
  try { File.Move(download, current); }
  catch { File.Move(old, current); throw; }
  if (start) Restart();
 }
 /// <summary>Starts the launcher EXE again (after an update; the caller then closes the window).</summary>
 public static void Restart()
 {
  var current = Environment.ProcessPath ?? throw new InvalidOperationException("The launcher location is unknown.");
  Process.Start(new ProcessStartInfo(current) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(current)! })?.Dispose();
 }

 /// <summary>Deletes the old EXE left by an update.</summary>
 public static void Cleanup()
 {
  try { var path = Environment.ProcessPath; if (path != null && File.Exists(path + ".old")) File.Delete(path + ".old"); } catch { }
 }
}
