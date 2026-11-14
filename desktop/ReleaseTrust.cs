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
 public const long MinSize = 1024 * 1024, MaxSize = 512L * 1024 * 1024;
 public const int MaxManifestBytes = 4096;
 private static readonly string[] Hosts = ["nickelclient.com", "github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com"];
 private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

 /// <summary>Canonical bytes of a manifest (what gets signed).</summary>
 public static byte[] Serialize(ReleaseManifest manifest) => JsonSerializer.SerializeToUtf8Bytes(manifest, Json);

 /// <summary>Why a manifest is not acceptable as a release, or null when its fields are well formed.</summary>
 public static string? Problem(ReleaseManifest m)
 {
  if (m.Product != "Nickel" || m.File != "Nickel.exe") return "product or file name is not Nickel / Nickel.exe";
  if (m.Size < MinSize || m.Size > MaxSize) return $"size {m.Size} is outside {MinSize}..{MaxSize}";
  if (m.Sha256.Length != 64 || !m.Sha256.All(Uri.IsHexDigit)) return "sha256 is not 64 hex digits";
  if (!IsVersion(m.Version) || m.Version.StartsWith('v') || m.Version.StartsWith('V')) return $"version \"{m.Version}\" is not a Semantic Version";
  if (m.Tag != "v" + m.Version) return "tag is not \"v\" + version";
  return null;
 }

 /// <summary>The manifest when <paramref name="signatureBase64"/> is a valid signature of exactly these bytes by the
 /// pinned key (or <paramref name="publicKeyPem"/>) and every field is well formed; otherwise null.</summary>
 public static ReleaseManifest? Verify(byte[] manifest, byte[] signatureBase64, string publicKeyPem = PublicKeyPem)
 {
  try
  {
   if (manifest.Length is 0 or > MaxManifestBytes || signatureBase64.Length > 1024) return null;
   var signature = Convert.FromBase64String(Encoding.ASCII.GetString(signatureBase64).Trim());
   using var key = ECDsa.Create(); key.ImportFromPem(publicKeyPem);
   if (key.KeySize != 256 || !key.VerifyData(manifest, signature, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)) return null;
   var m = JsonSerializer.Deserialize<ReleaseManifest>(manifest, Json);
   return m != null && Problem(m) == null ? m : null;
  }
  catch { return null; }
 }

 /// <summary>The newest of the verified manifests when it is newer than <paramref name="current"/>, else null.
 /// Every source is considered, so one stale or replayed source cannot hide a newer release.</summary>
 public static ReleaseManifest? Newest(IEnumerable<ReleaseManifest> verified, string current)
 {
  ReleaseManifest? best = null;
  foreach (var m in verified) if (best == null || Compare(m.Version, best.Version) > 0) best = m;
  return best != null && Compare(best.Version, current) > 0 ? best : null;
 }

 /// <summary>Downloads are taken over HTTPS from the project's own hosts only (checked after redirects too).</summary>
 public static bool AllowedSource(Uri? uri) => uri != null && uri.Scheme == Uri.UriSchemeHttps && uri.UserInfo.Length == 0 && Hosts.Contains(uri.Host, StringComparer.OrdinalIgnoreCase);

 /// <summary>True when the file has exactly the manifest's size and SHA-256.</summary>
 public static bool Matches(string path, ReleaseManifest manifest)
 {
  using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
  if (file.Length != manifest.Size) return false;
  return HashMatches(SHA256.HashData(file), manifest);
 }
 public static bool HashMatches(byte[] sha256, ReleaseManifest manifest) => string.Equals(Convert.ToHexString(sha256), manifest.Sha256, StringComparison.OrdinalIgnoreCase);

 // ---- versions: Semantic Versioning 2.0 ("1.2.3", "1.2.3-beta.2", "+build" ignored), with an optional leading "v"
 private static bool Digits(string s) => s.Length > 0 && s.All(char.IsAsciiDigit);
 private static bool Numeric(string s) => Digits(s) && (s.Length == 1 || s[0] != '0');   // no leading zeros
 private static (string[] Core, string[] Pre)? Parse(string version)
 {
  var text = version.StartsWith('v') || version.StartsWith('V') ? version[1..] : version;
  var plus = text.IndexOf('+');
  if (plus >= 0) { var build = text[(plus + 1)..].Split('.'); if (build.Any(b => b.Length == 0 || !b.All(c => char.IsAsciiLetterOrDigit(c) || c == '-'))) return null; text = text[..plus]; }
  var dash = text.IndexOf('-'); var core = (dash < 0 ? text : text[..dash]).Split('.');
  if (core.Length != 3 || !core.All(Numeric)) return null;
  var pre = dash < 0 ? [] : text[(dash + 1)..].Split('.');
  foreach (var p in pre) if (p.Length == 0 || !p.All(c => char.IsAsciiLetterOrDigit(c) || c == '-') || (Digits(p) && !Numeric(p))) return null;
  return (core, pre);
 }
 public static bool IsVersion(string version) => Parse(version) != null;
 private static int CompareNumbers(string a, string b) => a.Length != b.Length ? a.Length.CompareTo(b.Length) : string.CompareOrdinal(a, b);
 /// <summary>Negative, zero or positive as <paramref name="a"/> is older, equal or newer than <paramref name="b"/>
 /// (an unreadable version sorts first).</summary>
 public static int Compare(string a, string b)
 {
  var x = Parse(a); var y = Parse(b);
  if (x == null || y == null) return x == null ? (y == null ? 0 : -1) : 1;
  for (int i = 0; i < 3; ++i) { var c = CompareNumbers(x.Value.Core[i], y.Value.Core[i]); if (c != 0) return Math.Sign(c); }
  var p = x.Value.Pre; var q = y.Value.Pre;
  if (p.Length == 0 || q.Length == 0) return q.Length.CompareTo(p.Length);   // a release sorts after its pre-releases
  for (int i = 0; i < Math.Min(p.Length, q.Length); ++i)
  {
   bool pn = Digits(p[i]), qn = Digits(q[i]);
   var c = pn && qn ? CompareNumbers(p[i], q[i]) : pn ? -1 : qn ? 1 : string.CompareOrdinal(p[i], q[i]);
   if (c != 0) return Math.Sign(c);
  }
  return p.Length.CompareTo(q.Length);
 }

 /// <summary>Offline checks run by "Nickel.exe --self-test": null when they pass, else what failed.</summary>
 public static string? SelfTest()
 {
  using (var pinned = ECDsa.Create()) { pinned.ImportFromPem(PublicKeyPem); if (pinned.KeySize != 256) return "the pinned key is not P-256"; }
  using var key = ECDsa.Create(ECCurve.NamedCurves.nistP256); using var other = ECDsa.Create(ECCurve.NamedCurves.nistP256);
  var pub = key.ExportSubjectPublicKeyInfoPem();
  byte[] Sig(ECDsa k, byte[] data) => Encoding.ASCII.GetBytes(Convert.ToBase64String(k.SignData(data, HashAlgorithmName.SHA256, DSASignatureFormat.Rfc3279DerSequence)));
  var path = Path.Combine(Path.GetTempPath(), $"nickel-trust-{Guid.NewGuid():N}.bin");
  try
  {
   var body = new byte[MinSize + 17]; RandomNumberGenerator.Fill(body); File.WriteAllBytes(path, body);
   var m = new ReleaseManifest("Nickel", "beta", "1.0.1-beta", "v1.0.1-beta", "Nickel.exe", body.Length, Convert.ToHexString(SHA256.HashData(body)).ToLowerInvariant());
   var bytes = Serialize(m);
   if (Verify(bytes, Sig(key, bytes), pub) is not { } ok || ok != m) return "an authentic manifest is refused";
   if (!Matches(path, m)) return "an authentic file is refused";
   var forged = (byte[])bytes.Clone(); forged[^3] ^= 1;
   if (Verify(forged, Sig(key, bytes), pub) != null) return "a modified manifest is accepted";
   if (Verify(bytes, Sig(other, bytes), pub) != null) return "a manifest signed by another key is accepted";
   if (Verify(bytes, Sig(key, Serialize(m with { Version = "9.0.0", Tag = "v9.0.0" })), pub) != null) return "a valid signature of other bytes is accepted";
   if (Verify(bytes, Encoding.ASCII.GetBytes("not a signature"), pub) != null) return "a garbage signature is accepted";
   foreach (var bad in new[] { m with { Product = "Other" }, m with { File = "evil.exe" }, m with { Tag = "v2.0.0" }, m with { Size = MinSize - 1 },
                               m with { Size = MaxSize + 1 }, m with { Sha256 = "zz" }, m with { Version = "1.0", Tag = "v1.0" }, m with { Version = "v1.0.1", Tag = "vv1.0.1" } })
   { var b = Serialize(bad); if (Verify(b, Sig(key, b), pub) != null) return $"a malformed manifest is accepted: {Encoding.UTF8.GetString(b)}"; }
   var large = new byte[MaxManifestBytes + 1]; if (Verify(large, Sig(key, large), pub) != null) return "an oversized manifest is accepted";
   body[body.Length / 2] ^= 1; File.WriteAllBytes(path, body);
   if (Matches(path, m)) return "a file modified by one byte is accepted";
   File.WriteAllBytes(path, body.AsSpan(0, body.Length - 1).ToArray());
   if (Matches(path, m)) return "a truncated file is accepted";
   if (AllowedSource(new Uri("http://nickelclient.com/download")) || AllowedSource(new Uri("https://nickelclient.com.evil.example/x")) ||
       AllowedSource(new Uri("https://user@github.com/x")) || !AllowedSource(new Uri("https://github.com/x")))
    return "the download host policy is wrong";
   string[] order = ["0.9.9", "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta", "1.0.0-beta.2", "1.0.0-beta.10", "1.0.0-beta.x", "1.0.0-rc.1", "1.0.0", "v1.0.1-beta", "1.0.1", "1.10.0", "99999999999.0.0"];
   for (int i = 0; i + 1 < order.Length; ++i) if (Compare(order[i], order[i + 1]) >= 0 || Compare(order[i + 1], order[i]) <= 0) return $"version order: {order[i]} / {order[i + 1]}";
   if (Compare("1.0.0-beta", "v1.0.0-beta") != 0 || Compare("1.0.0+build.5", "1.0.0") != 0) return "version order: prefix or build metadata";
   foreach (var invalid in new[] { "vv1.0.0", "01.0.0", "1.0.0-01", "1.0", "+1.0.0", " 1.0.0", "1.0.0-", "1.0.0-be ta" }) if (IsVersion(invalid)) return $"invalid version accepted: {invalid}";
   // the newest of all sources wins; nothing older or equal is offered (replay / downgrade)
   var older = m with { Version = "1.0.0-beta", Tag = "v1.0.0-beta" }; var newer = m with { Version = "1.0.2", Tag = "v1.0.2" };
   if (Newest([older, newer], "1.0.1-beta") != newer || Newest([older], "1.0.1-beta") != null || Newest([m], "1.0.1-beta") != null) return "the newest-release choice is wrong";
   return null;
  }
  finally { try { File.Delete(path); } catch { } }
 }
}
