using System.Diagnostics;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
namespace Nickel.Desktop;

/// <summary>
/// Updates the launcher. The signed release manifest (ReleaseTrust.cs) is read from nickelclient.com and from the
/// latest GitHub release of crossthepath/nickel-client; the newest manifest that verifies against the pinned key is
/// offered when it is newer than this launcher. The EXE is downloaded over HTTPS from the project's hosts into the
/// launcher's folder, checked against the manifest's size and SHA-256, swapped with the running EXE (renamed to .old,
/// deleted on the next launch; put back if the swap fails) and started.
/// </summary>
public static class Updater
{
 /// <summary>This launcher's version, from &lt;Version&gt; in Nickel.Desktop.csproj (build metadata removed).</summary>
 public static readonly string Version = (typeof(Updater).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0").Split('+')[0];
 /// <summary>Shown next to the name in the launcher and in the in-game menu.</summary>
 public const string Label = "V1 Beta";
 private const string Site = "https://nickelclient.com/release/";
 private const string GitHub = "https://github.com/crossthepath/nickel-client/releases/";
 public sealed record Release(ReleaseManifest Manifest, string[] Urls)
 {
  public string Tag => Manifest.Tag;
  public long Size => Manifest.Size;
 }

 private static HttpClient Client(long? maxBuffer = null)
 {
  var client = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
  if (maxBuffer is long cap) client.MaxResponseContentBufferSize = cap;   // small documents: no unbounded buffering
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
  using var client = Client(16 * 1024);
  var verified = new List<ReleaseManifest>(); bool reached = false, invalid = false;
  foreach (var root in new[] { Site, GitHub + "latest/download/" })   // every source: a stale one cannot hide a newer release
  {
   var manifest = await GetBytes(client, root + ReleaseTrust.ManifestName, cancellation);
   var signature = await GetBytes(client, root + ReleaseTrust.SignatureName, cancellation);
   if (manifest == null || signature == null) continue;
   reached = true;
   if (ReleaseTrust.Verify(manifest, signature) is { } m) verified.Add(m); else invalid = true;   // never installed
  }
  if (verified.Count == 0)
  {
   if (invalid) throw new InvalidOperationException("The update could not be verified (invalid signature). Nothing was installed.");
   if (!reached) throw new InvalidOperationException("The update server could not be reached. Check your connection and try again.");
   return null;
  }
  var newest = ReleaseTrust.Newest(verified, Version);   // older or equal versions are never offered (replay, downgrade)
  return newest == null ? null : new Release(newest, [Site.Replace("/release/", "/download"), $"{GitHub}download/{newest.Tag}/Nickel.exe"]);
 }

 /// <summary>Downloads the release, verifies it and replaces the running EXE; with <paramref name="start"/>, starts the
 /// new version (the caller then closes the window).</summary>
 public static async Task InstallAsync(Release release, Action<int> progress, CancellationToken cancellation, bool start = true)
 {
  var current = Environment.ProcessPath ?? throw new InvalidOperationException("The launcher location is unknown.");
  // next to the launcher: the swap is a same-volume rename, and the file is not in a shared temporary folder
  var download = Path.Combine(Path.GetDirectoryName(current)!, $".Nickel-update-{Guid.NewGuid():N}.exe");
  try
  {
   var verified = false;
   foreach (var url in release.Urls)
   {
    try { verified = await DownloadAsync(url, download, release.Manifest, progress, cancellation); }
    catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
    catch { verified = false; }
    if (verified) break;
   }
   if (!verified || !ReleaseTrust.Matches(download, release.Manifest))
    throw new InvalidOperationException("The downloaded file does not match the signed release. Nothing was installed.");
   var old = current + ".old";
   if (File.Exists(old)) File.Delete(old);
   File.Move(current, old);   // Windows allows renaming a running EXE
   try { File.Move(download, current); }
   catch { File.Move(old, current, overwrite: true); throw; }
  }
  finally { try { if (File.Exists(download)) File.Delete(download); } catch { } }
  if (start) Restart();
 }
 /// <summary>Downloads into <paramref name="path"/> and returns whether the bytes have the manifest's size and SHA-256
 /// (hashed while they are written; the file is checked again before the swap).</summary>
 private static async Task<bool> DownloadAsync(string url, string path, ReleaseManifest manifest, Action<int> progress, CancellationToken cancellation)
 {
  using var client = Client();
  using var response = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellation);
  response.EnsureSuccessStatusCode();
  if (!ReleaseTrust.AllowedSource(response.RequestMessage?.RequestUri)) throw new InvalidOperationException("Unexpected download host.");
  using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
  await using var source = await response.Content.ReadAsStreamAsync(cancellation);
  await using (var target = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None))
  {
   var buffer = new byte[81920]; long done = 0; int read, last = -1;
   while ((read = await source.ReadAsync(buffer, cancellation)) > 0)
   {
    done += read; if (done > manifest.Size) throw new InvalidOperationException("The download is larger than the signed release.");
    hash.AppendData(buffer, 0, read);
    await target.WriteAsync(buffer.AsMemory(0, read), cancellation);
    var percent = (int)(done * 100 / manifest.Size); if (percent != last) { last = percent; progress(percent); }
   }
   if (done != manifest.Size) return false;
  }
  return ReleaseTrust.HashMatches(hash.GetHashAndReset(), manifest);
 }
 /// <summary>Starts the launcher EXE again (after an update; the caller then closes the window).</summary>
 public static void Restart()
 {
  var current = Environment.ProcessPath ?? throw new InvalidOperationException("The launcher location is unknown.");
  Process.Start(new ProcessStartInfo(current) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(current)! })?.Dispose();
 }

 /// <summary>Deletes the old EXE and any unfinished download left by an update.</summary>
 public static void Cleanup()
 {
  try
  {
   var path = Environment.ProcessPath; if (path == null) return;
   if (File.Exists(path + ".old")) File.Delete(path + ".old");
   foreach (var stale in Directory.EnumerateFiles(Path.GetDirectoryName(path)!, ".Nickel-update-*.exe")) File.Delete(stale);
  }
  catch { }
 }
}
