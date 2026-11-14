// Release signing for Nickel.
//   keygen <private.pem>                       creates an ECDSA P-256 key; prints the public key to pin in ReleaseTrust.cs
//   sign <Nickel.exe> <version> <private.pem> <out-folder> [channel]
//                                              writes release-manifest.json and release-manifest.sig (base64, DER)
//   verify <out-folder> [Nickel.exe]           checks the pair against the key pinned in ReleaseTrust.cs (and the file)
//   self-test                                  runs the launcher's offline update checks
// Keep the private key out of the repository and backed up offline: a lost key means shipping a launcher with a new one.
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using Nickel.Desktop;

if (args.Length == 1 && args[0] == "self-test")   // the launcher's update checks, without the launcher (CI, offline)
{
 var failure = ReleaseTrust.SelfTest();
 Console.WriteLine(failure == null ? "update checks passed" : "FAILED: " + failure);
 return failure == null ? 0 : 4;
}
if (args.Length >= 2 && args[0] == "keygen")
{
 if (File.Exists(args[1])) { Console.Error.WriteLine("Refusing to overwrite an existing key."); return 1; }
 using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256);
 File.WriteAllText(args[1], key.ExportPkcs8PrivateKeyPem());
 Console.WriteLine(key.ExportSubjectPublicKeyInfoPem());
 return 0;
}
if (args.Length >= 5 && args[0] == "sign")
{
 var (exePath, version, keyPath, outDir) = (args[1], args[2], args[3], args[4]);
 // the EXE must report the version being signed, or the updated launcher would see the same update again forever
 var exeVersion = (FileVersionInfo.GetVersionInfo(exePath).ProductVersion ?? "").Split('+')[0];
 if (exeVersion != version) { Console.Error.WriteLine($"{exePath} reports version \"{exeVersion}\", not \"{version}\" (set <Version> in desktop/Nickel.Desktop.csproj and rebuild)."); return 2; }
 var exe = File.ReadAllBytes(exePath);
 var manifest = new ReleaseManifest("Nickel", args.Length > 5 ? args[5] : "beta", version, "v" + version, "Nickel.exe", exe.LongLength,
  Convert.ToHexString(SHA256.HashData(exe)).ToLowerInvariant());
 if (ReleaseTrust.Problem(manifest) is string problem) { Console.Error.WriteLine($"Not signed: {problem}."); return 2; }
 var bytes = ReleaseTrust.Serialize(manifest);
 using var key = ECDsa.Create(); key.ImportFromPem(File.ReadAllText(keyPath));
 var signature = Encoding.ASCII.GetBytes(Convert.ToBase64String(key.SignData(bytes, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)));
 // verified with the pinned key before anything is written: a wrong key leaves no publishable files behind
 if (ReleaseTrust.Verify(bytes, signature) == null) { Console.Error.WriteLine("Not written: this private key does not match the public key pinned in desktop/ReleaseTrust.cs."); return 2; }
 Directory.CreateDirectory(outDir);
 File.WriteAllBytes(Path.Combine(outDir, ReleaseTrust.ManifestName), bytes);
 File.WriteAllBytes(Path.Combine(outDir, ReleaseTrust.SignatureName), signature);
 Console.WriteLine(Encoding.UTF8.GetString(bytes));
 return 0;
}
if (args.Length >= 2 && args[0] == "verify")
{
 var m = ReleaseTrust.Verify(File.ReadAllBytes(Path.Combine(args[1], ReleaseTrust.ManifestName)), File.ReadAllBytes(Path.Combine(args[1], ReleaseTrust.SignatureName)));
 if (m == null) { Console.WriteLine("INVALID: signature, key or manifest fields"); return 3; }
 if (args.Length >= 3 && !ReleaseTrust.Matches(args[2], m)) { Console.WriteLine($"INVALID: {args[2]} does not match the manifest's size and SHA-256"); return 3; }
 Console.WriteLine($"valid: {m.Version} {m.Size} bytes sha256 {m.Sha256}");
 return 0;
}
Console.Error.WriteLine("usage: keygen <private.pem> | sign <Nickel.exe> <version> <private.pem> <out-folder> [channel] | verify <folder> [Nickel.exe]");
return 1;
