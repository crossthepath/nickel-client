// Release signing for Nickel.
//   keygen <private.pem>                       creates an ECDSA P-256 key; prints the public key to pin in ReleaseTrust.cs
//   sign <Nickel.exe> <version> <private.pem> <out-folder> [channel]
//                                              writes release-manifest.json and release-manifest.sig (base64, DER)
//   verify <out-folder>                        checks the pair against the key pinned in ReleaseTrust.cs
// Keep the private key out of the repository and backed up offline: a lost key means shipping a launcher with a new one.
using System.Security.Cryptography;
using System.Text;
using Nickel.Desktop;

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
 var exe = File.ReadAllBytes(args[1]);
 var manifest = new ReleaseManifest("Nickel", args.Length > 5 ? args[5] : "beta", args[2], "v" + args[2], "Nickel.exe", exe.LongLength,
  Convert.ToHexString(SHA256.HashData(exe)).ToLowerInvariant());
 var bytes = ReleaseTrust.Serialize(manifest);
 using var key = ECDsa.Create(); key.ImportFromPem(File.ReadAllText(args[3]));
 var signature = key.SignData(bytes, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence);
 Directory.CreateDirectory(args[4]);
 File.WriteAllBytes(Path.Combine(args[4], ReleaseTrust.ManifestName), bytes);
 File.WriteAllText(Path.Combine(args[4], ReleaseTrust.SignatureName), Convert.ToBase64String(signature));
 if (ReleaseTrust.Verify(bytes, Encoding.ASCII.GetBytes(Convert.ToBase64String(signature))) == null)
 { Console.Error.WriteLine("Signed, but the key pinned in ReleaseTrust.cs does not verify it (wrong key?)."); return 2; }
 Console.WriteLine(Encoding.UTF8.GetString(bytes));
 return 0;
}
if (args.Length >= 2 && args[0] == "verify")
{
 var m = ReleaseTrust.Verify(File.ReadAllBytes(Path.Combine(args[1], ReleaseTrust.ManifestName)), File.ReadAllBytes(Path.Combine(args[1], ReleaseTrust.SignatureName)));
 Console.WriteLine(m == null ? "INVALID" : $"valid: {m.Version} {m.Size} bytes sha256 {m.Sha256}");
 return m == null ? 3 : 0;
}
Console.Error.WriteLine("usage: keygen <private.pem> | sign <Nickel.exe> <version> <private.pem> <out-folder> [channel] | verify <folder>");
return 1;
