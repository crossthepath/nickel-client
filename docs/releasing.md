# Releasing

Launchers install an update only when its manifest is signed by the release key pinned in
[desktop/ReleaseTrust.cs](../desktop/ReleaseTrust.cs), names a newer version, and the downloaded file matches the
manifest's size and SHA-256. Downloads come over HTTPS from nickelclient.com or GitHub only.

## Steps

1. Set `<Version>` in `desktop/Nickel.Desktop.csproj` (the launcher reads its version from it), for example
   `1.0.1-beta`. Versions follow Semantic Versioning: `1.0.1-beta` < `1.0.1-beta.2` < `1.0.1`.
2. Build and run the checks:
   ```sh
   powershell -ExecutionPolicy Bypass -File tools/build_desktop.ps1 -OutputFolder dist/Nickel
   ctest --test-dir build -C Release --output-on-failure
   ```
3. Sign the build with the private release key (kept offline, never committed):
   ```sh
   dotnet run --project tools/release-signer -c Release -- sign dist/Nickel/Nickel.exe 1.0.1-beta <private.pem> dist/release
   dotnet run --project tools/release-signer -c Release -- verify dist/release dist/Nickel/Nickel.exe
   ```
4. Publish the GitHub release `v1.0.1-beta` with three assets: `Nickel.exe`, `release-manifest.json`,
   `release-manifest.sig`, uploaded from the exact files signed in step 3 (builds are not byte-for-byte reproducible:
   sign the artifact you publish, never a rebuild). Mark it as the latest release and not as a pre-release: launchers
   read `releases/latest/download/`, which skips pre-releases. Then download the three published assets and run
   `verify` on them.
5. Copy the same three files to the website's data volume (`/data`), then write `{"tag":"v1.0.1-beta"}` to
   `/data/release.json` for launchers older than 1.0.1, which still read `/api/release`.

## Channel

`channel` is `beta` (default of the signer) or `stable` (pass `stable` as the last argument). A pre-release launcher
(`1.0.1-beta`) accepts both; a release launcher (`1.1.0`) accepts `stable` only.

## What the launcher checks

Both sources are read; a manifest is used only if its signature verifies with the pinned key, its fields are valid,
its channel is accepted and its version is newer than the launcher's own. Every redirect hop must be HTTPS on
nickelclient.com, github.com, objects.githubusercontent.com or release-assets.githubusercontent.com. The download is
written next to the launcher, abandoned after 10 minutes or 60 seconds without data, and must match the manifest's
size and SHA-256 before it replaces the running EXE (the old one is kept as `Nickel.exe.old` until the next start
and put back if the swap fails). `Nickel.exe --self-test` and `release-signer self-test` run these checks offline.

## Key

- `keygen <private.pem>` creates a new key and prints its public half. Losing the private key means shipping a
  launcher with a new pinned key, which older launchers cannot verify: keep an offline backup.
- Never sign a file you did not build from a reviewed commit.
- The key file is not encrypted: keep it on an offline or encrypted medium, with a second copy.
