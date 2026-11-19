[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$DependencyFile,
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [ValidateSet('edit-reuse-policy','present-frame-policy','capability-policy')][string]$Suite = 'edit-reuse-policy'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
$policyName = $Suite.ToLowerInvariant().Replace('-', '_')
$testSource = [IO.Path]::GetFullPath((Join-Path $repo "tests/${policyName}_tests.cpp"))
$headerName = if ($Suite -eq 'capability-policy') { 'capability.hpp' } else { "$policyName.hpp" }
$policyHeader = [IO.Path]::GetFullPath((Join-Path $SourceRoot $headerName))
$stagePlanHeader = if ($Suite -eq 'capability-policy') { [IO.Path]::GetFullPath((Join-Path $SourceRoot 'stage_plan.hpp')) }
$record = Get-Content -LiteralPath $DependencyFile -Raw | ConvertFrom-Json -AsHashtable
if ($record -isnot [Collections.IDictionary] -or $record['Data'] -isnot [Collections.IDictionary]) {
    throw "Missing MSVC dependency data: $DependencyFile"
}
$data = $record['Data']
if ($data['Source'] -isnot [string] -or -not [IO.Path]::IsPathFullyQualified($data['Source']) -or
    [IO.Path]::GetFullPath($data['Source']) -ine $testSource) {
    throw "Dependency record must identify $testSource`: $DependencyFile"
}
$includes = $data['Includes']
if ($includes -isnot [array] -or $includes.Count -eq 0) {
    throw "Missing or empty MSVC include closure: $DependencyFile"
}
if ($data['ProvidedModule'] -or $data['PCH'] -or $data['ImportedModules'] -or $data['ImportedHeaderUnits']) {
    throw "Execution policy must use textual includes: $DependencyFile"
}

if (-not ($env:VCToolsInstallDir -and $env:WindowsSdkDir -and $env:WindowsSDKVersion)) {
    . (Join-Path $repo 'tests/test-support.ps1')
    Initialize-TestBuild
}
$standardPrefixes = @(
    (Join-Path $env:VCToolsInstallDir 'include'),
    (Join-Path $env:WindowsSdkDir "Include/$($env:WindowsSDKVersion.TrimEnd('\'))/ucrt")
) | ForEach-Object { [IO.Path]::GetFullPath($_).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar }

# MSVC Includes already contains both direct and transitive headers.
$sawPolicyHeader = $false
foreach ($include in $includes) {
    if ($include -isnot [string] -or -not [IO.Path]::IsPathFullyQualified($include)) {
        throw "Invalid MSVC include path in $DependencyFile"
    }
    $path = [IO.Path]::GetFullPath($include)
    if ($path -ieq $policyHeader) {
        $sawPolicyHeader = $true
        continue
    }
    if ($path -ieq $stagePlanHeader) { continue }
    if (-not ($standardPrefixes | Where-Object { $path.StartsWith($_, [StringComparison]::OrdinalIgnoreCase) })) {
        throw "Disallowed execution policy dependency: $path"
    }
}
if (-not $sawPolicyHeader) { throw "Missing production policy header in dependency record: $policyHeader" }
Write-Host "PASS $($policyName.Replace('_', ' ')) dependencies evidence=STATIC"
