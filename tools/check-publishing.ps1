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
    foreach ($option in @('NR_NEXUS_DEV_CONTROL','NR_NEXUS_IME_DIAGNOSTICS')) {
        $setting = [regex]::Matches($cache, "(?m)^${option}:[^\r\n]+")
        if ($setting.Count -ne 1 -or $setting[0].Value.TrimEnd("`r") -cne "${option}:BOOL=OFF") {
            throw "Publishing requires ${option}:BOOL=OFF."
        }
    }
    $commandPath = Join-Path $BuildDirectory 'dlss5.dir/Release/dlss5.tlog/CL.command.1.tlog'
    $command = Get-Content -LiteralPath $commandPath -Raw -Encoding Unicode
    if ($command -notmatch '(?im)^\^.*[\\/]addon\.cpp\s*$' -or $command -notmatch '(?i)(?:^|\s)/c(?:\s|$)') {
        throw 'Publishing requires the actual Release addon.cpp compiler command.'
    }
    # These switches enable dedicated diagnostics or test behavior. Even =0
    # defines a macro for the source's defined()/ifdef guards.
    foreach ($define in @('NR_NEXUS_DEV_CONTROL','NR_NEXUS_IME_DIAGNOSTICS','RENODX_DLSS5_DEBUG',
        'NR_NEXUS_TEST_FG_OUTPUT_PROBE','NR_NEXUS_TEST_XEFG_PRESENT_PROBE','NR_NEXUS_TEST_NATIVE_NGX','NR_NEXUS_TEST_NATIVE_SDK_CLOCK','NR_NEXUS_TEST_COLD_CPU_TRACE',
        'NR_NEXUS_TEST_SUBMISSION_FAILURE','NR_NEXUS_DEV_CONTROL_PIPE_TEST_EXE','NR_NEXUS_TEST_WARP_ONLY')) {
        if ($command -match "(?i)(?:^|\s)`"?[/-]D\s*`"?$define\b") {
            throw "Publishing compiler command contains $define."
        }
    }
    $bytes = [IO.File]::ReadAllBytes($BinaryPath)
    $text = [Text.Encoding]::ASCII.GetString($bytes)
    foreach ($marker in @('inline_restore_roi','inline_restore_roi_v1','mafia-same-frame-roi',
        'unsupported_inline_restore_contract','bridge_adaptive_experiment','edit_reuse_adaptive_experiment',
        'edit_reuse_fg_experiment','adaptive_feature_experiment','fg_output_probe','xefg_present_probe','sdk_display_configuration_and_marker_recording_not_output_pixels',
        'linear_FP16_after_resolve_alpha_preserved','xefgSwapChainEnableDebugFeature','NR Nexus IME diagnostics')) {
        # Match UTF-16LE bytes at either alignment as well as plain ASCII.
        $wide = [Text.Encoding]::ASCII.GetString([Text.Encoding]::Unicode.GetBytes($marker))
        if ($text.Contains($marker) -or $text.Contains($wide)) { throw "Publishing binary contains development-only marker: $marker" }
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
