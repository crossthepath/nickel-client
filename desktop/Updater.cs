using System.Diagnostics;
using System.Net;
using System.Net.Http;
using System.Reflection;
using System.Security.Cryptography;
namespace Nickel.Desktop;

/// <summary>
/// Updates the launcher. The signed release manifest (ReleaseTrust.cs) is read from nickelclient.com and from the
/// latest GitHub release of crossthepath/nickel-client; the newest manifest that verifies against the pinned key, is
/// on an accepted channel and is newer than this launcher is offered. The EXE is downloaded over HTTPS from the
/// project's hosts (every redirect hop checked) into the launcher's folder, checked against the manifest's size and
/// SHA-256 while a handle keeps it from being replaced, swapped with the running EXE (renamed to .old, deleted on the
/// next launch; put back if the swap fails) and started.
/// </summary>
public static class Updater
{
 /// <summary>This launcher's version, from &lt;Version&gt; in Nickel.Desktop.csproj (build metadata removed).</summary>
 public static readonly string Version = (typeof(Updater).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "0.0.0").Split('+')[0];
 /// <summary>Shown next to the name in the launcher and in the in-game menu.</summary>
 public const string Label = "V1 Beta";
 private const string Site = "https://nickelclient.com/release/";
 private const string GitHub = "https://github.com/crossthepath/nickel-client/releases/";
 private static readonly TimeSpan DownloadLimit = TimeSpan.FromMinutes(10), StallLimit = TimeSpan.FromSeconds(60);
 public sealed record Release(ReleaseManifest Manifest, string[] Urls)
 {
  public string Tag => Manifest.Tag;
  public long Size => Manifest.Size;
 }

 private static HttpClient Client(long? maxBuffer = null)
 {
  // redirects are followed by SendChecked, so every hop is checked against the allowed hosts before it is requested
  var client = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromMinutes(2) };
  if (maxBuffer is long cap) client.MaxResponseContentBufferSize = cap;   // small documents: no unbounded buffering
  client.DefaultRequestHeaders.UserAgent.ParseAdd("Nickel-Launcher/" + Version);
  return client;
 }
 /// <summary>GET with redirects followed by hand (at most 5), each target HTTPS on an allowed host.</summary>
 private static async Task<HttpResponseMessage> SendChecked(HttpClient client, string url, HttpCompletionOption completion, CancellationToken cancellation)
 {
  var uri = new Uri(url);
  for (int hop = 0; ; ++hop)
  {
   if (!ReleaseTrust.AllowedSource(uri)) throw new InvalidOperationException($"Refused update host {uri.Host}.");
   var response = await client.GetAsync(uri, completion, cancellation);
   var code = (int)response.StatusCode;
   if (code is < 300 or > 399 || response.Headers.Location is null) return response;
   var next = response.Headers.Location.IsAbsoluteUri ? response.Headers.Location : new Uri(uri, response.Headers.Location);
   response.Dispose();
   if (hop == 5) throw new InvalidOperationException("Too many redirects.");
   uri = next;
  }
 }
 private static async Task<byte[]?> GetBytes(HttpClient client, string url, CancellationToken cancellation)
 {
  try
  {
   using var response = await SendChecked(client, url, HttpCompletionOption.ResponseContentRead, cancellation);
   return response.IsSuccessStatusCode ? await response.Content.ReadAsByteArrayAsync(cancellation) : null;
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
   if (ReleaseTrust.Verify(manifest, signature) is { } m && ReleaseTrust.ChannelAccepted(m.Channel, Version)) verified.Add(m); else invalid = true;   // never installed
  }
  if (verified.Count == 0)
  {
   if (invalid) throw new InvalidOperationException("The update could not be verified (invalid signature or channel). Nothing was installed.");
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
  var folder = Path.GetDirectoryName(current)!;
  // next to the launcher: the swap is a same-volume rename, and the file is not in a shared temporary folder
  var download = Path.Combine(folder, $".Nickel-update-{Guid.NewGuid():N}.exe");
  try
  {
   Exception? last = null; var ok = false;
   foreach (var url in release.Urls)
   {
    try { ok = await DownloadAsync(url, download, release.Manifest, progress, cancellation); if (ok) break; }
    catch (OperationCanceledException) when (cancellation.IsCancellationRequested) { throw; }
    catch (UnauthorizedAccessException) { throw new InvalidOperationException($"Nickel cannot write to {folder}. Move Nickel.exe to a folder you own (for example Documents) and update again."); }
    catch (IOException error) when (error.HResult is unchecked((int)0x80070070) or unchecked((int)0x80070027)) { throw new InvalidOperationException("Not enough disk space to download the update."); }
    catch (Exception error) { last = error; }
   }
   if (!ok) throw new InvalidOperationException("The update could not be downloaded or does not match the signed release. Nothing was installed." + (last != null ? $" ({last.Message})" : ""));
   ReleaseTrust.SwapVerified(download, current, release.Manifest);
  }
  finally { try { if (File.Exists(download)) File.Delete(download); } catch { } }
  if (start) Restart();
 }
 /// <summary>Downloads into <paramref name="path"/> and returns whether the bytes have the manifest's size and SHA-256
 /// (hashed while they are written). A download that takes too long or stops sending data is abandoned.</summary>
 private static async Task<bool> DownloadAsync(string url, string path, ReleaseManifest manifest, Action<int> progress, CancellationToken cancellation)
 {
  using var overall = CancellationTokenSource.CreateLinkedTokenSource(cancellation); overall.CancelAfter(DownloadLimit);
  using var client = Client();
  using var response = await SendChecked(client, url, HttpCompletionOption.ResponseHeadersRead, overall.Token);
  response.EnsureSuccessStatusCode();
  using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
  await using var source = await response.Content.ReadAsStreamAsync(overall.Token);
  await using (var target = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None))
  {
   var buffer = new byte[81920]; long done = 0; int read, last = -1;
   while (true)
   {
    using var stall = CancellationTokenSource.CreateLinkedTokenSource(overall.Token); stall.CancelAfter(StallLimit);
    try { read = await source.ReadAsync(buffer, stall.Token); }
    catch (OperationCanceledException) when (!cancellation.IsCancellationRequested) { throw new TimeoutException("The download stalled."); }
    if (read <= 0) break;
    done += read; if (done > manifest.Size) throw new InvalidOperationException("The download is larger than the signed release.");
    hash.AppendData(buffer, 0, read);
    await target.WriteAsync(buffer.AsMemory(0, read), overall.Token);
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

 /// <summary>Deletes the old EXE and unfinished downloads left by an update (older than an hour, so another launcher's
 /// download in progress is kept).</summary>
 public static void Cleanup()
 {
  try
  {
   var path = Environment.ProcessPath; if (path == null) return;
   if (File.Exists(path + ".old")) File.Delete(path + ".old");
   foreach (var stale in Directory.EnumerateFiles(Path.GetDirectoryName(path)!, ".Nickel-update-*.exe"))
    if (DateTime.UtcNow - File.GetLastWriteTimeUtc(stale) > TimeSpan.FromHours(1)) File.Delete(stale);
  }
  catch { }
 }
}
