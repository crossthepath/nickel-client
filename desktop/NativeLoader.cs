using System.ComponentModel;
using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace Nickel.Desktop;
public sealed record NativeLoadResult(MinecraftConnection Connection,bool RendererReady);
/// <summary>What the overlay reported about the game build: game features on (Detail names the build) or off (Detail
/// says why).</summary>
public sealed record GameBuildStatus(bool On,string Detail);
public static class NativeLoader
{
 /// <summary>Launcher Stream Hide: the "stream_hide" module (menu and module list hidden from captures) is enabled before loading.</summary>
 public static bool StreamHide;
 public static byte[] Payload(){using var source=Assembly.GetExecutingAssembly().GetManifestResourceStream("Nickel.Overlay")??throw new InvalidOperationException("The native component is missing. Rebuild the complete application.");using var bytes=new MemoryStream();source.CopyTo(bytes);return bytes.ToArray();}
 private static string Extract()
 {
  var bytes=Payload();if(bytes.Length<512||bytes[0]!='M'||bytes[1]!='Z')throw new InvalidOperationException("Native component invalid.");
  var hash=Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();var folder=Path.Combine(AppContext.BaseDirectory,"data","runtime",hash);Directory.CreateDirectory(folder);var file=Path.Combine(folder,"Nickel.Overlay.dll");
  if(!File.Exists(file)||!SHA256.HashData(File.ReadAllBytes(file)).SequenceEqual(SHA256.HashData(bytes))){var pending=file+"."+Guid.NewGuid().ToString("N")+".tmp";File.WriteAllBytes(pending,bytes);File.Move(pending,file,true);}
  AllowPackagedApps(file);
  if(StreamHide)SharedPreferences.Write("stream_hide","enabled","1");   // shared preferences in %APPDATA%\Nickel (read by the overlay)
  return file;
 }
 /// <summary>Lets a Minecraft that runs as a packaged app (an app container, as some launchers start it) read and run
 /// the component: read and execute for ALL APPLICATION PACKAGES and ALL RESTRICTED APPLICATION PACKAGES. A failure is
 /// ignored: a normal Minecraft does not need it.</summary>
 private static void AllowPackagedApps(string file)
 {
  try
  {
   var info=new FileInfo(file);var rules=info.GetAccessControl();
   foreach(var sid in new[]{"S-1-15-2-1","S-1-15-2-2"})
    rules.AddAccessRule(new System.Security.AccessControl.FileSystemAccessRule(new System.Security.Principal.SecurityIdentifier(sid),System.Security.AccessControl.FileSystemRights.ReadAndExecute,System.Security.AccessControl.AccessControlType.Allow));
   info.SetAccessControl(rules);
  }
  catch(Exception error) when(error is IOException or UnauthorizedAccessException or InvalidOperationException or PlatformNotSupportedException or System.Security.Principal.IdentityNotMappedException){}
 }
 public static async Task<NativeLoadResult> LoadAsync(CancellationToken cancellation)
 {
  var verified=await MinecraftConnection.OpenAsync(cancellation);
  try{return await Task.Run(()=>LoadIntoVerifiedProcess(verified,cancellation),cancellation);}
  catch{verified.Dispose();throw;}
 }
 private static NativeLoadResult LoadIntoVerifiedProcess(MinecraftConnection verified,CancellationToken cancellation)
 {
  var file=Extract();using var process=Process.GetProcessById(verified.Pid);
  foreach(ProcessModule module in process.Modules)
   if(string.Equals(module.ModuleName,"Nickel.Overlay.dll",StringComparison.OrdinalIgnoreCase))
   {if(!string.Equals(module.FileName,file,StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("A different Nickel build is already loaded. Restart Minecraft before loading this build.");return new(verified,WaitForRenderer(verified.Pid,1500));}
  cancellation.ThrowIfCancellationRequested();
  // Even with an explicit PID, never load Nickel into a process that already hosts another injected client
  // (manually mapped code does not appear in process.Modules).
  if(MinecraftConnection.ForeignCode(verified.Pid)!=0)
   throw new InvalidOperationException("Another injected client is present, or its absence could not be verified. Use a separate Minecraft instance.");
  using var handle=OpenProcess(0x43A,false,verified.Pid);if(handle.IsInvalid)throw new Win32Exception(Marshal.GetLastWin32Error(),"Windows refused to load the component into Minecraft.");
  var path=new StringBuilder(32768);uint size=(uint)path.Capacity;if(!QueryFullProcessImageName(handle,0,path,ref size)||!MinecraftConnection.SameFile(path.ToString(),verified.ImagePath)||!verified.IsAlive)throw new InvalidOperationException("Minecraft changed during loading. Try again.");
  var localKernel=GetModuleHandle("kernel32.dll");var localLoad=GetProcAddress(localKernel,"LoadLibraryW");if(localLoad==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error());
  string? ownerName=null;long offset=0;using(var own=Process.GetCurrentProcess())foreach(ProcessModule module in own.Modules){var start=module.BaseAddress.ToInt64();if(localLoad.ToInt64()>=start&&localLoad.ToInt64()<start+module.ModuleMemorySize){ownerName=module.ModuleName;offset=localLoad.ToInt64()-start;break;}}
  IntPtr remoteLoad=IntPtr.Zero;foreach(ProcessModule module in process.Modules)if(string.Equals(module.ModuleName,ownerName,StringComparison.OrdinalIgnoreCase)){remoteLoad=new IntPtr(module.BaseAddress.ToInt64()+offset);break;}
  if(remoteLoad==IntPtr.Zero)throw new InvalidOperationException("The Windows loader entry could not be located in Minecraft.");
  byte[] name=Encoding.Unicode.GetBytes(file+'\0');var memory=VirtualAllocEx(handle,IntPtr.Zero,(nuint)name.Length,0x3000,4);if(memory==IntPtr.Zero)throw new Win32Exception(Marshal.GetLastWin32Error());bool safeToFree=true;
  try
  {
   if(!WriteProcessMemory(handle,memory,name,(nuint)name.Length,out var written)||written!=(nuint)name.Length)throw new Win32Exception(Marshal.GetLastWin32Error());
   using var thread=CreateRemoteThread(handle,IntPtr.Zero,0,remoteLoad,memory,0,out _);if(thread.IsInvalid)throw new Win32Exception(Marshal.GetLastWin32Error());safeToFree=false;
   var wait=WaitForSingleObject(thread,30000);if(wait!=0)throw new InvalidOperationException("Loading has not completed. Do not retry until Minecraft has restarted.");safeToFree=true;
   if(!GetExitCodeThread(thread,out var result)||result==0)throw new InvalidOperationException("Windows could not load the native component.");
  }
  finally{if(safeToFree)VirtualFreeEx(handle,memory,0,0x8000);}
  return new(verified,WaitForRenderer(verified.Pid,8000));
 }
 /// <summary>The game build status the overlay published in Minecraft (native/overlay.cpp PublishBuildStatus), or null
 /// when there is none (an older overlay, or not loaded).</summary>
 public static GameBuildStatus? GameStatus(int pid)
 {
  try
  {
   using var map=System.IO.MemoryMappedFiles.MemoryMappedFile.OpenExisting($"Local\\NickelOverlay.Build.{pid}",System.IO.MemoryMappedFiles.MemoryMappedFileRights.Read);
   using var view=map.CreateViewStream(0,1024,System.IO.MemoryMappedFiles.MemoryMappedFileAccess.Read);
   var bytes=new byte[1024];var read=view.Read(bytes,0,bytes.Length);var text=Encoding.UTF8.GetString(bytes,0,read).TrimEnd('\0');
   var split=text.IndexOf('\n');if(split<0)return null;
   return new(text[..split]=="ok",text[(split+1)..]);
  }
  catch(Exception error) when(error is FileNotFoundException or IOException or UnauthorizedAccessException){return null;}
 }
 public static bool WaitForRenderer(int pid,uint milliseconds)
 {
  var deadline=Environment.TickCount64+milliseconds;do{using var ready=OpenEvent(0x100000,false,$"Local\\NickelOverlay.Rendered.{pid}");if(!ready.IsInvalid)return WaitForSingleObject(ready,(uint)Math.Max(0,deadline-Environment.TickCount64))==0;Thread.Sleep(50);}while(Environment.TickCount64<deadline);return false;
 }
 [DllImport("kernel32.dll",SetLastError=true)]private static extern SafeProcessHandle OpenProcess(uint access,bool inherit,int pid);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,EntryPoint="WritePrivateProfileStringW")]private static extern bool WritePrivateProfileString(string section,string key,string value,string file);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,EntryPoint="QueryFullProcessImageNameW",SetLastError=true)]private static extern bool QueryFullProcessImageName(SafeProcessHandle process,uint flags,StringBuilder path,ref uint length);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,EntryPoint="GetModuleHandleW")]private static extern IntPtr GetModuleHandle(string name);
 [DllImport("kernel32.dll",CharSet=CharSet.Ansi,ExactSpelling=true,SetLastError=true)]private static extern IntPtr GetProcAddress(IntPtr module,string name);
 [DllImport("kernel32.dll",SetLastError=true)]private static extern IntPtr VirtualAllocEx(SafeProcessHandle process,IntPtr address,nuint size,uint allocation,uint protect);
 [DllImport("kernel32.dll",SetLastError=true)]private static extern bool VirtualFreeEx(SafeProcessHandle process,IntPtr address,nuint size,uint kind);
 [DllImport("kernel32.dll",SetLastError=true)]private static extern bool WriteProcessMemory(SafeProcessHandle process,IntPtr address,byte[] buffer,nuint size,out nuint written);
 [DllImport("kernel32.dll",SetLastError=true)]private static extern SafeWaitHandle CreateRemoteThread(SafeProcessHandle process,IntPtr attributes,nuint stack,IntPtr start,IntPtr argument,uint flags,out uint threadId);
 [DllImport("kernel32.dll",SetLastError=true)]private static extern uint WaitForSingleObject(SafeWaitHandle handle,uint milliseconds);
 [DllImport("kernel32.dll",SetLastError=true)]private static extern bool GetExitCodeThread(SafeWaitHandle thread,out uint code);
 [DllImport("kernel32.dll",CharSet=CharSet.Unicode,EntryPoint="OpenEventW",SetLastError=true)]private static extern SafeWaitHandle OpenEvent(uint access,bool inherit,string name);
}
