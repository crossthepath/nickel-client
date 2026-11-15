namespace Nickel.Desktop;

/// <summary>Runs one check of the launcher's Minecraft monitor (MainWindow.Monitor_Tick). The timer handler is
/// <c>async void</c>: an exception leaving it would close the launcher, so the check's error is returned instead and the
/// next tick checks again.</summary>
public static class MonitorTick
{
 /// <summary>The error thrown by <paramref name="tick"/> (synchronously or after an await), or null.</summary>
 public static async Task<Exception?> Run(Func<Task> tick)
 {
  try { await tick().ConfigureAwait(false); return null; }
  catch (Exception error) { return error; }
 }

 public static string? SelfTest()
 {
  // Task.Run: no UI synchronization context, so waiting here cannot block a continuation.
  static Exception? Check(Func<Task> tick) => Task.Run(() => Run(tick)).GetAwaiter().GetResult();
  try
  {
   if (Check(() => Task.CompletedTask) != null) return "a check without error reported one";
   if (Check(() => throw new InvalidOperationException("scan")) is not InvalidOperationException) return "an error thrown by a check was not returned";
   if (Check(async () => { await Task.Yield(); throw new InvalidOperationException("later"); }) is not InvalidOperationException)
    return "an error thrown after an await was not returned";
  }
  catch (Exception error) { return "an error escaped the monitor: " + error.Message; }
  return null;
 }
}
