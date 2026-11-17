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

 /// <summary>Text of the notice under Load: nothing while loading, the game warning while loaded (game features off),
 /// the load error after a failed load, else the error of the last Minecraft check ("" when it worked).</summary>
 public static string Notice(bool waiting, bool loaded, bool failed, string message, string monitorError, string loadedWarning = "") =>
  loaded ? loadedWarning : waiting ? "" : failed ? message : monitorError;

 public static string? SelfTest()
 {
  const string check = "Nickel could not check for Minecraft (x). Retrying.";
  if (Notice(false, false, false, "", check) != check) return "a failed check is not shown";
  if (Notice(false, false, false, "", "") != "") return "a notice is shown after a good check";
  if (Notice(false, false, true, "Load error", check) != "Load error") return "a load error is not shown first";
  if (Notice(true, false, false, "", check) != "" || Notice(false, true, false, "Press Right Shift", check) != "") return "a notice is shown while loading or loaded";
  if (Notice(false, true, false, "Press Right Shift", check, "game modules are off") != "game modules are off") return "the warning of a loaded game is not shown";
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
