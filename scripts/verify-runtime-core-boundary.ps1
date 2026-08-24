$ErrorActionPreference = "Stop"
$engineRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\PrismatiXEngine\Engine"))
$coreRoots = @(
    "Animation", "Audio", "Core", "Diagnostics", "Graphics", "Preview",
    "Scene", "Session", "Support", "Text", "UI", "VN"
)
$forbidden = '(windows\.h|Win32|emscripten/|emscripten::|tauri|web_sys|wasm_bindgen)'
$violations = @()
foreach ($root in $coreRoots) {
    $path = Join-Path $engineRoot $root
    if (-not (Test-Path -LiteralPath $path)) { continue }
    $matches = Get-ChildItem -LiteralPath $path -Recurse -File -Include *.h,*.cpp |
        Select-String -Pattern $forbidden -CaseSensitive:$false
    $violations += $matches
}
$coreProgressionFiles = @(
    "CGGallery.cpp", "CGGallery.h", "GlobalProfile.cpp", "GlobalProfile.h",
    "SceneUnlock.cpp", "SceneUnlock.h"
)
$progressionRoot = Join-Path $engineRoot "Progression"
foreach ($name in $coreProgressionFiles) {
    $path = Join-Path $progressionRoot $name
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $violations += Select-String -LiteralPath $path -Pattern $forbidden -CaseSensitive:$false
    }
}
if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Error "$($_.Path):$($_.LineNumber): $($_.Line.Trim())" }
    throw "RuntimeCore contains a platform API reference"
}
Write-Output "RuntimeCore boundary scan passed"
