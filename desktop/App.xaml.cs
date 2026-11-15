using System.Windows;
namespace Nickel.Desktop;
public partial class App : Application
{
 protected override async void OnStartup(StartupEventArgs e)
 {
  base.OnStartup(e);
  if(e.Args.Contains("--self-test")){Shutdown(SelfCheck.Run());return;}
  if(e.Args.Length==2&&e.Args[0]=="--check-game")
  {
   // Support check of a game file without running it: does it hold a validated build? Writes data/check-game.json.
   object outcome;var code=1;
   try{var refusal=GameBuilds.Check(e.Args[1],GameBuilds.Validated);code=refusal==null?0:3;outcome=new{status=refusal==null?"validated":"refused",file=e.Args[1],message=refusal};}
   catch(Exception error){outcome=new{status="error",file=e.Args[1],message=error.Message};}
   try{var at=System.IO.Path.Combine(AppContext.BaseDirectory,"data","check-game.json");System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(at)!);System.IO.File.WriteAllText(at,System.Text.Json.JsonSerializer.Serialize(outcome));}
   catch{code=1;}
   Shutdown(code);return;
  }
  if(e.Args.Contains("--load-pid"))
  {
   // Command-line loading: same loader and checks as the Load button.
   // The PID is required; never pick implicitly between several Minecraft instances.
   var report=System.IO.Path.Combine(AppContext.BaseDirectory,"data","command-load.json");
   var exit=1;object result;
   try
   {
    var at=Array.IndexOf(e.Args,"--load-pid");
    if(e.Args.Length!=2||at!=0||!int.TryParse(e.Args[1],out var pid)||pid<=0)
     throw new ArgumentException("Usage: Nickel.exe --load-pid <positive Minecraft PID>");
    Environment.SetEnvironmentVariable("NICKEL_TARGET_PID",pid.ToString());
    var loaded=await NativeLoader.LoadAsync(CancellationToken.None);
    using var connection=loaded.Connection;
    exit=loaded.RendererReady?0:2;
    result=new{status=loaded.RendererReady?"ready":"loaded_waiting_renderer",pid=connection.Pid,renderer_ready=loaded.RendererReady,time=DateTimeOffset.UtcNow};
   }
   catch(Exception error){result=new{status="error",message=error.Message,time=DateTimeOffset.UtcNow};}
   try{System.IO.Directory.CreateDirectory(System.IO.Path.GetDirectoryName(report)!);System.IO.File.WriteAllText(report,System.Text.Json.JsonSerializer.Serialize(result));}
   catch{exit=1;}
   Shutdown(exit);return;
  }
  if(e.Args.Contains("--review-ui"))
  {
   // Visual review: the window is actually shown, then exported page by page.
   // No Minecraft monitor, no loading, no preference writes.
   var review=new MainWindow(true);MainWindow=review;
   review.ContentRendered+=async(_,_)=>
   {
    var code=0;
    try{await review.ExportReviewAsync(System.IO.Path.Combine(AppContext.BaseDirectory,"review"));}
    catch(Exception error){code=1;System.IO.File.WriteAllText(System.IO.Path.Combine(AppContext.BaseDirectory,"review-error.txt"),error.ToString());}
    Shutdown(code);
   };
   review.Show();return;
  }
  if(e.Args.Contains("--review-motion"))
  {
   // Motion review: same side-effect-free window, navigating Settings -> Credits -> Home at 1 s intervals.
   var moving=new MainWindow(true);MainWindow=moving;
   moving.ContentRendered+=async(_,_)=>{await moving.PlayMotionAsync();Shutdown(0);};
   moving.Show();return;
  }
  var window=new MainWindow();MainWindow=window;window.Show();
 }
}
