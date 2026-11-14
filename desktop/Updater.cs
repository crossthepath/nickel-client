using System.Diagnostics;
using System.Net.Http;
namespace Nickel.Desktop;

/// <summary>
/// Updates the launcher. The signed release manifest (ReleaseTrust.cs) is read from nickelclient.com, with the latest
/// GitHub release of crossthepath/nickel-client as a fallback; nothing is installed unless the manifest verifies
/// against the pinned key and names a newer version. The EXE is then downloaded over HTTPS from the project's hosts,
/// checked against the manifest's size and SHA-256, swapped with the running EXE (renamed to .old, deleted on the next
/// launch; put back if the swap fails) and started.
/// </summary>
public static class Updater
{
 public const string Version = "1.0.0-beta";
 /// <summary>Shown next to the name in the launcher and in the in-game menu.</summary>
 public const string Label = "V1 Beta";
 private const string Site = "https://nickelclient.com/release/";
 private const string GitHub = "https://github.com/crossthepath/nickel-client/releases/";
 public sealed record Release(ReleaseManifest Manifest, string[] Urls)
 {
  public string Tag => Manifest.Tag;
  public long Size => Manifest.Size;
 }

 private static HttpClient Client()
 {
  var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
  client.DefaultRequestHeaders.UserAgent.ParseAdd("Nickel-Launcher/" + Version);
  return client;
 }
 private static async Task<byte[]?> GetBytes(HttpClient client, string url, CancellationToken cancellation)
 {
  try
  {
   using var response = await client.GetAsync(url, cancellation);
   if (!response.IsSuccessStatusCode || !ReleaseTrust.AllowedSource(response.RequestMessage?.RequestUri)) return null;
   return await response.Content.ReadAsByteArrayAsync(cancellation);
  }
  catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
  catch { return null; }
 }

 /// <summary>The verified release newer than this launcher, or null when up to date.</summary>
 public static async Task<Release?> LatestAsync(CancellationToken cancellation)
 {
  using var client = Client();
  bool reached = false, unsigned = false;
  foreach (var root in new[] { Site, GitHub + "latest/download/" })
  {
   var manifest = await GetBytes(client, root + ReleaseTrust.ManifestName, cancellation);
   var signature = await GetBytes(client, root + ReleaseTrust.SignatureName, cancellation);
   if (manifest == null || signature == null) continue;
   reached = true;
   var verified = ReleaseTrust.Verify(manifest, signature);
   if (verified == null) { unsigned = true; continue; }   // not signed by the pinned key: try the other source, never install
   if (ReleaseTrust.Compare(verified.Version, Version) <= 0) return null;   // also refuses replayed older manifests
   return new Release(verified, [Site.Replace("/release/", "/download"), $"{GitHub}download/{verified.Tag}/Nickel.exe"]);
  }
  if (unsigned) throw new InvalidOperationException("The update could not be verified (invalid signature). Nothing was installed.");
  if (!reached) throw new InvalidOperationException("The update server could not be reached. Check your connection and try again.");
  return null;
 }

 /// <summary>Downloads the release, verifies it and replaces the running EXE; with <paramref name="start"/>, starts the
 /// new version (the caller then closes the window).</summary>
 public static async Task InstallAsync(Release release, Action<int> progress, CancellationToken cancellation, bool start = true)
 {
  var current = Environment.ProcessPath ?? throw new InvalidOperationException("The launcher location is unknown.");
  var folder = Path.Combine(Path.GetTempPath(), "Nickel-update"); Directory.CreateDirectory(folder);
  var download = Path.Combine(folder, $"Nickel-{Guid.NewGuid():N}.exe");
  try
  {
   var verified = false;
   foreach (var url in release.Urls)
   {
    try { await DownloadAsync(url, download, release.Size, progress, cancellation); }
    catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
    catch { continue; }
    if (ReleaseTrust.Matches(download, release.Manifest)) { verified = true; break; }
   }
   if (!verified) throw new InvalidOperationException("The downloaded file does not match the signed release. Nothing was installed.");
   var old = current + ".old";
   if (File.Exists(old)) File.Delete(old);
   File.Move(current, old);   // Windows allows renaming a running EXE
   try { File.Move(download, current); }
   catch { File.Move(old, current); throw; }
  }
  finally { try { if (File.Exists(download)) File.Delete(download); } catch { } }
  if (start) Restart();
 }
 private static async Task DownloadAsync(string url, string path, long expected, Action<int> progress, CancellationToken cancellation)
 {
  using var client = Client();
  using var response = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellation);
  response.EnsureSuccessStatusCode();
  if (!ReleaseTrust.AllowedSource(response.RequestMessage?.RequestUri)) throw new InvalidOperationException("Unexpected download host.");
  await using var source = await response.Content.ReadAsStreamAsync(cancellation);
  await using var target = File.Create(path);
  var buffer = new byte[81920]; long done = 0; int read, last = -1;
  while ((read = await source.ReadAsync(buffer, cancellation)) > 0)
  {
   done += read; if (done > expected) throw new InvalidOperationException("The download is larger than the signed release.");
   await target.WriteAsync(buffer.AsMemory(0, read), cancellation);
   var percent = (int)(done * 100 / expected); if (percent != last) { last = percent; progress(percent); }
  }
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
