param(
    [string]$PackageRoot = "",
    [switch]$SkipPackage
)

$ErrorActionPreference = "Stop"
$engineRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$emcmake = Get-Command emcmake -ErrorAction SilentlyContinue
if (-not $emcmake) {
    throw "Emscripten is not active. Run emsdk_env.ps1, then rerun this script."
}

Push-Location $engineRoot
try {
    & $emcmake.Source cmake --preset wasm-preview
    if ($LASTEXITCODE -ne 0) { throw "Emscripten configure failed" }
    cmake --build --preset wasm-preview
    if ($LASTEXITCODE -ne 0) { throw "WASM Preview build failed" }
} finally {
    Pop-Location
}

if ($SkipPackage) { return }
if ([string]::IsNullOrWhiteSpace($PackageRoot)) {
    $PackageRoot = Join-Path $engineRoot "out\package\wasm-preview"
}
$destination = [System.IO.Path]::GetFullPath($PackageRoot)
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$source = Join-Path $engineRoot "out\build\wasm-preview\PrismatiXEngine"
$artifacts = @(
    "prismatix-preview.mjs",
    "prismatix-preview.wasm",
    "prismatix-preview.wasm.map",
    "preview-contract.json"
)
foreach ($artifact in $artifacts) {
    $path = Join-Path $source $artifact
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "WASM Preview artifact is missing: $path"
    }
    Copy-Item -LiteralPath $path -Destination (Join-Path $destination $artifact) -Force
}
& (Join-Path $PSScriptRoot "finalize-wasm-preview-manifest.ps1") `
    -PreviewRoot $destination
if ($LASTEXITCODE -ne 0) { throw "WASM Preview manifest finalization failed" }
Write-Output "Packaged WASM Preview in $destination"
