param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$BuildDirectory = "build/release",
    [string]$OutputDirectory = "dist"
)

$ErrorActionPreference = "Stop"
$Version = $Version.TrimStart("v")
$Root = Split-Path -Parent $PSScriptRoot
if ($Version -ne (python (Join-Path $PSScriptRoot "version.py"))) { throw "Version differs from CMake" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$OutputDirectory = (Resolve-Path $OutputDirectory).Path

cmake --preset release -B $BuildDirectory "-DTODOBENCH_BUNDLE_RUNTIME=ON" "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $BuildDirectory --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Stage = Join-Path $BuildDirectory "windows-stage"
if (Test-Path $Stage) { Remove-Item -Recurse -Force $Stage }
cmake --install $BuildDirectory --prefix $Stage
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$Binary = Get-ChildItem -Path $Stage -Recurse -Filter TodoBench.exe | Select-Object -First 1
if (-not $Binary) { throw "TodoBench.exe was not installed" }

$NativeDlls = Join-Path $BuildDirectory "vcpkg_installed/x64-windows/bin/*.dll"
Copy-Item $NativeDlls $Binary.DirectoryName
$Windeploy = Get-Command windeployqt -ErrorAction SilentlyContinue
if (-not $Windeploy) { throw "windeployqt is required to bundle Qt on Windows" }
& $Windeploy.Source --release --no-translations $Binary.FullName
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Copy-Item -Recurse (Join-Path $Stage "share/doc/TodoBench") (Join-Path $Binary.DirectoryName "doc")
Copy-Item (Join-Path $Root "LICENSE") $Binary.DirectoryName
Copy-Item (Join-Path $Root "README.md") $Binary.DirectoryName
Copy-Item (Join-Path $Root "packaging/THIRD_PARTY_NOTICES.md") $Binary.DirectoryName

$Arch = if ($env:PROCESSOR_ARCHITECTURE -eq "ARM64") { "arm64" } else { "x64" }
$ZipName = "TodoBench-$Version-windows-$Arch.zip"
$ZipPath = Join-Path $OutputDirectory $ZipName
if (Test-Path $ZipPath) { Remove-Item $ZipPath }
Compress-Archive -Path (Join-Path $Binary.DirectoryName "*") -DestinationPath $ZipPath
Get-FileHash -Algorithm SHA256 $ZipPath |
    ForEach-Object { "$($_.Hash.ToLower())  $ZipName" } |
    Set-Content -Path (Join-Path $OutputDirectory "TodoBench-$Version-windows-$Arch.sha256")
Write-Host "Created $ZipPath"

$Makensis = Get-Command makensis -ErrorAction SilentlyContinue
if ($Makensis) {
    $Nsi = Join-Path $Root "packaging\windows\todobench.nsi"
    $Installer = Join-Path $OutputDirectory "TodoBench-$Version-windows-$Arch-setup.exe"
    & $Makensis.Source "/DVERSION=$Version" "/DARCH=$Arch" "/DSTAGE=$($Binary.DirectoryName)" "/DOUTPUT_FILE=$Installer" $Nsi
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Get-FileHash -Algorithm SHA256 $Installer |
        ForEach-Object { "$($_.Hash.ToLower())  $(Split-Path $Installer -Leaf)" } |
        Set-Content -Path (Join-Path $OutputDirectory "TodoBench-$Version-windows-$Arch-setup.sha256")
    Write-Host "Created $Installer"
} else {
    throw "makensis is required for the release installer"
}
