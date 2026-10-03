param([string]$PackageRoot = "", [switch]$SkipBuild,
    [string]$NativeBuildRoot = "", [string]$Naga = "", [string]$SpirvCross = "", [string]$Shadercross = "")
$ErrorActionPreference = "Stop"
$engineRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $SkipBuild) {
    $emcmake = Get-Command emcmake -ErrorAction SilentlyContinue
    if (-not $emcmake) { throw "Emscripten is not active. Run emsdk_env.ps1 first." }
    Push-Location $engineRoot
    try {
        & $emcmake.Source cmake --preset wasm-player
        if ($LASTEXITCODE -ne 0) { throw "Web Player configure failed" }
        cmake --build --preset wasm-player
        if ($LASTEXITCODE -ne 0) { throw "Web Player build failed" }
    } finally { Pop-Location }
}
if ([string]::IsNullOrWhiteSpace($PackageRoot)) { $PackageRoot = Join-Path $engineRoot "out/package/wasm-player" }
$destination = [System.IO.Path]::GetFullPath($PackageRoot)
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$source = Join-Path $engineRoot "out/build/wasm-player/PrismatiXEngine"
$hashes = [ordered]@{}
foreach ($name in @("prismatix-player.mjs", "prismatix-player.wasm")) {
    $file = Join-Path $source $name
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing Web Player artifact: $file" }
    Copy-Item -LiteralPath $file -Destination (Join-Path $destination $name) -Force
    $hashes[$name] = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
}
if ([string]::IsNullOrWhiteSpace($NativeBuildRoot)) { $NativeBuildRoot = Join-Path $engineRoot "out/build/x64-release" }
if ([string]::IsNullOrWhiteSpace($Naga)) { $Naga = Join-Path $engineRoot "out/tools/naga/bin/naga.exe" }
if ([string]::IsNullOrWhiteSpace($SpirvCross)) { $SpirvCross = Join-Path $NativeBuildRoot "vcpkg_installed/x64-windows/tools/spirv-cross/spirv-cross.exe" }
if ([string]::IsNullOrWhiteSpace($Shadercross)) { $Shadercross = Join-Path $NativeBuildRoot "vcpkg_installed/x64-windows/tools/sdl3-shadercross/shadercross.exe" }
foreach ($tool in @($Naga, $SpirvCross, $Shadercross)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Missing offline shader converter: $tool. See docs/web-export.md." }
}
$nagaVersion = & $Naga --version
if ($nagaVersion -notmatch '^27\.0\.0$') { throw "Web shader conversion requires naga-cli 27.0.0; found $nagaVersion" }
$shaderDirectory = Join-Path $engineRoot "out/package/web-shaders"
New-Item -ItemType Directory -Path $shaderDirectory -Force | Out-Null
$shaders = [ordered]@{}
foreach ($stem in @("compositor", "rule")) {
    $spirv = Join-Path $shaderDirectory "$stem.spv"
    & $Shadercross (Join-Path $engineRoot "PrismatiXEngine/Shaders/Builtin/$stem.frag.hlsl") --source HLSL --dest SPIRV --stage fragment --entrypoint main --output $spirv
    if ($LASTEXITCODE -ne 0) { throw "Built-in $stem shader compilation failed" }
    node (Join-Path $engineRoot "scripts/convert-web-shader.mjs") $spirv (Join-Path $shaderDirectory $stem) $SpirvCross $Naga
    if ($LASTEXITCODE -ne 0) { throw "Built-in $stem Web shader conversion failed" }
    $shaders[$stem] = Get-Content -LiteralPath (Join-Path $shaderDirectory "$stem.json") -Raw | ConvertFrom-Json
}
$shaderOutput = Join-Path $destination "web-shaders.json"
$shaders | ConvertTo-Json -Depth 30 -Compress | Set-Content -LiteralPath $shaderOutput -Encoding utf8NoBOM
$hashes['web-shaders.json'] = (Get-FileHash -LiteralPath $shaderOutput -Algorithm SHA256).Hash.ToLowerInvariant()
New-Item -ItemType Directory -Path (Join-Path $destination "tools") -Force | Out-Null
Copy-Item -LiteralPath $Naga -Destination (Join-Path $destination "tools/naga.exe") -Force
Copy-Item -LiteralPath $SpirvCross -Destination (Join-Path $destination "tools/spirv-cross.exe") -Force
$cargoRegistry = Join-Path $env:USERPROFILE ".cargo/registry/src"
$noticeFiles = node (Join-Path $engineRoot "scripts/collect-web-notices.mjs") (Join-Path $engineRoot "out/build/wasm-player") $destination $cargoRegistry | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw "Web runtime notice collection failed" }
$spirvNotice = Join-Path $NativeBuildRoot "vcpkg_installed/x64-windows/share/spirv-cross/copyright"
New-Item -ItemType Directory -Path (Join-Path $destination "ToolNotices") -Force | Out-Null
Copy-Item -LiteralPath $spirvNotice -Destination (Join-Path $destination "ToolNotices/spirv-cross.txt") -Force
foreach ($name in $noticeFiles) {
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
node (Join-Path $engineRoot "scripts/archive-web-runtime-source.mjs") $destination
if ($LASTEXITCODE -ne 0) { throw "Web runtime source archive creation failed" }
$sources = @("RuntimeSource.tar.gz", "FriBidiSource.tar.gz")
foreach ($name in $sources) {
    $hashes[$name] = (Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
}
$version = (Get-Content -LiteralPath (Join-Path $engineRoot "packages/authoring-sdk/package.json") -Raw | ConvertFrom-Json).version
[ordered]@{format="PrismatiXWebPlayer";schemaRevision=1;variant="shipped";engineVersion=$version;artifacts=$hashes;notices=$noticeFiles;sources=$sources} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $destination "web-player.json") -Encoding utf8NoBOM
Write-Output "Packaged shipping Web Player: $destination"
