[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'tmp/ci-tests/core'))

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'tests/test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$previousCl = $env:CL
$outcome = 'Failed'
$policySuites = @('edit-reuse-policy','present-frame-policy')
$suites = @('native','sdr-policy','hdr-policy','scope','config','constants','stage-plan') +
    $policySuites + @('adaptive-resolution','copy-slots','lifecycle','fg-native')
$independentCases = [ordered]@{
    'native-prefg-resolution'='^PASS native-prefg-resolution hardware=0 steady_pairs_observed='
    'present-chain'='^PASS B1 WARP format=DXGI_FORMAT_R8G8B8A8_UNORM source_input='
    'nr-edit-reuse-contracts-public'='^PASS case=nr-edit-reuse-contracts-public evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-render-contracts'='^PASS case=nr-edit-reuse-render-contracts evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-present-contracts'='^PASS case=nr-edit-reuse-present-contracts evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-xefg-contracts'='^PASS case=nr-edit-reuse-xefg-contracts evidence=WARP\+MOCK dev=0$'
}
$devCases = @('nr-edit-reuse-native-contracts','native-present-decision')
$summary = [ordered]@{ schema='nr-nexus-ci-core/1'; status='Running'
    sourceCommit=(& git --no-optional-locks -C $repo rev-parse HEAD)
    sourceTree=(& git --no-optional-locks -C $repo rev-parse 'HEAD^{tree}')
    evidence='STATIC / CPU / WARP+MOCK'; suites=$suites; independent=@() }

function Get-CoreCaseReceipt([string]$Case, [string]$Stdout, [string]$Pattern) {
    # A mistyped flag can return zero after running the default FG group.
    $receipts = @(Get-Content -LiteralPath $Stdout | Where-Object { $_ -cmatch $Pattern })
    if ($receipts.Count -ne 1) { throw "Missing or ambiguous receipt for $Case`: $Stdout" }
    return $receipts[0]
}

try {
    # -SoftwareOnly controls runner selection; the compile guard also rejects
    # any accidental native adapter path inside the existing FG fixtures.
    $env:CL = ($previousCl + ' /DNR_NEXUS_TEST_WARP_ONLY').Trim()
    $suiteOutput = Join-Path $OutputDirectory 'suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite $suites -SoftwareOnly -UiPreview None -OutputDirectory $suiteOutput -RetentionMode Standard
    foreach ($policySuite in $policySuites) {
        $policy = Get-Content -LiteralPath (Join-Path $suiteOutput "$policySuite/result.json") -Raw | ConvertFrom-Json
        if ($policy.suite -cne $policySuite -or $policy.build -cne 'passed' -or $policy.run -cne 'passed' -or
            $policy.dependency_policy -cne 'passed') {
            throw "$policySuite requires a passing CPU run and STATIC dependency check."
        }
    }
    $fg = Join-Path $suiteOutput 'fg-native'
    $executable = Join-Path $fg 'fg_native_tests.exe'
    $inputs = @(Get-Content -LiteralPath (Join-Path $fg 'inputs.json') -Raw | ConvertFrom-Json)
    foreach ($case in $independentCases.Keys) {
        foreach ($input in $inputs) {
            if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
        }
        $directory = Initialize-TestOutput (Join-Path $OutputDirectory $case)
        Invoke-TestProcess $executable $directory 'test' @("--$case") -TimeoutMilliseconds 180000
        $receipt = Get-CoreCaseReceipt $case (Join-Path $directory 'test.stdout.log') $independentCases[$case]
        Complete-TestOutput $directory
        $summary.independent += @{name=$case;status='Passed';evidence='WARP+MOCK';receipt=$receipt
            executableSha256=(Get-FileHash -LiteralPath $executable).Hash}
    }
    $summary['presentFrameTrace'] = & (Join-Path $PSScriptRoot 'check-present-frame-traces.ps1') `
        -OrdinaryLog (Join-Path $OutputDirectory 'nr-edit-reuse-present-contracts/test.stdout.log') `
        -XefgLog (Join-Path $OutputDirectory 'nr-edit-reuse-xefg-contracts/test.stdout.log')
    foreach ($input in $inputs) {
        if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
    }

    # The Native x2 contract selects the DEV macro for this separate FG binary.
    $devOutput = Join-Path $OutputDirectory 'dev-suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite fg-native -NativeCase $devCases -SoftwareOnly -UiPreview None -OutputDirectory $devOutput -RetentionMode Standard
    $devFg = Join-Path $devOutput 'fg-native'
    $devResult = Get-Content -LiteralPath (Join-Path $devFg 'result.json') -Raw | ConvertFrom-Json
    if ($devResult.suite -cne 'fg-native' -or $devResult.build -cne 'passed' -or $devResult.run -cne 'passed') {
        throw 'Native DEV contracts require a passing FG build and run.'
    }
    $devExecutableHash = (Get-FileHash -LiteralPath (Join-Path $devFg 'fg_native_tests.exe')).Hash
    foreach ($case in $devCases) {
        $receipt = Get-CoreCaseReceipt $case (Join-Path $devFg "$case.stdout.log") "^PASS case=$case evidence=WARP\+MOCK dev=1$"
        $summary.independent += @{name=$case;status='Passed';evidence='WARP+MOCK';receipt=$receipt
            executableSha256=$devExecutableHash}
    }
    $outcome = 'Passed'
} finally {
    $env:CL = $previousCl
    $summary.status = $outcome
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    foreach ($directory in @($global:NexusTestOutputs.Keys | Where-Object {
        $_ -eq $OutputDirectory -or $_.StartsWith($OutputDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object Length -Descending)) { Complete-TestOutput $directory -Outcome $outcome }
}
