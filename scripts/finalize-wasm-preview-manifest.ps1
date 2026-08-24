param(
    [Parameter(Mandatory = $true)]
    [string]$PreviewRoot
)

$ErrorActionPreference = "Stop"
$resolvedRoot = (Resolve-Path -LiteralPath $PreviewRoot).Path
$contractPath = Join-Path $resolvedRoot "preview-contract.json"
if (-not (Test-Path -LiteralPath $contractPath -PathType Leaf)) {
    throw "WASM Preview contract is missing: $contractPath"
}

$contract = Get-Content -LiteralPath $contractPath -Raw | ConvertFrom-Json
if ($contract.format -ne "PrismatiXPreviewContract" -or
    $contract.schemaRevision -ne 2 -or
    $contract.protocolVersion -ne 2) {
    throw "WASM Preview contract revision 2 / protocol 2 is required"
}
$commitPattern = '^[0-9a-f]{40}$'
if ($contract.engineCommit -notmatch $commitPattern -or
    $contract.runtimeCoreCommit -notmatch $commitPattern -or
    $contract.engineCommit -ne $contract.runtimeCoreCommit) {
    throw "WASM Preview and RuntimeCore must identify the same Engine commit"
}
if ($contract.emscriptenVersion -notmatch '^\d+\.\d+\.\d+$') {
    throw "WASM Preview contract has no valid Emscripten version"
}

function Get-Sha256Text([string]$Text) {
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        return ([System.BitConverter]::ToString($sha.ComputeHash($bytes))).Replace("-", "").ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

$required = @(
    "prismatix-preview.mjs",
    "prismatix-preview.wasm",
    "prismatix-preview.wasm.map",
    "preview-contract.json"
)
$artifacts = @()
foreach ($name in $required) {
    $path = Join-Path $resolvedRoot $name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "WASM Preview artifact is missing: $path"
    }
    $item = Get-Item -LiteralPath $path
    $artifacts += [ordered]@{
        path = $name
        size = $item.Length
        sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$artifacts = @($artifacts | Sort-Object { $_.path })
$artifactIdentity = ($artifacts | ForEach-Object {
    "$($_.path)`0$($_.sha256)`0$($_.size)`n"
}) -join ""
$byPath = @{}
foreach ($artifact in $artifacts) { $byPath[$artifact.path] = $artifact }

$manifest = [ordered]@{
    format = "PrismatiXWasmPreviewManifest"
    schemaRevision = 1
    engineCommit = $contract.engineCommit
    runtimeCoreCommit = $contract.runtimeCoreCommit
    emscriptenVersion = $contract.emscriptenVersion
    protocolVersion = 2
    moduleFactory = $contract.moduleFactory
    bundleSha256 = Get-Sha256Text $artifactIdentity
    moduleSha256 = $byPath["prismatix-preview.mjs"].sha256
    wasmSha256 = $byPath["prismatix-preview.wasm"].sha256
    sourceMapSha256 = $byPath["prismatix-preview.wasm.map"].sha256
    contractSha256 = $byPath["preview-contract.json"].sha256
    manifestSha256 = "0" * 64
    artifacts = $artifacts
}
$manifestPath = Join-Path $resolvedRoot "PrismatiXWasmPreviewManifest.json"
$serialized = $manifest | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText(
    $manifestPath,
    $serialized + "`n",
    [System.Text.UTF8Encoding]::new($false)
)
$raw = [System.IO.File]::ReadAllText($manifestPath)
$manifestHash = Get-Sha256Text $raw
$raw = $raw.Replace(("0" * 64), $manifestHash)
[System.IO.File]::WriteAllText(
    $manifestPath,
    $raw,
    [System.Text.UTF8Encoding]::new($false)
)
[System.IO.File]::WriteAllText(
    (Join-Path $resolvedRoot "PrismatiXWasmPreviewManifest.sha256"),
    "$manifestHash  PrismatiXWasmPreviewManifest.json`n",
    [System.Text.UTF8Encoding]::new($false)
)

Write-Output "Finalized WASM Preview manifest for Engine $($contract.engineCommit)"
Write-Output "Bundle SHA-256: $($manifest.bundleSha256)"
Write-Output "Manifest SHA-256: $manifestHash"
