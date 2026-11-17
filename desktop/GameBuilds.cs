using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32.SafeHandles;

namespace Nickel.Desktop;

/// <summary>A section of a validated game build: where it sits, how large it is and the SHA-256 of its content in the
/// file (the bytes the section is loaded from).</summary>
public sealed record GameSection(string Name, uint Address, uint Size, string Sha256);

/// <summary>A PE data directory (index in the optional header, address, size) that must match the validated build.</summary>
public sealed record GameDirectory(int Index, uint Address, uint Size);

/// <summary>A Minecraft build Nickel was validated against: every offset, hook and structure of the overlay is checked
/// against it. A game that holds none of them but has a version GameBuilds.Adaptable() accepts is loaded too: the
/// overlay then finds its game functions itself (native/game_resolve.hpp).</summary>
public sealed record GameBuild(string Name, int Major, int Minor, int Build, uint EntryPoint, GameDirectory[] Directories, GameSection[] Sections);

/// <summary>
/// Identifies the game build of a Minecraft.Windows.exe. A file holds a validated build when its code (.text), constants
/// (.rdata), initial data (.data), unwind table (.pdata) and relocations (.reloc) sit where the build has them with the
/// build's sizes and bytes, and when its entry point and the directories that decide what runs or is rewritten at load
/// (exports, imports, exception table, relocations, TLS callbacks, load configuration, import address table, delayed
/// imports, .NET header) are the build's. What may differ: signature, resources, debug data, other header fields and added sections, which
/// installations from the Microsoft Store / Xbox app, LeviLauncher or other launchers change without moving anything the
/// overlay depends on. The code loaded in the process is then compared with the build (MemoryCodeMatches): a file
/// swapped after the game started or code patched at start-up is not that build, and the overlay adapts to it like to
/// any other copy of an adaptable version. Keep in step with native/game_build.hpp.
/// </summary>
public static class GameBuilds
{
 public static readonly GameBuild[] Validated =
 [
  new("Minecraft 1.26.62.1", 1, 26, 62, 0xeea5e90,
  [
   new(0, 0xf9a4468, 0x162), new(1, 0xf9a45cc, 0x6f4), new(3, 0x12bdf000, 0xb59b60), new(5, 0x13742000, 0xfa3c4),
   new(9, 0xf4f39c0, 0x28), new(10, 0xefc6990, 0x140), new(11, 0, 0), new(12, 0xf9a6f20, 0x2260), new(13, 0, 0), new(14, 0, 0),
  ],
  [
   new(".text", 0x1000, 0xeec65d6, "a3443f50ff150994fc0efd0dc35aceb2409d45dd1cd03111bd1bbfe8de4eca15"),
   new(".rdata", 0xeec8000, 0x3441284, "6a52b8635beb1c28af46b43b61934788edb4a033bdd52b2ef5c46273db960b2a"),
   new(".data", 0x1230a000, 0x8d4b68, "6039c0c55e0908235f6fbbfcaf6693ce4b02bdc2cddd7ba2ed8cfde747909f7c"),
   new(".pdata", 0x12bdf000, 0xb59b60, "843683dccddf06209a5a16a54516c6eeda5ba8c42fbde899a09aaa3ff923bb02"),
   new(".reloc", 0x13742000, 0xfa3c4, "3130b70506c2d712e5fce5dca4251db1c982630874da1c2a2f57eb6a6acde9a5"),
  ]),
  // Same game code as 1.26.62.1 (every function Nickel uses is identical), relinked: the overlay translates its
  // addresses (native/generated_build_1_26_63_1.hpp, tools/port_build.py).
  new("Minecraft 1.26.63.1", 1, 26, 63, 0xeea6340,
  [
   new(0, 0xf9a4438, 0x162), new(1, 0xf9a459c, 0x6f4), new(3, 0x12bdf000, 0xb59b78), new(5, 0x13742000, 0xfa3c4),
   new(9, 0xf4f39d0, 0x28), new(10, 0xefc6990, 0x140), new(11, 0, 0), new(12, 0xf9a6ef0, 0x2260), new(13, 0, 0), new(14, 0, 0),
  ],
  [
   new(".text", 0x1000, 0xeec6a86, "59b8c727d734a5197e7dd298bb7a939c0dc6b47fe5072d1af3518793a76a7995"),
   new(".rdata", 0xeec8000, 0x34412bc, "ceff25ce8c650ebfd49c15292797f655ffb65a26e995b315131d1bec1e419be3"),
   new(".data", 0x1230a000, 0x8d4b68, "91d273db34e5e4403abe4272a2630ebe16750133c55b161d77246234d8d9a4f7"),
   new(".pdata", 0x12bdf000, 0xb59b78, "99ef9e1fcb99c725250d6953c9a5178a7736c4b7da9a105655648076243187e4"),
   new(".reloc", 0x13742000, 0xfa3c4, "4dfd51dff270c7a698325d00b229bfb83d8f9b48b8a2e2eb943fc545f01214cd"),
  ]),
  // The Microsoft Store / Xbox app update of November 2026: same game code again, relinked
  // (native/generated_build_1_26_65_1.hpp).
  new("Minecraft 1.26.65.1", 1, 26, 65, 0xeea6e80,
  [
   new(0, 0xf9a54d8, 0x162), new(1, 0xf9a563c, 0x6f4), new(3, 0x12be0000, 0xb59ba8), new(5, 0x13743000, 0xfa3a4),
   new(9, 0xf4f4a50, 0x28), new(10, 0xefc7990, 0x140), new(11, 0, 0), new(12, 0xf9a7f90, 0x2260), new(13, 0, 0), new(14, 0, 0),
  ],
  [
   new(".text", 0x1000, 0xeec75c6, "7e23c77b2f7b5a06b6cf1ba881980a5e61f3e2af0ab56597cbf63c6a2e77b2d3"),
   new(".rdata", 0xeec9000, 0x344143c, "d5d53f75445c4ca7a13b51fb2d1c4bf5af7379a7907b76a0f13eb28ee10e3efb"),
   new(".data", 0x1230b000, 0x8d4b68, "1efbe5d55e5d450004973b0640a7c75ed2969a44d65b5e3659943d8700a4f608"),
   new(".pdata", 0x12be0000, 0xb59ba8, "48b914569d08b5ad54d374ec4297c47a84bb3bf3bf084fda560e65f5615d2371"),
   new(".reloc", 0x13743000, 0xfa3a4, "be9b11b103e56b672f9e70f36b4de99e4ebc0a7ce84ac2a876b81b57c7a5e4b8"),
  ]),
 ];

 /// <summary>The validated game versions for display, for example "1.26.62 / 1.26.63 / 1.26.65".</summary>
 public static string SupportedVersions => string.Join(" / ", Validated.Select(b => $"{b.Major}.{b.Minor}.{b.Build}").Distinct());

 /// <summary>True for the file name of the game's executable (any folder).</summary>
 public static bool IsGameImage(string path) => string.Equals(Path.GetFileName(path), "Minecraft.Windows.exe", StringComparison.OrdinalIgnoreCase);

 /// <summary>True when a file version (major.minor.build) is the version of one of the given builds.</summary>
 public static bool VersionValidated(int major, int minor, int build, IReadOnlyList<GameBuild> builds) => builds.Any(b => b.Major == major && b.Minor == minor && b.Build == build);

 private sealed record FileSection(uint Address, uint Size, uint RawSize, uint RawOffset);
 private sealed record FileImage(uint EntryPoint, (uint Address, uint Size)[] Directories, Dictionary<string, FileSection> Sections);

 /// <summary>The validated build a game image (read from a stream) holds, or null with the first reason it does not.</summary>
 public static GameBuild? Identify(Stream file, IReadOnlyList<GameBuild> builds, out string reason)
 {
  reason = "not a 64-bit Windows program";
  var image = ReadImage(file);
  if (image == null) return null;
  foreach (var build in builds)
  {
   var miss = Mismatch(file, image, build);
   if (miss == null) { reason = ""; return build; }
   reason = miss;
  }
  return null;
 }

 private static string? Mismatch(Stream file, FileImage image, GameBuild build)
 {
  if (image.EntryPoint != build.EntryPoint) return "its entry point differs";
  foreach (var d in build.Directories)
   if (image.Directories[d.Index] != (d.Address, d.Size)) return $"its data directory {d.Index} differs";
  foreach (var want in build.Sections)
  {
   if (!image.Sections.TryGetValue(want.Name, out var have)) return $"no {want.Name} section";
   if (have.Address != want.Address || have.Size != want.Size) return $"{want.Name} is laid out differently";
   if (have.RawOffset % 0x200 != 0) return $"{want.Name} is not aligned";   // the loader rounds the file offset down
   var length = Math.Min(have.Size, have.RawSize);
   if ((long)have.RawOffset + length > file.Length) return $"{want.Name} is truncated";
   if (!string.Equals(Hash(file, have.RawOffset, length), want.Sha256, StringComparison.OrdinalIgnoreCase)) return $"{want.Name} holds different bytes";
  }
  return null;
 }

 /// <summary>True for the versions the overlay adapts to when the game file holds no validated build (a launcher's copy
 /// changed elsewhere, a relinked hotfix): it then finds every game function Nickel uses itself and keeps game features
 /// off when one differs (native/game_resolve.hpp, AdaptableVersion; keep both in step).</summary>
 public static bool Adaptable(int major, int minor, int build) => major == 1 && minor == 26 && build >= 60 && build <= 69;

 /// <summary>Checks a game image file: null when Nickel can load into it, else the refusal. build is the validated build
 /// the file holds; null with a version Adaptable() means the overlay will look for Nickel's game functions itself, and
 /// note then says why the file is not a validated build.</summary>
 public static string? Check(string path, IReadOnlyList<GameBuild> builds, out GameBuild? build, out string note)
 {
  build = null; note = "";
  if (!IsGameImage(path)) return "This process is not Minecraft.Windows.exe.";
  var info = FileVersionInfo.GetVersionInfo(path);
  var version = $"{info.FileMajorPart}.{info.FileMinorPart}.{info.FileBuildPart}.{info.FilePrivatePart}";
  var validated = VersionValidated(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart, builds);
  if (!validated && !Adaptable(info.FileMajorPart, info.FileMinorPart, info.FileBuildPart))
   return $"Nickel supports Minecraft 1.26.60 to 1.26.69 (tested: {SupportedVersions}); this game is {version}.";
  var sameVersion = builds.Where(b => b.Major == info.FileMajorPart && b.Minor == info.FileMinorPart && b.Build == info.FileBuildPart).ToArray();
  using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
  build = Identify(file, sameVersion, out var reason);
  if (build == null) note = sameVersion.Length == 0 ? $"Minecraft {version} has no profile yet" : $"this Minecraft {version} is not the tested build ({reason})";
  return null;
 }

 /// <summary>True when the code (.text) loaded at imageBase in the process is byte for byte the build's code (it has no
 /// relocations, so the loaded bytes equal the file's). False when it cannot be read or differs, for example because
 /// the file was replaced after the game started or something patched the code.</summary>
 public static bool MemoryCodeMatches(SafeProcessHandle process, ulong imageBase, GameBuild build)
 {
  var text = build.Sections.First(s => s.Name == ".text");
  using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256); var buffer = new byte[1 << 20];
  for (ulong done = 0; done < text.Size;)
  {
   var want = (int)Math.Min((ulong)buffer.Length, text.Size - done);
   if (!ReadProcessMemory(process, imageBase + text.Address + done, buffer, (nint)want, out var read) || read != want) return false;
   hash.AppendData(buffer, 0, want); done += (ulong)want;
  }
  return string.Equals(Convert.ToHexString(hash.GetHashAndReset()), text.Sha256, StringComparison.OrdinalIgnoreCase);
 }
 [DllImport("kernel32.dll", SetLastError = true)] [return: MarshalAs(UnmanagedType.Bool)] private static extern bool ReadProcessMemory(SafeProcessHandle process, ulong address, byte[] buffer, nint size, out nint read);

 private static string Hash(Stream file, long offset, long length)
 {
  using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256); var buffer = new byte[1 << 20]; file.Position = offset;
  while (length > 0) { var read = file.Read(buffer, 0, (int)Math.Min(buffer.Length, length)); if (read <= 0) break; hash.AppendData(buffer, 0, read); length -= read; }
  return length == 0 ? Convert.ToHexString(hash.GetHashAndReset()) : "";
 }

 private static FileImage? ReadImage(Stream file)
 {
  if (file.Length < 0x200) return null;
  var reader = new BinaryReader(file);
  file.Position = 0; if (reader.ReadUInt16() != 0x5a4d) return null;
  file.Position = 0x3c; var nt = reader.ReadInt32(); if (nt <= 0 || nt > 0x1000 || nt + 0x108L > file.Length) return null;
  file.Position = nt; if (reader.ReadUInt32() != 0x4550 || reader.ReadUInt16() != 0x8664) return null;   // "PE\0\0", x64
  var count = reader.ReadUInt16(); file.Position = nt + 20; var optional = reader.ReadUInt16();
  if (optional < 0xf0) return null;   // a PE32+ optional header with its 16 data directories
  file.Position = nt + 24; if (reader.ReadUInt16() != 0x20b) return null;   // PE32+
  file.Position = nt + 24 + 16; var entry = reader.ReadUInt32();
  file.Position = nt + 24 + 108; if (reader.ReadUInt32() < 16) return null;   // NumberOfRvaAndSizes
  var directories = new (uint, uint)[16];
  for (var i = 0; i < 16; ++i) directories[i] = (reader.ReadUInt32(), reader.ReadUInt32());
  long table = nt + 24 + optional; if (count == 0 || count > 96 || table + 40L * count > file.Length) return null;
  var sections = new Dictionary<string, FileSection>();
  for (var i = 0; i < count; ++i)
  {
   file.Position = table + 40L * i; var name = System.Text.Encoding.ASCII.GetString(reader.ReadBytes(8)).TrimEnd('\0');
   var size = reader.ReadUInt32(); var address = reader.ReadUInt32(); var rawSize = reader.ReadUInt32(); var rawOffset = reader.ReadUInt32();
   if (!sections.TryAdd(name, new(address, size, rawSize, rawOffset))) return null;   // a duplicated section name is not a game build
  }
  return new(entry, directories, sections);
 }

 /// <summary>Offline checks run by "Nickel.exe --self-test" on synthetic images: null when they pass, else what failed.</summary>
 public static string? SelfTest()
 {
  // A small synthetic program: .text / .rdata / .data / .reloc with known bytes, an entry point and directories, and
  // the build profile made from it.
  const int nt = 0x80, table0 = nt + 24 + 0xf0, dirs = nt + 24 + 112;
  static byte[] Image(Action<byte[]>? edit = null, bool extraSection = false, uint textAddress = 0x1000)
  {
   var image = new byte[0x1000 + 0x800]; image[0] = 0x4d; image[1] = 0x5a; BitConverter.GetBytes(nt).CopyTo(image, 0x3c);
   "PE\0\0"u8.ToArray().CopyTo(image, nt); BitConverter.GetBytes((ushort)0x8664).CopyTo(image, nt + 4);
   BitConverter.GetBytes((ushort)(extraSection ? 5 : 4)).CopyTo(image, nt + 6); BitConverter.GetBytes((ushort)0xf0).CopyTo(image, nt + 20);
   BitConverter.GetBytes((ushort)0x20b).CopyTo(image, nt + 24); BitConverter.GetBytes(0x1010u).CopyTo(image, nt + 24 + 16);
   BitConverter.GetBytes(16u).CopyTo(image, nt + 24 + 108);
   BitConverter.GetBytes(0x2010u).CopyTo(image, dirs + 8); BitConverter.GetBytes(0x28u).CopyTo(image, dirs + 12);       // imports
   BitConverter.GetBytes(0x4000u).CopyTo(image, dirs + 5 * 8); BitConverter.GetBytes(0x20u).CopyTo(image, dirs + 5 * 8 + 4);   // relocations
   void Section(int i, string name, uint address, uint size, uint raw, byte fill)
   {
    var at = table0 + 40 * i; System.Text.Encoding.ASCII.GetBytes(name).CopyTo(image, at);
    BitConverter.GetBytes(size).CopyTo(image, at + 8); BitConverter.GetBytes(address).CopyTo(image, at + 12);
    BitConverter.GetBytes(0x200u).CopyTo(image, at + 16); BitConverter.GetBytes(raw).CopyTo(image, at + 20);
    for (var k = 0; k < 0x200; ++k) image[raw + k] = (byte)(fill + k);
   }
   Section(0, ".text", textAddress, 0x1f0, 0x400, 0x10); Section(1, ".rdata", 0x2000, 0x200, 0x600, 0x40);
   Section(2, ".data", 0x3000, 0x400, 0x800, 0x70); Section(3, ".reloc", 0x4000, 0x20, 0xa00, 0x30);
   if (extraSection) Section(4, ".launch", 0x5000, 0x200, 0xc00, 0x99);
   edit?.Invoke(image);
   return image;
  }
  static string Sha(byte[] image, int offset, int length) => Convert.ToHexString(SHA256.HashData(image.AsSpan(offset, length)));
  var good = Image();
  var builds = new[] { new GameBuild("test", 1, 26, 62, 0x1010, [new(1, 0x2010, 0x28), new(5, 0x4000, 0x20), new(9, 0, 0)],
   [new(".text", 0x1000, 0x1f0, Sha(good, 0x400, 0x1f0)), new(".rdata", 0x2000, 0x200, Sha(good, 0x600, 0x200)),
    new(".data", 0x3000, 0x400, Sha(good, 0x800, 0x200)), new(".reloc", 0x4000, 0x20, Sha(good, 0xa00, 0x20))]) };
  bool Accepts(byte[] image) { using var s = new MemoryStream(image); return Identify(s, builds, out _) != null; }
  if (!Accepts(good)) return "the validated build is refused";
  if (!Accepts(Image(i => { i[0x3a] = 0x77; i[0x1000] = 0x42; BitConverter.GetBytes(0x7000u).CopyTo(i, dirs + 4 * 8); }, extraSection: true)))
   return "the same build with another signature directory, other header bytes and an added section is refused";
  if (Accepts(Image(i => i[0x400 + 0x80] ^= 1))) return "a build whose code (.text) differs by one byte is accepted";
  if (Accepts(Image(i => i[0x600 + 0x10] ^= 1))) return "a build whose constants (.rdata) differ is accepted";
  if (Accepts(Image(i => i[0x800 + 0x20] ^= 1))) return "a build whose data (.data) differ is accepted";
  if (Accepts(Image(i => i[0xa00 + 0x4] ^= 1))) return "a build whose relocations (.reloc) differ is accepted";
  if (Accepts(Image(i => BitConverter.GetBytes(0x1020u).CopyTo(i, nt + 24 + 16)))) return "a build with another entry point is accepted";
  if (Accepts(Image(i => BitConverter.GetBytes(0x5000u).CopyTo(i, dirs + 8), extraSection: true))) return "a build with its imports moved to an added section is accepted";
  if (Accepts(Image(i => BitConverter.GetBytes(0x5000u).CopyTo(i, dirs + 9 * 8), extraSection: true))) return "a build with added TLS callbacks is accepted";
  if (Accepts(Image(i => BitConverter.GetBytes(0x404u).CopyTo(i, table0 + 20)))) return "a build with an unaligned code offset is accepted";
  if (Accepts(Image(textAddress: 0x1400))) return "a build with its code at another address is accepted";
  if (Accepts(Image(i => i[table0 + 8] = 0xf8))) return "a build with a different code size is accepted";
  if (Accepts(Image(i => System.Text.Encoding.ASCII.GetBytes(".rdat2").CopyTo(i, table0 + 40)))) return "a build without .rdata is accepted";
  if (Accepts(Image(i => System.Text.Encoding.ASCII.GetBytes(".text\0\0\0").CopyTo(i, table0 + 4 * 40), extraSection: true))) return "a build with two .text sections is accepted";
  if (Accepts(good[..0x700])) return "a truncated file is accepted";
  if (Accepts(Image(i => i[nt + 4] = 0x4c))) return "a 32-bit program is accepted";
  if (Accepts(new byte[0x2000])) return "a file that is not a program is accepted";
  if (!IsGameImage(@"C:\XboxGames\Minecraft for Windows\Content\Minecraft.Windows.exe") || IsGameImage(@"C:\x\Minecraft.Windows.exe.bak") || IsGameImage(@"C:\x\Notepad.exe"))
   return "the game image name rule is wrong";
  if (!VersionValidated(1, 26, 62, builds) || VersionValidated(1, 26, 63, builds) || VersionValidated(1, 25, 62, builds)) return "the version rule is wrong";
  if (!Adaptable(1, 26, 60) || !Adaptable(1, 26, 65) || !Adaptable(1, 26, 69) || Adaptable(1, 26, 59) || Adaptable(1, 26, 70) || Adaptable(1, 21, 62))
   return "the adaptable version rule is wrong";
  // The in-memory comparison on this launcher's own process: a buffer holding the profile's code is accepted, and the
  // same buffer with one byte changed is refused.
  var code = new byte[0x1000 + 0x1f0]; good.AsSpan(0x400, 0x1f0).CopyTo(code.AsSpan(0x1000));
  var pinned = GCHandle.Alloc(code, GCHandleType.Pinned);
  try
  {
   using var current = Process.GetCurrentProcess(); using var self = new SafeProcessHandle(current.Handle, false);
   var at = (ulong)pinned.AddrOfPinnedObject();
   if (!MemoryCodeMatches(self, at, builds[0])) return "loaded code equal to the build is refused";
   code[0x1000 + 0x33] ^= 1;
   if (MemoryCodeMatches(self, at, builds[0])) return "loaded code that differs from the build is accepted";
  }
  finally { pinned.Free(); }
  return null;
 }
}
