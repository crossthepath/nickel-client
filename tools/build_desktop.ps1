param([string]$OutputFolder = 'dist/Nickel')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $ProjectRoot
function Check-BuildExit { if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE" } }
# native/generated_catalog.hpp is the module catalog; ui/catalog.js must match it (python tools/sync_catalog.py).
python tools/sync_catalog.py --check
Check-BuildExit
cmake -S . -B build
Check-BuildExit
cmake --build build --config Release --target nickel_overlay nickel_overlay_probe
Check-BuildExit
dotnet publish desktop/Nickel.Desktop.csproj -c Release -o $OutputFolder --ignore-failed-sources
Check-BuildExit
$ApplicationPath = Join-Path $ProjectRoot "$OutputFolder/Nickel.exe"
$CheckProcess = Start-Process -FilePath $ApplicationPath -ArgumentList '--self-test' -WindowStyle Hidden -Wait -PassThru
if ($CheckProcess.ExitCode -ne 0) { throw 'The bundled EXE self-check failed.' }
Copy-Item -LiteralPath 'THIRD_PARTY_NOTICES.md' -Destination (Join-Path $ProjectRoot "$OutputFolder/THIRD_PARTY_NOTICES.txt")
$Report = @{ status='passed'; executable=$ApplicationPath; sha256=(Get-FileHash -LiteralPath $ApplicationPath -Algorithm SHA256).Hash.ToLowerInvariant(); native_sha256=(Get-FileHash -LiteralPath 'build/Release/Nickel.Overlay.dll' -Algorithm SHA256).Hash.ToLowerInvariant(); bundled_native_component=$true; minecraft_load_verified=$false }
New-Item -ItemType Directory -Force -Path 'reports' | Out-Null
$Report | ConvertTo-Json | Set-Content -LiteralPath 'reports/desktop-build.json' -Encoding utf8
Write-Output "Built: $ApplicationPath"
