[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'tmp/ci-tests/core'))

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'tests/test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$previousCl = $env:CL
$outcome = 'Failed'
$suites = @('native','sdr-policy','hdr-policy','scope','config','constants','stage-plan','edit-reuse-policy',
    'adaptive-resolution','copy-slots','lifecycle','fg-native')
$independentCases = [ordered]@{
    'native-prefg-resolution'='^PASS native-prefg-resolution hardware=0 steady_pairs_observed='
    'present-chain'='^PASS B1 WARP format=DXGI_FORMAT_R8G8B8A8_UNORM source_input='
    'nr-edit-reuse-contracts-public'='^PASS case=nr-edit-reuse-contracts-public evidence=WARP\+MOCK dev=0$'
    'nr-edit-reuse-render-contracts'='^PASS case=nr-edit-reuse-render-contracts evidence=WARP\+MOCK dev=0$'
}
$summary = [ordered]@{ schema='nr-nexus-ci-core/1'; status='Running'
    sourceCommit=(& git --no-optional-locks -C $repo rev-parse HEAD)
    sourceTree=(& git --no-optional-locks -C $repo rev-parse 'HEAD^{tree}')
    evidence='STATIC / CPU / WARP+MOCK'; suites=$suites; independent=@() }
try {
    # -SoftwareOnly controls runner selection; the compile guard also rejects
    # any accidental native adapter path inside the existing FG fixtures.
    $env:CL = ($previousCl + ' /DNR_NEXUS_TEST_WARP_ONLY').Trim()
    $suiteOutput = Join-Path $OutputDirectory 'suites'
    & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite $suites -SoftwareOnly -UiPreview None -OutputDirectory $suiteOutput -RetentionMode Standard
    $policy = Get-Content -LiteralPath (Join-Path $suiteOutput 'edit-reuse-policy/result.json') -Raw | ConvertFrom-Json
    if ($policy.suite -cne 'edit-reuse-policy' -or $policy.build -cne 'passed' -or $policy.run -cne 'passed' -or
        $policy.dependency_policy -cne 'passed') {
        throw 'Edit reuse policy requires a passing CPU run and STATIC dependency check.'
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
        # A mistyped flag can return zero after running the default FG group.
        $stdout = Join-Path $directory 'test.stdout.log'
        $receipts = @(Get-Content -LiteralPath $stdout | Where-Object { $_ -cmatch $independentCases[$case] })
        if ($receipts.Count -ne 1) { throw "Missing or ambiguous receipt for $case`: $stdout" }
        Complete-TestOutput $directory
        $summary.independent += @{name=$case;status='Passed';evidence='WARP+MOCK';receipt=$receipts[0]
            executableSha256=(Get-FileHash -LiteralPath $executable).Hash}
    }
    foreach ($input in $inputs) {
        if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Core input changed: $($input.path)" }
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
