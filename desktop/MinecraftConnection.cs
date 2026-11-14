using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;

namespace Nickel.Desktop;
public sealed class MinecraftConnection : IDisposable
{
 public const string Version="1.26.62.01";
 public const string ExpectedHash="0fa33a61f4e8e7efa663197a39a6650070c742a88229a12478f347f819395415";
 public const int ExpectedImageSize=327405568;
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
   var path=module.FileName;var imageSize=module.ModuleMemorySize;var pid=process.Id;
   var opened=OpenProcess(0x410,false,pid); // QUERY_INFORMATION | VM_READ, never VM_WRITE.
   if(opened.IsInvalid){opened.Dispose();throw new Win32Exception(Marshal.GetLastWin32Error(),"Windows refused access to Minecraft.");}
   try
   {
    var actual=new StringBuilder(32768);var length=(uint)actual.Capacity;
    if(!QueryFullProcessImageName(opened,0,actual,ref length))throw new Win32Exception(Marshal.GetLastWin32Error());
    if(!string.Equals(Path.GetFullPath(path),Path.GetFullPath(actual.ToString()),StringComparison.OrdinalIgnoreCase))throw new InvalidOperationException("Minecraft changed during connection. Try again.");
    if(imageSize!=ExpectedImageSize)throw new InvalidOperationException($"This build supports Minecraft {Version}. Your running version differs.");
    using var file=new FileStream(path,FileMode.Open,FileAccess.Read,FileShare.Read);
    var digest=Convert.ToHexString(SHA256.HashData(file)).ToLowerInvariant();
    if(digest!=ExpectedHash)throw new InvalidOperationException($"Minecraft {Version} with the matching game file is required.");
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
 [DllImport("kernel32.dll",EntryPoint="QueryFullProcessImageNameW",CharSet=CharSet.Unicode,SetLastError=true)] [return:MarshalAs(UnmanagedType.Bool)]private static extern bool QueryFullProcessImageName(SafeProcessHandle process,uint flags,StringBuilder path,ref uint size);
}
