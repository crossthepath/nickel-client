# Releasing

Launchers install an update only when its manifest is signed by the release key pinned in
[desktop/ReleaseTrust.cs](../desktop/ReleaseTrust.cs), names a newer version, and the downloaded file matches the
manifest's size and SHA-256. Downloads come over HTTPS from nickelclient.com or GitHub only.

## Steps

1. Set the version in `desktop/Updater.cs` (`Version`) and `desktop/Nickel.Desktop.csproj` (`<Version>`), for example
   `1.0.1-beta`. Versions follow Semantic Versioning: `1.0.1-beta` < `1.0.1-beta.2` < `1.0.1`.
2. Build and run the checks:
   ```sh
   powershell -ExecutionPolicy Bypass -File tools/build_desktop.ps1 -OutputFolder dist/Nickel
   ctest --test-dir build -C Release --output-on-failure
   ```
3. Sign the build with the private release key (kept offline, never committed):
   ```sh
   dotnet run --project tools/release-signer -c Release -- sign dist/Nickel/Nickel.exe 1.0.1-beta <private.pem> dist/release
   dotnet run --project tools/release-signer -c Release -- verify dist/release
   ```
4. Publish the GitHub release `v1.0.1-beta` with three assets: `Nickel.exe`, `release-manifest.json`,
   `release-manifest.sig`. Mark it as the latest release.
5. Copy the same three files to the website's data volume (`/data`), then write `{"tag":"v1.0.1-beta"}` to
   `/data/release.json` for launchers older than 1.0.1, which still read `/api/release`.

## Key

- `keygen <private.pem>` creates a new key and prints its public half. Losing the private key means shipping a
  launcher with a new pinned key, which older launchers cannot verify: keep an offline backup.
- Never sign a file you did not build from a reviewed commit.
