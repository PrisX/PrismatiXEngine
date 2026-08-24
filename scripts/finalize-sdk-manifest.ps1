param(
    [Parameter(Mandatory = $true)]
    [string]$SdkRoot,
    [string]$TargetTriple
)

$ErrorActionPreference = "Stop"
$resolvedRoot = (Resolve-Path -LiteralPath $SdkRoot).Path
$manifestPath = Join-Path $resolvedRoot "PrismatiXSDKManifest.json"
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    throw "SDK manifest is missing: $manifestPath"
}

$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.format -ne "PrismatiXSDKManifest" -or $manifest.schemaRevision -ne 2) {
    throw "Only PrismatiXSDKManifest schema revision 2 can be finalized"
}
if (-not [string]::IsNullOrWhiteSpace($TargetTriple)) {
    $manifest.target = $TargetTriple
}
if ([string]::IsNullOrWhiteSpace($manifest.target)) {
    throw "SDK target triple is required"
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

$excluded = @("PrismatiXSDKManifest.json", "PrismatiXSDKManifest.sha256")
$artifacts = @(
    Get-ChildItem -LiteralPath $resolvedRoot -File -Recurse |
        Where-Object { $excluded -notcontains $_.Name } |
        ForEach-Object {
            $relative = [System.IO.Path]::GetRelativePath($resolvedRoot, $_.FullName).Replace("\", "/")
            [ordered]@{
                path = $relative
                size = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        } |
        Sort-Object { $_.path }
)
if ($artifacts.Count -eq 0) {
    throw "SDK package contains no payload artifacts"
}
$artifactIdentity = ($artifacts | ForEach-Object {
    "$($_.path)`0$($_.sha256)`0$($_.size)`n"
}) -join ""

$contractFiles = [ordered]@{
    characterResources = "include/PrismatiX/Engine/SDK/CharacterResources.h"
    gameCatalogResources = "include/PrismatiX/Engine/SDK/GameCatalogResources.h"
    packager = "include/PrismatiX/Engine/SDK/Packager.h"
    runtimeIr = "include/PrismatiX/Engine/SDK/RuntimeIr.h"
    studioUi = "include/PrismatiX/Engine/SDK/StudioUi.h"
    uiTypeRegistry = "include/PrismatiX/Engine/SDK/UiTypeRegistry.h"
}
$contractHashes = [ordered]@{}
foreach ($entry in $contractFiles.GetEnumerator()) {
    $path = Join-Path $resolvedRoot $entry.Value
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "SDK contract is missing: $($entry.Value)"
    }
    $contractHashes[$entry.Key] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
$previewContract = @(
    "PrismatiXPreviewProtocol",
    "schemaRevision=2",
    "protocolVersion=2",
    "envelope=protocolVersion,sessionId,requestId,documentId,revision",
    "commands=apply,patch,play,pause,continue,step,seek,selectChoice,input,capture,stop",
    "events=state,runtimeFocus,diagnostics,output,debug,audioState,unsupported,crashed"
) -join "`n"
$contractHashes.previewProtocol = Get-Sha256Text $previewContract

$manifest.artifactSha256 = Get-Sha256Text $artifactIdentity
$manifest.contractHashes = $contractHashes
$manifest | Add-Member -NotePropertyName artifacts -NotePropertyValue $artifacts -Force
$placeholder = "0" * 64
$manifest.manifestSha256 = $placeholder
$serialized = $manifest | ConvertTo-Json -Depth 12
[System.IO.File]::WriteAllText($manifestPath, $serialized + "`n", [System.Text.UTF8Encoding]::new($false))
$raw = [System.IO.File]::ReadAllText($manifestPath)
$manifestHash = Get-Sha256Text $raw
$raw = $raw.Replace($placeholder, $manifestHash)
[System.IO.File]::WriteAllText($manifestPath, $raw, [System.Text.UTF8Encoding]::new($false))
[System.IO.File]::WriteAllText(
    (Join-Path $resolvedRoot "PrismatiXSDKManifest.sha256"),
    "$manifestHash  PrismatiXSDKManifest.json`n",
    [System.Text.UTF8Encoding]::new($false)
)

Write-Output "Finalized SDK manifest for $($manifest.target)"
Write-Output "Artifact SHA-256: $($manifest.artifactSha256)"
Write-Output "Manifest SHA-256: $manifestHash"
