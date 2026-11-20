[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'tmp/ci-tests/core'))

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'tests/test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$previousCl = $env:CL
$outcome = 'Failed'
$policySuites = @('edit-reuse-policy','present-frame-policy','capability-policy','adaptive-resolution')
$suites = @('native','sdr-policy','hdr-policy','scope','config','constants','stage-plan') +
    $policySuites + @('copy-slots','lifecycle','fg-native')
$independentCases = [ordered]@{
    'native-prefg-resolution'='^PASS native-prefg-resolution hardware=0 steady_pairs_observed='
    'present-chain'='^PASS B1 WARP format=DXGI_FORMAT_R8G8B8A8_UNORM source_input='
    'adaptive-status'='^PASS case=adaptive-status evidence=MOCK$'
    'output-framerate-registration'='^PASS case=output-framerate-registration evidence=WARP\+MOCK dev=0$'
    'output-framerate-sdk'='^PASS output SDK callback ownership and count provenance evidence=MOCK$'
    'native-options-delayed'='^PASS case=native-options-delayed evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-contracts-public'='^PASS case=nr-edit-reuse-contracts-public evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-render-contracts'='^PASS case=nr-edit-reuse-render-contracts evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-present-contracts'='^PASS case=nr-edit-reuse-present-contracts evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-xefg-contracts'='^PASS case=nr-edit-reuse-xefg-contracts evidence=WARP\+MOCK dev=0$'
}
$adaptiveContractReceipts = @{
    'nr-edit-reuse-render-contracts'='^ADAPTIVE_DELIVERY_RENDER evidence=WARP\+MOCK rejected_sr_color_restored=1 recovered_reset=1$'
    'nr-edit-reuse-present-contracts'=@(
        '^ADAPTIVE_DELIVERY_PRESENT evidence=WARP\+MOCK failed_signal_writes=1 recovered_reset=1 base_claim_delta=0$'
        '^ADAPTIVE_OUTPUT_SOURCE evidence=WARP\+MOCK framed_tags=1 legacy_tags=1 actual_sr=1 release_revoked=1$'
        '^ADAPTIVE_OUTPUT_NGX evidence=WARP\+MOCK stages=Render,Upscaled intermediate=1 full_copy=1 rejected_partial_recording_unsubmitted=1$'
    )
    'nr-edit-reuse-xefg-contracts'='^ADAPTIVE_DELIVERY_XEFG evidence=WARP\+MOCK rejected_proxy=1 restored_anchor=1 late_receipt=ignored$'
}
$devCases = @('nr-edit-reuse-native-contracts','native-present-decision')
$summary = [ordered]@{ schema='nr-nexus-ci-core/1'; status='Running'
    sourceCommit=(& git --no-optional-locks -C $repo rev-parse HEAD)
    sourceTree=(& git --no-optional-locks -C $repo rev-parse 'HEAD^{tree}')
    evidence='STATIC / CPU / MOCK / WARP+MOCK'; suites=$suites; independent=@() }

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
    $summary['adaptiveInlineReceipt'] = Get-CoreCaseReceipt 'adaptive-inline' (Join-Path $fg 'adaptive-inline.stdout.log') `
        '^PASS case=adaptive-inline evidence=WARP\+MOCK dev=0$'
    $summary['adaptiveFgServiceReceipt'] = Get-CoreCaseReceipt 'adaptive-fg-service' (Join-Path $fg 'adaptive-fg-service.stdout.log') `
        '^PASS adaptive single/dual-color two-Pass Service evidence=WARP\+MOCK frames=[0-9]+ source_successes=[0-9]+ model_calls=[0-9]+ preparation_calls=[0-9]+ preparation_failures=[0-9]+$'
    foreach ($case in $independentCases.Keys) {
        foreach ($input in $inputs) {
            if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
        }
        $directory = Initialize-TestOutput (Join-Path $OutputDirectory $case)
        Invoke-TestProcess $executable $directory 'test' @("--$case") -TimeoutMilliseconds 180000
        $stdout = Join-Path $directory 'test.stdout.log'
        $receipt = Get-CoreCaseReceipt $case $stdout $independentCases[$case]
        $caseResult = @{name=$case;status='Passed';receipt=$receipt
            evidence=$(if ($case -in @('adaptive-status','output-framerate-sdk')) { 'MOCK' } else { 'WARP+MOCK' })
            executableSha256=(Get-FileHash -LiteralPath $executable).Hash}
        if ($adaptiveContractReceipts.ContainsKey($case)) {
            $caseResult.adaptiveContractReceipts = @(foreach ($pattern in $adaptiveContractReceipts[$case]) {
                Get-CoreCaseReceipt "$case adaptive contract $pattern" $stdout $pattern
            })
        }
        Complete-TestOutput $directory
        $summary.independent += $caseResult
    }
    $summary['presentFrameTrace'] = & (Join-Path $PSScriptRoot 'check-present-frame-traces.ps1') `
        -OrdinaryLog (Join-Path $OutputDirectory 'nr-edit-reuse-present-contracts/test.stdout.log') `
        -XefgLog (Join-Path $OutputDirectory 'nr-edit-reuse-xefg-contracts/test.stdout.log')
    foreach ($input in $inputs) {
        if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
    }

    $bridgeOutput = Join-Path $OutputDirectory 'bridge-suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite bridge-motion -BridgeCase edit-reuse-production `
        -SoftwareOnly -UiPreview None -OutputDirectory $bridgeOutput -RetentionMode Standard
    $bridge = Join-Path $bridgeOutput 'bridge-motion'
    $bridgeResult = Get-Content -LiteralPath (Join-Path $bridge 'result.json') -Raw | ConvertFrom-Json
    if ($bridgeResult.suite -cne 'bridge-motion' -or $bridgeResult.build -cne 'passed' -or $bridgeResult.run -cne 'passed') {
        throw 'Bridge production edit reuse requires a passing Bridge build and run.'
    }
    $bridgeStdout = Join-Path $bridge 'warp-edit-reuse-production.stdout.log'
    $bridgeReceipt = Get-CoreCaseReceipt 'bridge-edit-reuse-production' $bridgeStdout `
        '^PASS case=bridge-edit-reuse-production evidence=WARP\+MOCK dev=0$'
    $summary.independent += @{name='bridge-edit-reuse-production';status='Passed';evidence='WARP+MOCK';receipt=$bridgeReceipt
        executableSha256=(Get-FileHash -LiteralPath (Join-Path $bridge 'd3d11_bridge_motion_tests.exe')).Hash}
    $summary['inlineFrameTrace'] = & (Join-Path $PSScriptRoot 'check-inline-frame-traces.ps1') `
        -PublicLog (Join-Path $OutputDirectory 'nr-edit-reuse-contracts-public/test.stdout.log') -BridgeLog $bridgeStdout

    # The Native x2 contract selects the DEV macro for this separate FG binary.
    $devOutput = Join-Path $OutputDirectory 'dev-suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite fg-native,bridge-motion -NativeCase $devCases -BridgeCase adaptive `
        -SoftwareOnly -UiPreview None -OutputDirectory $devOutput -RetentionMode Standard
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
    $devBridge = Join-Path $devOutput 'bridge-motion'
    $devBridgeResult = Get-Content -LiteralPath (Join-Path $devBridge 'result.json') -Raw | ConvertFrom-Json
    if ($devBridgeResult.suite -cne 'bridge-motion' -or $devBridgeResult.build -cne 'passed' -or $devBridgeResult.run -cne 'passed') {
        throw 'Bridge adaptive XeFG Off requires a passing DEV Bridge build and run.'
    }
    $devBridgeReceipt = Get-CoreCaseReceipt 'bridge-adaptive-xefg-off' (Join-Path $devBridge 'warp-adaptive.stdout.log') `
        '^PASS Bridge Adaptive XeFG Off output ownership, resize and failure revocation$'
    $summary.independent += @{name='bridge-adaptive-xefg-off';status='Passed';evidence='WARP+MOCK';receipt=$devBridgeReceipt
        executableSha256=(Get-FileHash -LiteralPath (Join-Path $devBridge 'd3d11_bridge_motion_tests.exe')).Hash}
    $outcome = 'Passed'
} finally {
    $env:CL = $previousCl
    $summary.status = $outcome
    $summary | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    foreach ($directory in @($global:NexusTestOutputs.Keys | Where-Object {
        $_ -eq $OutputDirectory -or $_.StartsWith($OutputDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object Length -Descending)) { Complete-TestOutput $directory -Outcome $outcome }
}
