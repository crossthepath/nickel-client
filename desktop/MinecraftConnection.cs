using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Nickel.Desktop;
public sealed class MinecraftConnection : IDisposable
{
 public const string Version="1.26.62.01";
 // Any installation of a validated build is accepted (Microsoft Store / Xbox app, LeviLauncher, other launchers): the
 // image must be Minecraft.Windows.exe, carry a validated version and hold that build's code, constants and data
 // (GameBuilds). The overlay checks the version and the section layout again before it hooks anything.
 private readonly SafeProcessHandle handle;
 public int Pid{get;}
 public string ImagePath{get;}
 public bool IsAlive=>!handle.IsClosed&&!handle.IsInvalid&&GetExitCodeProcess(handle,out var code)&&code==259;
 private MinecraftConnection(SafeProcessHandle handle,int pid,string path){this.handle=handle;Pid=pid;ImagePath=path;}
 public static Task<MinecraftConnection> OpenAsync(CancellationToken cancellation)=>Task.Run(()=>
 {
  var processes=Process.GetProcessesByName("Minecraft.Windows");
  // With several instances running, NICKEL_TARGET_PID selects the target process.
  if(int.TryParse(Environment.GetEnvironmentVariable("NICKEL_TARGET_PID"),out var targetPid)&&targetPid>0)
  {var kept=processes.Where(p=>p.Id==targetPid).ToArray();foreach(var other in processes)if(!kept.Contains(other))other.Dispose();processes=kept;}
  // With several Minecraft instances open, skip those that already contain another injected client (large private
  // executable regions) to avoid a double injection.
  else if(processes.Length>1)
  {var kept=processes.Where(p=>ForeignCode(p.Id)==0).ToArray();foreach(var other in processes)if(!kept.Contains(other))other.Dispose();processes=kept;}
  try
  {
   if(processes.Length!=1)throw new InvalidOperationException(processes.Length==0?"Start Minecraft before connecting.":"Close extra Minecraft instances, then try again.");
   cancellation.ThrowIfCancellationRequested();
   var process=processes[0];var module=process.MainModule??throw new InvalidOperationException("Minecraft could not be inspected.");
   var path=module.FileName;var pid=process.Id;
   var opened=OpenProcess(0x410,false,pid); // QUERY_INFORMATION | VM_READ, never VM_WRITE.
   if(opened.IsInvalid){opened.Dispose();throw new Win32Exception(Marshal.GetLastWin32Error(),"Windows refused access to Minecraft.");}
   try
   {
    var actual=new StringBuilder(32768);var length=(uint)actual.Capacity;
    if(!QueryFullProcessImageName(opened,0,actual,ref length))throw new Win32Exception(Marshal.GetLastWin32Error());
    // Same file, not same text: a Microsoft Store / Xbox app installation reports its main module under
    // C:\Program Files\WindowsApps\... and its image under C:\XboxGames\..., two names of one file.
    if(!SameFile(path,actual.ToString()))throw new InvalidOperationException("Minecraft changed during connection. Try again.");
    if(GameBuilds.Check(actual.ToString(),GameBuilds.Validated) is string refusal)throw new InvalidOperationException(refusal);
    cancellation.ThrowIfCancellationRequested();
    if(!GetExitCodeProcess(opened,out var code)||code!=259)throw new InvalidOperationException("Minecraft closed during connection.");
    return new MinecraftConnection(opened,pid,path);
   }
   catch{opened.Dispose();throw;}
  }
  finally{foreach(var process in processes)process.Dispose();}
 },cancellation);
 public void Dispose()=>handle.Dispose();
 [StructLayout(LayoutKind.Sequential)] private struct MemoryInfo{public ulong Base,AllocationBase;public uint AllocationProtect,Pad0;public ulong Size;public uint State,Protect,Type,Pad1;}
 [DllImport("kernel32.dll")] private static extern nint VirtualQueryEx(SafeProcessHandle process,ulong address,out MemoryInfo info,nint length);
 // Bytes of private executable memory in regions over 1 MB (0 for a Minecraft without a manually mapped client).
 internal static long ForeignCode(int pid)
 {
  using var opened=OpenProcess(0x410,false,pid);if(opened.IsInvalid)return -1;ulong address=0;long total=0;
  while(VirtualQueryEx(opened,address,out var info,Marshal.SizeOf<MemoryInfo>())!=0)
  {if(info.State==0x1000&&info.Type==0x20000&&(info.Protect&0xF0)!=0&&info.Size>0x100000)total+=(long)info.Size;var next=info.Base+info.Size;if(next<=address)break;address=next;}
  return total;
 }
 [DllImport("kernel32.dll",SetLastError=true)] private static extern SafeProcessHandle OpenProcess(uint access,[MarshalAs(UnmanagedType.Bool)]bool inherit,int pid);
 [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]private static extern bool GetExitCodeProcess(SafeProcessHandle process,out uint code);
 // True when both paths name the same file (same volume and file index), whatever links or junctions lead to it.
 internal static bool SameFile(string first,string second)
 {
  if(string.Equals(Path.GetFullPath(first),Path.GetFullPath(second),StringComparison.OrdinalIgnoreCase))return true;
  static (uint,uint,uint)? Id(string path)
  {
   using var file=CreateFile(path,0x80,7,0,3,0x02000000,0);   // FILE_READ_ATTRIBUTES, share all, OPEN_EXISTING, BACKUP_SEMANTICS
   if(file.IsInvalid||!GetFileInformationByHandle(file,out var info))return null;
   return (info.VolumeSerialNumber,info.FileIndexHigh,info.FileIndexLow);
  }
  var a=Id(first);var b=Id(second);return a!=null&&a==b;
 }
 /// <summary>Offline check run by "Nickel.exe --self-test": null when SameFile passes, else what failed.</summary>
 internal static string? SelfTest()
 {
  var dir=Path.Combine(Path.GetTempPath(),$"nickel-samefile-{Guid.NewGuid():N}");Directory.CreateDirectory(dir);
  try
  {
   var first=Path.Combine(dir,"a.bin");var other=Path.Combine(dir,"b.bin");var link=Path.Combine(dir,"link.bin");
   File.WriteAllText(first,"a");File.WriteAllText(other,"a");
   if(!SameFile(first,first.ToUpperInvariant()))return "the same path in another case is refused";
   if(SameFile(first,other))return "two files with the same content are taken for one";
   if(SameFile(first,Path.Combine(dir,"missing.bin")))return "a missing file is taken for an existing one";
   if(CreateHardLink(link,first,0)&&!SameFile(first,link))return "two names of one file (hard link) are refused";
   return null;
  }
  finally{try{Directory.Delete(dir,true);}catch(IOException){}catch(UnauthorizedAccessException){}}
 }
 [DllImport("kernel32.dll",EntryPoint="CreateHardLinkW",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]private static extern bool CreateHardLink(string link,string existing,nint security);
 [StructLayout(LayoutKind.Sequential,Pack=4)] private struct FileInformation{public uint Attributes;public long Created,Accessed,Written;public uint VolumeSerialNumber,SizeHigh,SizeLow,Links,FileIndexHigh,FileIndexLow;}
 [DllImport("kernel32.dll",EntryPoint="CreateFileW",CharSet=CharSet.Unicode,SetLastError=true)] private static extern SafeFileHandle CreateFile(string path,uint access,uint share,nint security,uint disposition,uint flags,nint template);
 [DllImport("kernel32.dll",SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]private static extern bool GetFileInformationByHandle(SafeFileHandle file,out FileInformation info);
 [DllImport("kernel32.dll",EntryPoint="QueryFullProcessImageNameW",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]private static extern bool QueryFullProcessImageName(SafeProcessHandle process,uint flags,StringBuilder path,ref uint size);
}
