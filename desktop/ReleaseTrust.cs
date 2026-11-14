using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
namespace Nickel.Desktop;

/// <summary>Release manifest: what a signed release promises (the file, its exact size and SHA-256).</summary>
public sealed record ReleaseManifest(string Product, string Channel, string Version, string Tag, string File, long Size, string Sha256);

/// <summary>
/// Chain of trust for launcher updates. Each release ships release-manifest.json and release-manifest.sig (base64 DER
/// ECDSA P-256 / SHA-256 signature of the manifest bytes), made offline with tools/release-signer. The launcher only
/// installs a file whose manifest verifies against the public key pinned below, whose version is newer than its own,
/// and whose downloaded bytes match the manifest's size and SHA-256. A compromised server, CDN or release page can
/// therefore not make it install another file.
/// </summary>
public static class ReleaseTrust
{
 public const string ManifestName = "release-manifest.json";
 public const string SignatureName = "release-manifest.sig";
 public const string PublicKeyPem = """
-----BEGIN PUBLIC KEY-----
MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEGLSWUSO9B47ufXxClBLsGzJ/iWaW
nPyPtAPFFv7Bbrw0skygiWqt6xC0HMHVzYyFtKyPC7rek9TwENLS46xh3A==
-----END PUBLIC KEY-----
""";
 private const long MinSize = 1024 * 1024, MaxSize = 512L * 1024 * 1024;
 private static readonly string[] Hosts = ["nickelclient.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"];
 private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

 /// <summary>Canonical bytes of a manifest (what gets signed).</summary>
 public static byte[] Serialize(ReleaseManifest manifest) => JsonSerializer.SerializeToUtf8Bytes(manifest, Json);

 /// <summary>The manifest when <paramref name="signatureBase64"/> is a valid signature of exactly these bytes by the
 /// pinned key (or <paramref name="publicKeyPem"/>) and every field is well formed; otherwise null.</summary>
 public static ReleaseManifest? Verify(byte[] manifest, byte[] signatureBase64, string publicKeyPem = PublicKeyPem)
 {
  try
  {
   if (manifest.Length is 0 or > 4096) return null;
   var signature = Convert.FromBase64String(Encoding.ASCII.GetString(signatureBase64).Trim());
   using var key = ECDsa.Create(); key.ImportFromPem(publicKeyPem);
   if (!key.VerifyData(manifest, signature, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)) return null;
   var m = JsonSerializer.Deserialize<ReleaseManifest>(manifest, Json);
   if (m == null || m.Product != "Nickel" || m.File != "Nickel.exe" || m.Size < MinSize || m.Size > MaxSize) return null;
   if (m.Sha256.Length != 64 || !m.Sha256.All(Uri.IsHexDigit) || m.Tag != "v" + m.Version || Parse(m.Version) == null) return null;
   return m;
  }
  catch { return null; }
 }

 /// <summary>Downloads are taken over HTTPS from the project's own hosts only (checked after redirects too).</summary>
 public static bool AllowedSource(Uri? uri) => uri != null && uri.Scheme == Uri.UriSchemeHttps && Hosts.Contains(uri.Host, StringComparer.OrdinalIgnoreCase);

 /// <summary>True when the file has exactly the manifest's size and SHA-256.</summary>
 public static bool Matches(string path, ReleaseManifest manifest)
 {
  using var file = File.OpenRead(path);
  if (file.Length != manifest.Size) return false;
  return string.Equals(Convert.ToHexString(SHA256.HashData(file)), manifest.Sha256, StringComparison.OrdinalIgnoreCase);
 }

 /// <summary>Offline checks run by "Nickel.exe --self-test": null when they pass, else what failed.</summary>
 public static string? SelfTest()
 {
  using (var pinned = ECDsa.Create()) pinned.ImportFromPem(PublicKeyPem);   // the pinned key parses
  using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256); using var other = ECDsa.Create(ECCurve.NamedCurves.nistP256);
  var pub = key.ExportSubjectPublicKeyInfoPem();
  var path = Path.Combine(Path.GetTempPath(), $"nickel-trust-{Guid.NewGuid():N}.bin");
  try
  {
   var body = new byte[MinSize + 17]; RandomNumberGenerator.Fill(body); body[0] = (byte)'M'; body[1] = (byte)'Z'; File.WriteAllBytes(path, body);
   var m = new ReleaseManifest("Nickel", "beta", "1.0.1-beta", "v1.0.1-beta", "Nickel.exe", body.Length, Convert.ToHexString(SHA256.HashData(body)).ToLowerInvariant());
   var bytes = Serialize(m);
   byte[] Sig(ECDsa k, byte[] data) => Encoding.ASCII.GetBytes(Convert.ToBase64String(k.SignData(data, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)));
   if (Verify(bytes, Sig(key, bytes), pub) is not { } ok || ok != m) return "an authentic manifest is refused";
   if (!Matches(path, m)) return "an authentic file is refused";
   var forged = (byte[])bytes.Clone(); forged[^3] ^= 1;
   if (Verify(forged, Sig(key, bytes), pub) != null) return "a modified manifest is accepted";
   if (Verify(bytes, Sig(other, bytes), pub) != null) return "a manifest signed by another key is accepted";
   if (Verify(bytes, Encoding.ASCII.GetBytes("not a signature"), pub) != null) return "a garbage signature is accepted";
   body[body.Length / 2] ^= 1; File.WriteAllBytes(path, body);
   if (Matches(path, m)) return "a file modified by one byte is accepted";
   File.WriteAllBytes(path, body.AsSpan(0, body.Length - 1).ToArray());
   if (Matches(path, m)) return "a truncated file is accepted";
   if (AllowedSource(new Uri("http://nickelclient.com/download")) || AllowedSource(new Uri("https://nickelclient.com.evil.example/x")) || !AllowedSource(new Uri("https://github.com/x")))
    return "the download host policy is wrong";
   string[] order = ["0.9.9", "1.0.0-alpha", "1.0.0-beta", "1.0.0-beta.1", "1.0.0-beta.2", "1.0.0-beta.10", "1.0.0-rc.1", "1.0.0", "v1.0.1-beta", "1.0.1", "1.10.0"];
   for (int i = 0; i + 1 < order.Length; ++i) if (Compare(order[i], order[i + 1]) >= 0 || Compare(order[i + 1], order[i]) <= 0) return $"version order: {order[i]} / {order[i + 1]}";
   if (Compare("1.0.0-beta", "v1.0.0-beta") != 0) return "version order: v prefix";
   return null;
  }
  finally { try { File.Delete(path); } catch { } }
 }

 // ---- versions: "1.2.3" with an optional pre-release ("-beta", "-beta.2"), ordered as Semantic Versioning 2.0
 private static (int[] Core, string[] Pre)? Parse(string version)
 {
  var text = version.Trim().TrimStart('v', 'V'); var plus = text.IndexOf('+'); if (plus >= 0) text = text[..plus];
  var dash = text.IndexOf('-'); var core = (dash < 0 ? text : text[..dash]).Split('.');
  if (core.Length != 3) return null;
  var numbers = new int[3];
  for (int i = 0; i < 3; ++i) if (!int.TryParse(core[i], out numbers[i]) || numbers[i] < 0) return null;
  var pre = dash < 0 ? [] : text[(dash + 1)..].Split('.');
  if (pre.Any(p => p.Length == 0)) return null;
  return (numbers, pre);
 }
 /// <summary>Negative, zero or positive as <paramref name="a"/> is older, equal or newer than <paramref name="b"/>
 /// (an unreadable version sorts first).</summary>
 public static int Compare(string a, string b)
 {
  var x = Parse(a); var y = Parse(b);
  if (x == null || y == null) return x == null ? (y == null ? 0 : -1) : 1;
  for (int i = 0; i < 3; ++i) if (x.Value.Core[i] != y.Value.Core[i]) return x.Value.Core[i].CompareTo(y.Value.Core[i]);
  var p = x.Value.Pre; var q = y.Value.Pre;
  if (p.Length == 0 || q.Length == 0) return q.Length.CompareTo(p.Length);   // a release sorts after its pre-releases
  for (int i = 0; i < Math.Min(p.Length, q.Length); ++i)
  {
   var pn = int.TryParse(p[i], out var pi); var qn = int.TryParse(q[i], out var qi);
   var c = pn && qn ? pi.CompareTo(qi) : pn ? -1 : qn ? 1 : string.CompareOrdinal(p[i], q[i]);
   if (c != 0) return c;
  }
  return p.Length.CompareTo(q.Length);
 }
}
