[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../build'),
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../dist'),
    [string]$ExpectedTag
)

function Assert-PublishingIsolation {
    param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$BinaryPath)
    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw -Encoding utf8
    $control = [regex]::Matches($cache, '(?m)^NR_NEXUS_DEV_CONTROL:[^\r\n]+')
    if ($control.Count -ne 1 -or $control[0].Value.TrimEnd("`r") -cne 'NR_NEXUS_DEV_CONTROL:BOOL=OFF') {
        throw 'Publishing requires NR_NEXUS_DEV_CONTROL:BOOL=OFF.'
    }
    $commandPath = Join-Path $BuildDirectory 'dlss5.dir/Release/dlss5.tlog/CL.command.1.tlog'
    $command = Get-Content -LiteralPath $commandPath -Raw -Encoding Unicode
    if ($command -notmatch '(?im)^\^.*[\\/]addon\.cpp\s*$' -or $command -notmatch '(?i)(?:^|\s)/c(?:\s|$)') {
        throw 'Publishing requires the actual Release addon.cpp compiler command.'
    }
    if ($command -match '(?i)(?:^|\s)[/-]D\s*"?NR_NEXUS_DEV_CONTROL\b') {
        throw 'Publishing compiler command contains NR_NEXUS_DEV_CONTROL.'
    }
    $bytes = [IO.File]::ReadAllBytes($BinaryPath)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    foreach ($marker in @('inline_restore_roi','inline_restore_roi_v1','mafia-same-frame-roi','unsupported_inline_restore_contract')) {
        # Match UTF-16LE bytes at either alignment as well as plain ASCII.
        $wide = [Text.Encoding]::ASCII.GetString([Text.Encoding]::Unicode.GetBytes($marker))
        if ($text.Contains($marker) -or $text.Contains($wide)) { throw "Publishing binary contains development ROI marker: $marker" }
    }
    [pscustomobject]@{ DllSha256=(Get-FileHash -LiteralPath $BinaryPath).Hash.ToLowerInvariant()
        CompilerCommandSha256=(Get-FileHash -LiteralPath $commandPath).Hash.ToLowerInvariant() }
}

if ($MyInvocation.InvocationName -eq '.') { return }
$ErrorActionPreference = 'Stop'
$binary = Join-Path $BuildDirectory 'Release/renodx-dlss5.addon64'
$isolation = Assert-PublishingIsolation -BuildDirectory $BuildDirectory -BinaryPath $binary
# package.ps1 checks PE version/brand, pinned license inputs, the exact ZIP
# allowlist and every embedded file hash, then emits the archive checksum.
$package = & (Join-Path $PSScriptRoot 'package.ps1') -BinaryPath $binary -HostRoot $HostRoot -OutputDirectory $OutputDirectory -ExpectedTag $ExpectedTag
$hash = (Get-FileHash -LiteralPath $package.ArchivePath).Hash.ToLowerInvariant()
$expected = "$hash  $([IO.Path]::GetFileName($package.ArchivePath))`n"
if ([IO.File]::ReadAllText($package.ChecksumPath) -cne $expected) { throw 'Publishing archive checksum mismatch.' }
Write-Host "Publishing isolation verified; DLL SHA256 $($isolation.DllSha256); ZIP SHA256 $hash"
$package
