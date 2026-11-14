using System.Net.Http;
using System.Net.Http.Json;
using System.Security.Cryptography;
namespace Nickel.Desktop;

/// <summary>
/// Anonymous usage ping for the player counters of nickelclient.com: while the launcher is open (and Usage statistics
/// is on), it sends { id, version } every 2 minutes. The id is a random value created once on this PC; nothing else is
/// sent (no name, no account, no hardware data). Failures are ignored.
/// </summary>
public static class UsagePing
{
 public const string Endpoint = "https://nickelclient.com/api/ping";
 private static readonly TimeSpan Every = TimeSpan.FromMinutes(2);
 private static CancellationTokenSource? running;

 public static string NewId() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();

 public static void SetEnabled(bool enabled, string id)
 {
  running?.Cancel(); running = null;
  if (!enabled || string.IsNullOrEmpty(id)) return;
  var stop = running = new CancellationTokenSource();
  _ = Task.Run(async () =>
  {
   using var client = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
   client.DefaultRequestHeaders.UserAgent.ParseAdd("Nickel-Launcher/" + Updater.Version);
   while (!stop.IsCancellationRequested)
   {
    try { using var _ = await client.PostAsJsonAsync(Endpoint, new { id, version = Updater.Version }, stop.Token); } catch { }
    try { await Task.Delay(Every, stop.Token); } catch { break; }
   }
  });
 }
}
