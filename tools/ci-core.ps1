[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'tmp/ci-tests/core'),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [hashtable]$ReusableSuites = @{},
    [hashtable]$ReusableIndependentCases = @{}
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'tests/test-support.ps1')
. (Join-Path $PSScriptRoot 'check-release-experiment-parity.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$previousCl = $env:CL
$outcome = 'Failed'
$policySuites = @('edit-reuse-policy','present-frame-policy','capability-policy','adaptive-resolution')
$suites = @('native','sdr-policy','hdr-policy','scope','config','constants','stage-plan') +
    $policySuites + @('copy-slots','lifecycle','fg-native','update','update-lifecycle')
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
    $candidateReceiptChecks = @(& (Join-Path $repo 'tests/candidate_acceptance_tests.ps1'))
    $candidateReceipt = @($candidateReceiptChecks | Where-Object {
        $_ -is [string] -and $_ -cmatch '^PASS candidate acceptance: [1-9][0-9]* receipt negatives, input drift and preflight integration\.$'
    })
    if ($candidateReceipt.Count -ne 1) { throw 'Candidate receipt validation contracts did not complete.' }
    $summary['candidateReceiptContracts'] = @{status='Passed'; evidence='CPU'; receipt=$candidateReceipt[0]}
    $registration = Get-ReleaseParityRegistration -SourceRoot $repo -AllowPending
    $summary['releaseParity'] = [ordered]@{ status='Running'; pending=$registration.pending; rows=@(); builds=@{} }
    if ($registration.pending.Count) {
        $summary.releaseParity.status = 'Pending'
        throw "Release parity migration pending: $($registration.pending -join ', ')."
    }
    $hostRoot = Join-Path $repo '.local/renodx'
    $buildContext = Get-ReleaseParityBuildContext $repo $hostRoot
    $parityCases = @($registration.rows | ForEach-Object { $_.cases })
    $pairedFgCases = @($parityCases | Where-Object suite -CEQ 'fg-native' | ForEach-Object name | Select-Object -Unique)
    $pairedBridgeCases = @($parityCases | Where-Object suite -CEQ 'bridge-motion' | ForEach-Object name | Select-Object -Unique)
    foreach ($case in $parityCases | Where-Object { $_.suite -ceq 'fg-native' -and $_.log -cne 'adaptive-inline' }) {
        $independentCases[$case.name] = $case.receipt.Replace('{dev}', '0')
    }
    $devCases = @($pairedFgCases + 'native-present-decision' | Select-Object -Unique)
    # -SoftwareOnly controls runner selection; the compile guard also rejects
    # any accidental native adapter path inside the existing FG fixtures.
    if ("$previousCl" -notmatch '(?i)(?:^|\s)[/-]D\s*NR_NEXUS_TEST_WARP_ONLY(?:\s|$)') {
        $env:CL = ($previousCl + ' /DNR_NEXUS_TEST_WARP_ONLY').Trim()
    }
    $compileInputs = @{BuildProject=$BuildProject; ShaderIncludeDirectory=$ShaderIncludeDirectory}
    $suiteOutput = Join-Path $OutputDirectory 'suites'
    $ordinary = @{}
    $reusedSuites = [ordered]@{}
    $needed = @(foreach ($suite in $suites) {
        $ordinary[$suite] = Join-Path $suiteOutput $suite
        if ($ReusableSuites.ContainsKey($suite)) {
            $directory = [IO.Path]::GetFullPath($ReusableSuites[$suite])
            Assert-TestArtifactPath $directory
            $resultPath = Join-Path $directory 'result.json'
            $inputsPath = Join-Path $directory 'inputs.json'
            # Legacy config, constants and lifecycle runners do not all emit
            # the current result and input manifests.
            if (!(Test-Path -LiteralPath $resultPath -PathType Leaf) -or !(Test-Path -LiteralPath $inputsPath -PathType Leaf)) {
                $suite
                continue
            }
            $state = Get-Content -LiteralPath (Join-Path $directory 'artifact-lifecycle.json') -Raw | ConvertFrom-Json -AsHashtable
            $result = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -AsHashtable
            $identity = $result['buildIdentity']
            $explicitOff = $suite -notin @('fg-native','adaptive-resolution','update','update-lifecycle') -or
                ($identity -and $identity.mode -ceq 'Off' -and $identity.softwareOnly -eq $true)
            if ($state.status -ceq 'Passed' -and $state.sourceCommit -ceq $summary.sourceCommit -and
                $result.suite -ceq $suite -and $result.build -ceq 'passed' -and $result.run -ceq 'passed' -and $explicitOff) {
                $manifest = Get-Content -LiteralPath $inputsPath -Raw | ConvertFrom-Json -AsHashtable -NoEnumerate
                $inputChanged = @($manifest | Where-Object {
                    (Get-FileHash -LiteralPath $_.path).Hash -cne $_.sha256
                })
                if (@($manifest).Count -and !$inputChanged.Count) {
                    $ordinary[$suite] = $directory
                    $reusedSuites[$suite] = $directory
                    continue
                }
            }
        }
        $suite
    })
    if ($needed.Count) {
        $baseRun = @{Suite=$needed; SoftwareOnly=$true; UiPreview='None'; OutputDirectory=$suiteOutput; RetentionMode='Standard'}
        if (@($needed | Where-Object { $_ -in @('fg-native','adaptive-resolution','update','update-lifecycle') }).Count) { $baseRun.DevControlMode = 'Off' }
        & (Join-Path $PSScriptRoot 'run-tests.ps1') @baseRun @compileInputs
    }
    $summary['reusedSuites'] = $reusedSuites
    foreach ($policySuite in $policySuites) {
        $policy = Get-Content -LiteralPath (Join-Path $ordinary[$policySuite] 'result.json') -Raw | ConvertFrom-Json
        if ($policy.suite -cne $policySuite -or $policy.build -cne 'passed' -or $policy.run -cne 'passed' -or
            $policy.dependency_policy -cne 'passed') {
            throw "$policySuite requires a passing CPU run and STATIC dependency check."
        }
    }
    $fg = $ordinary['fg-native']
    $executable = Join-Path $fg 'fg_native_tests.exe'
    $executableHash = (Get-FileHash -LiteralPath $executable).Hash
    $caseDirectories = @{}
    $inputs = @(Get-Content -LiteralPath (Join-Path $fg 'inputs.json') -Raw | ConvertFrom-Json)
    $summary['adaptiveInlineReceipt'] = Get-CoreCaseReceipt 'adaptive-inline' (Join-Path $fg 'adaptive-inline.stdout.log') `
        '^PASS case=adaptive-inline evidence=WARP\+MOCK dev=0$'
    $summary['adaptiveFgServiceReceipt'] = Get-CoreCaseReceipt 'adaptive-fg-service' (Join-Path $fg 'adaptive-fg-service.stdout.log') `
        '^PASS adaptive single/dual-color two-Pass Service evidence=WARP\+MOCK frames=[0-9]+ source_successes=[0-9]+ model_calls=[0-9]+ preparation_calls=[0-9]+ preparation_failures=[0-9]+$'
    foreach ($case in $independentCases.Keys) {
        foreach ($input in $inputs) {
            if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
        }
        $reuse = $ReusableIndependentCases[$case]
        $reused = ($reuse -and $reuse.executableSha256 -ceq $executableHash)
        if ($reused) {
            $directory = [IO.Path]::GetFullPath($reuse.logs)
            Assert-TestArtifactPath $directory
            $state = Get-Content -LiteralPath (Join-Path $directory 'artifact-lifecycle.json') -Raw | ConvertFrom-Json -AsHashtable
            if ($state.status -cne 'Passed' -or $state.sourceCommit -cne $summary.sourceCommit) {
                throw "Reused core case lacks a successful current-source run: $case"
            }
        } else {
            $directory = Initialize-TestOutput (Join-Path $OutputDirectory $case)
            Invoke-TestProcess $executable $directory 'test' @("--$case") -TimeoutMilliseconds 180000
        }
        $caseDirectories[$case] = $directory
        $stdout = Join-Path $directory 'test.stdout.log'
        $receipt = Get-CoreCaseReceipt $case $stdout $independentCases[$case]
        $caseResult = @{name=$case;status='Passed';receipt=$receipt;logs=$directory;reused=[bool]$reused
            evidence=$(if ($case -in @('adaptive-status','output-framerate-sdk')) { 'MOCK' } else { 'WARP+MOCK' })
            executableSha256=$executableHash}
        if ($adaptiveContractReceipts.ContainsKey($case)) {
            $caseResult.adaptiveContractReceipts = @(foreach ($pattern in $adaptiveContractReceipts[$case]) {
                Get-CoreCaseReceipt "$case adaptive contract $pattern" $stdout $pattern
            })
        }
        if (!$reused) { Complete-TestOutput $directory }
        $summary.independent += $caseResult
    }
    $summary['presentFrameTrace'] = & (Join-Path $PSScriptRoot 'check-present-frame-traces.ps1') `
        -OrdinaryLog (Join-Path $caseDirectories['nr-edit-reuse-present-contracts'] 'test.stdout.log') `
        -XefgLog (Join-Path $caseDirectories['nr-edit-reuse-xefg-contracts'] 'test.stdout.log')
    foreach ($input in $inputs) {
        if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
    }

    $bridgeOutput = Join-Path $OutputDirectory 'bridge-suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite bridge-motion -BridgeCase $pairedBridgeCases -DevControlMode Off `
        -SoftwareOnly -UiPreview None -OutputDirectory $bridgeOutput -RetentionMode Standard @compileInputs
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
        -PublicLog (Join-Path $caseDirectories['nr-edit-reuse-contracts-public'] 'test.stdout.log') -BridgeLog $bridgeStdout

    $devOutput = Join-Path $OutputDirectory 'dev-suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite fg-native,bridge-motion,adaptive-resolution,update,update-lifecycle `
        -NativeCase $devCases -BridgeCase $pairedBridgeCases -DevControlMode On `
        -SoftwareOnly -UiPreview None -OutputDirectory $devOutput -RetentionMode Standard @compileInputs
    $after = Get-ReleaseParityBuildContext $repo $hostRoot
    if (($after | ConvertTo-Json -Depth 5 -Compress) -cne ($buildContext | ConvertTo-Json -Depth 5 -Compress)) {
        throw 'Core source or locked dependencies changed between the two modes.'
    }
    $ordinaryBuilds = @{ 'fg-native'=$fg; 'bridge-motion'=$bridge; 'adaptive-resolution'=$ordinary['adaptive-resolution']; update=$ordinary['update']; 'update-lifecycle'=$ordinary['update-lifecycle'] }
    foreach ($suite in $ordinaryBuilds.Keys) {
        $summary.releaseParity.builds[$suite] = Assert-ReleaseParityBuildPair $suite $ordinaryBuilds[$suite] `
            (Join-Path $devOutput $suite) $buildContext
    }
    $devController = Get-Content -LiteralPath (Join-Path $devOutput 'adaptive-resolution/result.json') -Raw | ConvertFrom-Json
    if ($devController.dependency_policy -cne 'passed') { throw 'DEV controller requires STATIC dependency isolation.' }
    $devFg = Join-Path $devOutput 'fg-native'
    $summary.independent += @{name='native-present-decision';status='Passed';evidence='WARP+MOCK'
        receipt=(Get-CoreCaseReceipt 'native-present-decision' (Join-Path $devFg 'native-present-decision.stdout.log') `
            '^PASS case=native-present-decision evidence=WARP\+MOCK dev=1$')
        executableSha256=$summary.releaseParity.builds['fg-native'].On.executableSha256}
    foreach ($row in $registration.rows) {
        $cases = @(foreach ($case in $row.cases) {
            $pair = $summary.releaseParity.builds[$case.suite]
            if ($pair.On.cases -cnotcontains $case.name) { throw "Missing DEV invocation: $($case.suite)/$($case.name)" }
            $offDirectory = $ordinaryBuilds[$case.suite]
            $offLogName = "$($case.log).stdout.log"
            if ($case.suite -ceq 'fg-native' -and $independentCases.Contains($case.name)) {
                $offDirectory = $caseDirectories[$case.name]
                $offLogName = 'test.stdout.log'
            } elseif ($pair.Off.cases -cnotcontains $case.name) {
                throw "Missing ordinary invocation: $($case.suite)/$($case.name)"
            }
            $receipt = Compare-ReleaseParityCase $row.id $case (Join-Path $offDirectory $offLogName) `
                (Join-Path $devOutput "$($case.suite)/$($case.log).stdout.log")
            $receipt['suite'] = $case.suite
            $receipt['ordinaryExeSha256'] = $pair.Off.executableSha256
            $receipt['devExeSha256'] = $pair.On.executableSha256
            $receipt
        })
        $summary.releaseParity.rows += @{ id=$row.id; status='Passed'; cases=$cases }
    }
    $summary.releaseParity.status = 'Passed'
    $outcome = 'Passed'
} finally {
    $env:CL = $previousCl
    $summary.status = $outcome
    if ($summary.Contains('releaseParity') -and $summary.releaseParity.status -eq 'Running') {
        $summary.releaseParity.status = 'Failed'
    }
    $summary | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    foreach ($directory in @($global:NexusTestOutputs.Keys | Where-Object {
        $_ -eq $OutputDirectory -or $_.StartsWith($OutputDirectory + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)
    } | Sort-Object Length -Descending)) { Complete-TestOutput $directory -Outcome $outcome }
}
