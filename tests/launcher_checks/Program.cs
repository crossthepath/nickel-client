// Offline checks of the launcher's pure logic, compiled against the launcher's own sources (see launcher-checks.csproj).
// Each check prints "ok" or "FAIL" with its name; the exit code is 0 when every check passes, 1 otherwise.
//
//   dotnet run --project tests/launcher_checks -c Release

using System.Security.Cryptography;
using System.Text;
using Nickel.Desktop;

var checks = new List<(string Name, Func<bool> Run)>();
void Check(string name, Func<bool> run) => checks.Add((name, run));

// ---- the published 1.1.0-beta release: its manifest and the signature made by tools/release-signer
const string PublishedManifest = """{"product":"Nickel","channel":"beta","version":"1.1.0-beta","tag":"v1.1.0-beta","file":"Nickel.exe","size":9351768,"sha256":"08e6faab9fd710d4b73d81ab397adaa2eb9aaab2584a746bbd96cdb523029c70"}""";
const string PublishedSignature = "MEQCIGR8JCK40pAhP99qf95bKy1KXzO4FuJRNDZtANfPO7TXAiBmHIQohy9PccQoOw8zcP0EchKu58ppa+9rU9GffvjnlA==";
var published = new ReleaseManifest("Nickel", "beta", "1.1.0-beta", "v1.1.0-beta", "Nickel.exe", 9351768, "08e6faab9fd710d4b73d81ab397adaa2eb9aaab2584a746bbd96cdb523029c70");
byte[] Ascii(string text) => Encoding.ASCII.GetBytes(text);
byte[] Utf8(string text) => Encoding.UTF8.GetBytes(text);
ReleaseManifest At(string version) => published with { Version = version, Tag = "v" + version };

// ---- version order (Semantic Versioning 2.0)
Check("version: 1.1.0-beta is newer than 1.0.0-beta", () => ReleaseTrust.Compare("1.1.0-beta", "1.0.0-beta") > 0 && ReleaseTrust.Compare("1.0.0-beta", "1.1.0-beta") < 0);
Check("version: a release is newer than its pre-release (1.1.0 > 1.1.0-beta)", () => ReleaseTrust.Compare("1.1.0", "1.1.0-beta") > 0 && ReleaseTrust.Compare("1.1.0-beta", "1.1.0") < 0);
Check("version: numeric identifiers compare as numbers (beta.10 > beta.2, 1.10.0 > 1.9.0)", () =>
 ReleaseTrust.Compare("1.0.0-beta.10", "1.0.0-beta.2") > 0 && ReleaseTrust.Compare("1.10.0", "1.9.0") > 0 && ReleaseTrust.Compare("10.0.0", "9.99.99") > 0);
Check("version: numeric identifiers sort before alphanumeric ones (beta.2 < beta.x)", () => ReleaseTrust.Compare("1.0.0-beta.2", "1.0.0-beta.x") < 0);
Check("version: equal versions, leading v and build metadata ignored", () =>
 ReleaseTrust.Compare("1.1.0-beta", "1.1.0-beta") == 0 && ReleaseTrust.Compare("v1.1.0-beta", "1.1.0-beta") == 0 && ReleaseTrust.Compare("1.1.0+build.7", "1.1.0") == 0);
Check("version: well-formed versions are accepted", () =>
 new[] { "0.0.0", "1.1.0-beta", "1.0.0-beta.2", "1.0.0-rc.1+build.5", "v1.1.0", "V1.1.0", "10.20.30", "1.0.0-0" }.All(ReleaseTrust.IsVersion));
Check("version: leading zeros in numeric identifiers are refused", () =>
 !new[] { "01.0.0", "1.01.0", "1.0.01", "1.0.0-01", "1.0.0-beta.02" }.Any(ReleaseTrust.IsVersion));
Check("version: malformed versions are refused", () =>
 !new[] { "", "1", "1.0", "1.0.0.0", "vv1.0.0", " 1.0.0", "1.0.0 ", "1.0.0-", "1.0.0-be ta", "1.0.0+", "+1.0.0", "a.b.c", "1..0", "-1.0.0", "1.0.0-beta..1" }.Any(ReleaseTrust.IsVersion));
Check("version: an unreadable version sorts first", () =>
 ReleaseTrust.Compare("garbage", "0.0.0") < 0 && ReleaseTrust.Compare("0.0.0", "garbage") > 0 && ReleaseTrust.Compare("garbage", "01.0.0") == 0);

// ---- channels: stable always, beta only for a pre-release launcher
Check("channel: stable is accepted by every launcher", () => ReleaseTrust.ChannelAccepted("stable", "1.1.0-beta") && ReleaseTrust.ChannelAccepted("stable", "1.1.0"));
Check("channel: beta is accepted by a pre-release launcher", () => ReleaseTrust.ChannelAccepted("beta", "1.1.0-beta") && ReleaseTrust.ChannelAccepted("beta", "1.0.0-rc.1"));
Check("channel: beta is refused by a stable launcher", () => !ReleaseTrust.ChannelAccepted("beta", "1.1.0") && !ReleaseTrust.ChannelAccepted("beta", "2.0.0"));
Check("channel: unknown or differently cased channels are refused", () =>
 !new[] { "", "nightly", "Stable", "BETA", "stable " }.Any(c => ReleaseTrust.ChannelAccepted(c, "1.1.0-beta")));

// ---- download sources: HTTPS, the project's hosts, no user info
Check("source: the project's HTTPS hosts are allowed", () => new[] {
 "https://nickelclient.com/release/release-manifest.json", "https://nickelclient.com/download",
 "https://github.com/crossthepath/nickel-client/releases/latest/download/release-manifest.json",
 "https://objects.githubusercontent.com/x", "https://release-assets.githubusercontent.com/x", "https://GitHub.com/x" }.All(u => ReleaseTrust.AllowedSource(new Uri(u))));
Check("source: other schemes are refused", () => !new[] {
 "http://nickelclient.com/download", "http://github.com/x", "ftp://github.com/x", "file:///C:/Nickel.exe", "wss://github.com/x" }.Any(u => ReleaseTrust.AllowedSource(new Uri(u))));
Check("source: other hosts are refused (look-alikes, subdomains, other GitHub hosts)", () => !new[] {
 "https://nickelclient.com.evil.example/x", "https://evil.nickelclient.com/x", "https://nickelclient.co/x", "https://api.github.com/x",
 "https://raw.githubusercontent.com/x", "https://githubusercontent.com/x", "https://example.com/x" }.Any(u => ReleaseTrust.AllowedSource(new Uri(u))));
Check("source: user info in the address is refused", () =>
 !ReleaseTrust.AllowedSource(new Uri("https://user@github.com/x")) && !ReleaseTrust.AllowedSource(new Uri("https://user:secret@nickelclient.com/x")));
Check("source: no address is refused", () => !ReleaseTrust.AllowedSource(null));

// ---- newest release: never older or equal to the launcher (replay, downgrade)
Check("newest: an older release is not offered", () => ReleaseTrust.Newest([At("1.0.0-beta")], "1.1.0-beta") == null);
Check("newest: the same release is not offered again (replay)", () => ReleaseTrust.Newest([At("1.1.0-beta")], "1.1.0-beta") == null);
Check("newest: a pre-release of the running release is not offered (downgrade)", () => ReleaseTrust.Newest([At("1.1.0-beta")], "1.1.0") == null);
Check("newest: no source gives nothing", () => ReleaseTrust.Newest([], "1.1.0-beta") == null);
Check("newest: the newest of all sources wins, in any order", () =>
 ReleaseTrust.Newest([At("1.0.0-beta"), At("1.1.1-beta"), At("1.1.0-beta")], "1.1.0-beta")?.Version == "1.1.1-beta" &&
 ReleaseTrust.Newest([At("1.2.0"), At("1.1.1-beta")], "1.1.0-beta")?.Version == "1.2.0" &&
 ReleaseTrust.Newest([At("1.1.1-beta"), At("1.2.0")], "1.1.0-beta")?.Version == "1.2.0");
Check("newest: a stale source cannot hide a newer one", () => ReleaseTrust.Newest([At("1.0.0-beta"), At("1.1.0-beta"), At("1.1.1-beta")], "1.1.0-beta")?.Version == "1.1.1-beta");

// ---- the published manifest: canonical bytes and signature
Check("serialize: the 1.1.0-beta manifest gives the published bytes exactly", () => ReleaseTrust.Serialize(published).AsSpan().SequenceEqual(Utf8(PublishedManifest)));
Check("verify: the published manifest and signature are accepted", () => ReleaseTrust.Verify(Utf8(PublishedManifest), Ascii(PublishedSignature)) == published);
Check("verify: the published signature with a trailing newline is accepted", () => ReleaseTrust.Verify(Utf8(PublishedManifest), Ascii(PublishedSignature + "\r\n")) == published);
Check("verify: the published signature over a 1.1.1-beta manifest is refused", () =>
{
 var replayed = PublishedManifest.Replace("1.1.0-beta", "1.1.1-beta");   // version and tag, so only the signature can refuse it
 return replayed != PublishedManifest && ReleaseTrust.Problem(At("1.1.1-beta")) == null && ReleaseTrust.Verify(Utf8(replayed), Ascii(PublishedSignature)) == null;
});
Check("verify: the published manifest with trailing data is refused", () => ReleaseTrust.Verify(Utf8(PublishedManifest + " "), Ascii(PublishedSignature)) == null);
Check("verify: a garbage or empty signature is refused", () =>
 ReleaseTrust.Verify(Utf8(PublishedManifest), Ascii("not a signature")) == null && ReleaseTrust.Verify(Utf8(PublishedManifest), []) == null);
Check("verify: the published signature checked with another key is refused", () =>
{
 using var other = ECDsa.Create(ECCurve.NamedCurves.nistP256);
 return ReleaseTrust.Verify(Utf8(PublishedManifest), Ascii(PublishedSignature), other.ExportSubjectPublicKeyInfoPem()) == null;
});

// ---- manifest fields
Check("problem: the published manifest is well formed", () => ReleaseTrust.Problem(published) == null);
Check("problem: an empty sha256 is refused", () => ReleaseTrust.Problem(published with { Sha256 = "" }) is { } p && p.Contains("sha256"));
Check("problem: a sha256 that is not 64 hex digits is refused", () =>
 ReleaseTrust.Problem(published with { Sha256 = published.Sha256[..63] }) != null && ReleaseTrust.Problem(published with { Sha256 = published.Sha256[..63] + "g" }) != null);
Check("problem: an upper-case sha256 is well formed", () => ReleaseTrust.Problem(published with { Sha256 = published.Sha256.ToUpperInvariant() }) == null);
Check("problem: size 0 is refused", () => ReleaseTrust.Problem(published with { Size = 0 }) is { } p && p.Contains("size"));
Check("problem: sizes outside the limits are refused", () =>
 ReleaseTrust.Problem(published with { Size = ReleaseTrust.MinSize - 1 }) != null && ReleaseTrust.Problem(published with { Size = ReleaseTrust.MaxSize + 1 }) != null &&
 ReleaseTrust.Problem(published with { Size = ReleaseTrust.MinSize }) == null && ReleaseTrust.Problem(published with { Size = ReleaseTrust.MaxSize }) == null);
Check("problem: a wrong product is refused", () => ReleaseTrust.Problem(published with { Product = "Other" }) is { } p && p.Contains("product"));
Check("problem: a wrong file name is refused", () => ReleaseTrust.Problem(published with { File = "Other.exe" }) != null);
Check("problem: an unknown channel is refused", () => ReleaseTrust.Problem(published with { Channel = "nightly" }) is { } p && p.Contains("channel"));
Check("problem: a tag that is not v + version is refused", () =>
 ReleaseTrust.Problem(published with { Tag = "1.1.0-beta" }) != null && ReleaseTrust.Problem(published with { Tag = "v1.1.1-beta" }) != null);
Check("problem: a version with a leading v is refused", () => ReleaseTrust.Problem(published with { Version = "v1.1.0-beta", Tag = "vv1.1.0-beta" }) != null);

// ---- download hash comparison
Check("hash: a matching SHA-256 is accepted whatever the case of the manifest", () =>
{
 var body = Utf8("Nickel"); var hex = Convert.ToHexString(SHA256.HashData(body));
 return ReleaseTrust.HashMatches(SHA256.HashData(body), published with { Sha256 = hex.ToLowerInvariant() }) &&
        ReleaseTrust.HashMatches(SHA256.HashData(body), published with { Sha256 = hex.ToUpperInvariant() }) &&
        ReleaseTrust.HashMatches(SHA256.HashData(body), published with { Sha256 = hex[..32].ToLowerInvariant() + hex[32..] });
});
Check("hash: a different SHA-256 is refused", () => !ReleaseTrust.HashMatches(SHA256.HashData(Utf8("Nickel!")), published with { Sha256 = Convert.ToHexString(SHA256.HashData(Utf8("Nickel"))) }));
Check("hash: a truncated hash is refused", () => !ReleaseTrust.HashMatches(SHA256.HashData(Utf8("Nickel"))[..31], published with { Sha256 = Convert.ToHexString(SHA256.HashData(Utf8("Nickel"))) }));

// ---- the launcher's own self-tests (also run by "Nickel.exe --self-test")
Check("ReleaseTrust.SelfTest passes", () => Report(ReleaseTrust.SelfTest()));
Check("GameBuilds.SelfTest passes", () => Report(GameBuilds.SelfTest()));

// ---- validated game builds
Check("game builds: SupportedVersions is \"1.26.62 / 1.26.63 / 1.26.65\"", () => GameBuilds.SupportedVersions == "1.26.62 / 1.26.63 / 1.26.65");
Check("game builds: versions 1.26.60 to 1.26.69 are adapted in game, others refused", () =>
 GameBuilds.Adaptable(1, 26, 60) && GameBuilds.Adaptable(1, 26, 64) && GameBuilds.Adaptable(1, 26, 69) &&
 !GameBuilds.Adaptable(1, 26, 59) && !GameBuilds.Adaptable(1, 26, 70) && !GameBuilds.Adaptable(1, 27, 62));
Check("game builds: each validated version is recognised, others are not", () =>
 GameBuilds.VersionValidated(1, 26, 62, GameBuilds.Validated) && GameBuilds.VersionValidated(1, 26, 63, GameBuilds.Validated) &&
 !GameBuilds.VersionValidated(1, 26, 64, GameBuilds.Validated) && !GameBuilds.VersionValidated(1, 25, 62, GameBuilds.Validated));
Check("game builds: every profile has the five checked sections", () =>
 GameBuilds.Validated.All(b => b.Sections.Select(s => s.Name).SequenceEqual([".text", ".rdata", ".data", ".pdata", ".reloc"]) && b.Sections.All(s => s.Sha256.Length == 64)));

int failed = 0;
foreach (var (name, run) in checks)
{
 bool pass;
 try { pass = run(); }
 catch (Exception error) { pass = false; Console.WriteLine($"        {error.GetType().Name}: {error.Message}"); }
 if (!pass) ++failed;
 Console.WriteLine($"{(pass ? "ok  " : "FAIL")}  {name}");
}
Console.WriteLine($"{checks.Count - failed}/{checks.Count} launcher checks passed");
return failed == 0 ? 0 : 1;

// A self-test returns null when it passes, else what failed (printed).
static bool Report(string? problem)
{
 if (problem != null) Console.WriteLine($"        {problem}");
 return problem == null;
}
