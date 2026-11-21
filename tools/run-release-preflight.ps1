<#
.SYNOPSIS
Local release checks. Software is the default; native modes require a separately
coordinated device window and never start a game. GitHub CI keeps its own scope.
.DESCRIPTION
Software retains Suite all, then reuses its executables for existing independent
stage, resolution, source-lifecycle and pre-FG cases. ReuseAllOutputDirectory may
reuse a completed all run only when its source and recorded inputs still match.
NativeGpu runs real D3D12 with model stand-ins. NativeNgx requires the FG case's
native inline handoff report and the paired full/on-demand adaptive preparation
cost reports. NativeNgx requires an explicit SR runtime.
CandidateAcceptance checks receipts for one specified candidate and run. It
launches no workload and reports missing host evidence as PendingHost.
#>
[CmdletBinding()]
param(
    [ValidateSet('Software','NativeGpu','NativeNgx','CandidateAcceptance')][string]$Mode = 'Software',
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/release-preflight/' + [guid]::NewGuid().ToString('N'))),
    [string]$ReuseAllOutputDirectory,
    [switch]$RefreshFg,
    [string]$PythonExecutable,
    [string]$VulkanIncludeDirectory,
    [string]$NativeNgxProjectIdPath,
    [string]$NativeNgxSdkLibraryDirectory,
    [string]$NativeNgxRuntimePath,
    [string]$NativeNgxSrRuntimePath,
    [string]$CandidateReceipt,
    [string]$CandidateDll,
    [string]$CandidateRunId,
    [string]$CandidateOfflineCase,
    [string[]]$CandidateOfflineStages,
    [string]$CandidateHostCase,
    [string[]]$CandidateHostStages
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $repo 'tests/test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$source = (& git --no-optional-locks -C $repo rev-parse HEAD).Trim()
$previousCl = $env:CL
$outcome = 'Failed'
$checks = [Collections.Generic.List[object]]::new()
$nativeCases = @('stage-chain','stage-resolution','same-stage-resolution','render-stage-resolution',
    'present-stage-resolution','native-prefg-resolution','source-lifecycle','feature-switch',
    'native-required-menu','native-owner-idle','native-options-resume','native-options-delayed',
    'native-guide-contract','native-present-decision','present-chain',
    'nr-edit-reuse-contracts','nr-edit-reuse-xefg-contracts')
$bridgeCases = @('stage-resolution','same-stage-resolution','render-stage-resolution','present-stage-resolution')
$summary = [ordered]@{
    schema='nr-nexus-local-preflight/1'; mode=$Mode; sourceCommit=$source
    workingTreeFiles=@(& git --no-optional-locks -C $repo diff --name-only)
    startedUtc=[DateTimeOffset]::UtcNow.ToString('o'); status='Running'
    entrySha256=(Get-FileHash $PSCommandPath).Hash; checks=$checks
    candidateAcceptance=@{status='NotEvaluated'; candidateComplete=$false}
    limits=@('No game or dynamic visual acceptance.',
        'Software uses NR/FG model stand-ins; native modes are separate.',
        'Bridge and native-present-decision fixtures do not assert all InfoQueue messages.',
        'Adaptive inline retains its existing initial phase; no new phase matrix.')
}
try {
    if ($Mode -eq 'CandidateAcceptance') {
        $summary.limits = @('Receipt verification for the selected candidate and cases.', 'Subjective VISUAL is outside this check.')
        . (Join-Path $PSScriptRoot 'check-candidate-acceptance.ps1')
        $candidate = Test-CandidateAcceptance -ReceiptPath $CandidateReceipt -DllPath $CandidateDll -RunId $CandidateRunId `
            -OfflineCase $CandidateOfflineCase -OfflineStages $CandidateOfflineStages `
            -HostCase $CandidateHostCase -HostStages $CandidateHostStages
        $summary.candidateAcceptance = $candidate
        if (-not $candidate.candidateComplete) {
            $outcome = $candidate.status
            throw "Candidate acceptance $($candidate.status): $($candidate.reason)"
        }
    } elseif ($Mode -eq 'Software') {
        $env:CL = ($previousCl + ' /DNR_NEXUS_TEST_WARP_ONLY').Trim()
        $all = if ($ReuseAllOutputDirectory) { [IO.Path]::GetFullPath($ReuseAllOutputDirectory) } else { Join-Path $OutputDirectory 'all' }
        Assert-TestArtifactPath $all
        if (-not $ReuseAllOutputDirectory) {
            $arguments = @{Suite=@('all');SoftwareOnly=$true;UiPreview='None';OutputDirectory=$all;RetentionMode='Standard'}
            if ($PythonExecutable) { $arguments.PythonExecutable = $PythonExecutable }
            if ($VulkanIncludeDirectory) { $arguments.VulkanIncludeDirectory = $VulkanIncludeDirectory }
            & (Join-Path $PSScriptRoot 'run-tests.ps1') @arguments
        }
        $allState = Get-Content (Join-Path $all 'artifact-lifecycle.json') -Raw | ConvertFrom-Json -AsHashtable
        if ($allState.status -ne 'Passed') { throw 'Suite all has no successful terminal record.' }
        # The recorded compiler inputs below bind each retained result to its
        # actual code and dependencies, including uncommitted fixture fixes.
        & git --no-optional-locks -C $repo diff --quiet $allState.sourceCommit $source -- tools/run-tests.ps1 tests/test-support.ps1
        if ($LASTEXITCODE -ne 0) { throw 'Suite all runner changed; run a fresh all precheck.' }
        $fgDirectory = Join-Path $all 'fg-native'
        if ($RefreshFg) {
            $fgRoot = Join-Path $OutputDirectory 'refreshed-fg'
            # No NativeCase: preserve the default groups before additions.
            & (Join-Path $PSScriptRoot 'run-tests.ps1') -Suite fg-native -SoftwareOnly -UiPreview None -OutputDirectory $fgRoot
            $fgDirectory = Join-Path $fgRoot 'fg-native'
        }
        $verified = @{}
        $checkoutEquivalent = @{}
        $inputFiles = @(Get-ChildItem -LiteralPath $all -Filter '*inputs.json' -File -Recurse | Where-Object {
            -not $RefreshFg -or $_.DirectoryName -ne (Join-Path $all 'fg-native')
        })
        if ($RefreshFg) { $inputFiles += Get-Item (Join-Path $fgDirectory 'inputs.json') }
        foreach ($file in $inputFiles) {
            $data = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json -AsHashtable
            $inputs = if ($data -is [Collections.IDictionary]) { $data['inputs'] } else { $data }
            foreach ($inputRecord in $inputs) {
                if (-not $inputRecord.path -or -not $inputRecord.sha256) { throw "Unsupported input record: $($file.Name)" }
                if (-not $verified.ContainsKey($inputRecord.path)) {
                    $verified[$inputRecord.path] = (Get-FileHash -LiteralPath $inputRecord.path).Hash
                }
                if ($verified[$inputRecord.path] -ne $inputRecord.sha256) {
                    # A branch checkout can turn the same committed LF input
                    # into CRLF. Accept only these exact blob-derived bytes;
                    # external dependencies and other changes still fail.
                    if (-not $checkoutEquivalent.ContainsKey($inputRecord.path)) {
                        $relative = [IO.Path]::GetRelativePath($repo, $inputRecord.path).Replace('\','/')
                        if ($relative.StartsWith('../') -or [IO.Path]::IsPathRooted($relative)) {
                            throw "Suite all input changed: $($inputRecord.path)"
                        }
                        $gitBlob = [Diagnostics.Process]::new()
                        $gitBlob.StartInfo = [Diagnostics.ProcessStartInfo]::new('git')
                        $gitBlob.StartInfo.WorkingDirectory = $repo
                        $gitBlob.StartInfo.UseShellExecute = $false
                        $gitBlob.StartInfo.CreateNoWindow = $true
                        $gitBlob.StartInfo.RedirectStandardOutput = $true
                        $gitBlob.StartInfo.RedirectStandardError = $true
                        foreach ($argument in @('cat-file','blob',"$($allState.sourceCommit):$relative")) { $gitBlob.StartInfo.ArgumentList.Add($argument) }
                        $bytes = [IO.MemoryStream]::new()
                        try {
                            [void]$gitBlob.Start()
                            $gitBlob.StandardOutput.BaseStream.CopyTo($bytes)
                            $gitError = $gitBlob.StandardError.ReadToEnd()
                            $gitBlob.WaitForExit()
                            if ($gitBlob.ExitCode -ne 0) { throw "Cannot verify source input: $relative ($gitError)" }
                            $blob = $bytes.ToArray()
                        } finally { $bytes.Dispose(); $gitBlob.Dispose() }
                        $utf8 = [Text.UTF8Encoding]::new($false, $true)
                        $blobText = $utf8.GetString($blob)
                        if ($blobText.Contains("`r") -or $blobText.Contains([char]0)) { throw "Input is not a committed LF text file: $relative" }
                        $hashes = @([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($blob)),
                            [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes($blobText.Replace("`n","`r`n")))))
                        if ($verified[$inputRecord.path] -notin $hashes) { throw "Suite all input changed: $($inputRecord.path)" }
                        $checkoutEquivalent[$inputRecord.path] = $hashes
                    }
                    if ($inputRecord.sha256 -notin $checkoutEquivalent[$inputRecord.path]) { throw "Suite all recorded input differs from source: $($inputRecord.path)" }
                }
            }
        }
        $summary['allEvidence'] = @{directory=$all;sourceCommit=$allState.sourceCommit;reused=[bool]$ReuseAllOutputDirectory;verifiedInputs=$verified.Count;checkoutLineEndingsOnly=@($checkoutEquivalent.Keys);fgDirectory=$fgDirectory;refreshedFg=[bool]$RefreshFg}
        $required = @('report','ui','presets','unicode-input','branding','native','sdr-policy','lastgasp',
            'hdr-policy','scope','config','constants','control-mask','control-mask-execution','vulkan-control-mask',
            'fg-xefg','fg-native','lifecycle','bridge-motion','stage-plan','adaptive-resolution','copy-slots')
        foreach ($suite in $required) {
            $directory = if ($suite -eq 'fg-native') { $fgDirectory } else { Join-Path $all $suite }
            $state = Get-Content (Join-Path $directory 'artifact-lifecycle.json') -Raw | ConvertFrom-Json -AsHashtable
            if ($state.status -ne 'Passed') { throw "Suite did not complete: $suite" }
            $checks.Add(@{name="all/$suite";status='Passed';sourceCommit=$state.sourceCommit;evidence=$directory})
        }
        $vulkan = $checks | Where-Object name -EQ 'all/vulkan-control-mask'
        $vulkan.status = 'NotRun'
        $vulkan['reason'] = 'Build passed; execution requires native Vulkan GPU/loader.'
        foreach ($case in @('test','f5-mixed','control-mask','adaptive-fg-service','adaptive-inline','present-guide-models')) {
            if (-not (Test-Path (Join-Path $fgDirectory "$case.stdout.log"))) { throw "Default FG group is missing: $case" }
        }
        $defaultLog = Get-Content (Join-Path $fgDirectory 'test.stdout.log') -Raw
        if ($defaultLog -notmatch 'D3D12 debug layer: enabled' -or $defaultLog -notmatch 'errors=0') {
            throw 'Required default FG Debug Layer/InfoQueue validation is absent; not a passed graphics-state check.'
        }
        $extra = Initialize-TestOutput (Join-Path $OutputDirectory 'independent')
        $plan = @($nativeCases | ForEach-Object { @{name="fg/$_";log="fg-$_";exe=(Join-Path $fgDirectory 'fg_native_tests.exe');arguments=@("--$_");evidence=$(if ($_ -eq 'source-lifecycle') {'CPU'} else {'WARP+MOCK'})} })
        $plan += @($bridgeCases | ForEach-Object { @{name="bridge/$_";log="bridge-$_";exe=(Join-Path $all 'bridge-motion/d3d11_bridge_motion_tests.exe');arguments=@('--warp',"--$_");evidence='WARP+MOCK'} })
        foreach ($case in $plan) { $checks.Add(@{name=$case.name;status='NotRun';reason='not reached';evidence=$case.evidence}) }
        foreach ($case in $plan) {
            $record = $checks | Where-Object name -EQ $case.name
            $caseOutput = Initialize-TestOutput (Join-Path $extra $case.log)
            $record['logs'] = $caseOutput
            try {
                $record['executableSha256'] = (Get-FileHash $case.exe).Hash
                Invoke-TestProcess $case.exe $caseOutput 'test' $case.arguments -TimeoutMilliseconds 180000
                Complete-TestOutput $caseOutput
                $record.status = 'Passed'; [void]$record.Remove('reason')
            } catch {
                $record.status = 'Failed'; $record.reason = $_.Exception.Message
                Complete-TestOutput $caseOutput -Outcome Failed
            }
        }
        $extraOutcome = if (@($checks | Where-Object status -EQ 'Failed').Count) {'Failed'} else {'Passed'}
        Complete-TestOutput $extra -Outcome $extraOutcome
        foreach ($name in @('NATIVE_GPU','NATIVE_NGX','same-stage-model-pair','same-stage-mixed-pair')) {
            $checks.Add(@{name=$name;status='NotRun';reason='Requires a separately coordinated native window.'})
        }
        foreach ($name in @('IN_GAME','VISUAL','DEV-only diagnostics/control','HDR format variants')) {
            $checks.Add(@{name=$name;status='NotRun';reason='Outside the selected release preflight scope; no result inferred from other cases.'})
        }
        if ($extraOutcome -eq 'Failed') { throw 'Independent preflight cases failed; see individual results.' }
    } else {
        if ($ReuseAllOutputDirectory) { throw 'Native mode does not rerun or consume Suite all.' }
        if ($previousCl -match 'NR_NEXUS_TEST_WARP_ONLY') { throw 'Native mode conflicts with the inherited WARP-only build guard.' }
        $summary['softwareDefaultGroups'] = 'NotRun in this native branch; use the separate Software result.'
        $arguments = @{Suite=@('fg-native');OutputDirectory=(Join-Path $OutputDirectory 'native');RetentionMode='Standard'}
        if ($Mode -eq 'NativeGpu') {
            # Compile once, then keep separate receipts for the existing core,
            # pre-FG and copy-chain cases affected by the release fixes.
            $arguments.BuildOnly = $true
            $hardwareCases = @('native-full-hardware','native-prefg-resolution-hardware','present-chain-hardware')
            foreach ($case in $hardwareCases) {
                $checks.Add(@{name=$case;status='NotRun';evidence='NATIVE_GPU+MOCK';reason='not started'})
            }
            $hardwareOutcome = 'Failed'
            try {
                & (Join-Path $PSScriptRoot 'run-tests.ps1') @arguments
                $executable = Join-Path $arguments.OutputDirectory 'fg-native/fg_native_tests.exe'
                foreach ($record in $checks) {
                    $caseOutput = Initialize-TestOutput (Join-Path $OutputDirectory $record.name)
                    $record['logs'] = $caseOutput
                    $record['executableSha256'] = (Get-FileHash $executable).Hash
                    try {
                        Invoke-TestProcess $executable $caseOutput 'test' @("--$($record.name)") -TimeoutMilliseconds 180000
                        Complete-TestOutput $caseOutput
                        $record.status='Passed'; [void]$record.Remove('reason')
                    } catch {
                        $record.status='Failed'; $record.reason=$_.Exception.Message
                        Complete-TestOutput $caseOutput -Outcome Failed
                        throw
                    }
                }
                $hardwareOutcome = 'Passed'
            } finally {
                # BuildOnly deliberately retains its registration until its
                # caller has finished consuming the compiled executable.
                foreach ($directory in @((Join-Path $arguments.OutputDirectory 'fg-native'), $arguments.OutputDirectory)) {
                    if (Test-Path (Join-Path $directory 'artifact-lifecycle.json')) {
                        Complete-TestOutput $directory -Outcome $hardwareOutcome
                    }
                }
            }
        } else {
            $fgCase = 'control-mask-native-ngx-fg'
            $resizeCase = 'control-mask-native-ngx-resize'
            $checks.Add(@{name=$fgCase;case=$fgCase;status='NotRun';evidence='NATIVE_GPU+NATIVE_NGX+MOCK_XeFG';
                output=$arguments.OutputDirectory;reason='not reached';
                report=(Join-Path $arguments.OutputDirectory 'fg-native/xefg-inline-handoff-native.json')})
            foreach ($condition in @('full','on-demand')) {
                $name = "adaptive-preparation-cost-$condition"
                $output = Join-Path $OutputDirectory $name
                $directory = Join-Path $output 'fg-native'
                $checks.Add(@{name=$name;case=$resizeCase;condition=$condition;status='NotRun';
                    evidence='NATIVE_GPU+NATIVE_NGX';output=$output;reason='not reached';
                    stdout=(Join-Path $directory "$resizeCase.stdout.log");
                    report=(Join-Path $directory 'adaptive-preparation-cost.json')})
            }
            foreach ($pair in @(@('NativeNgxProjectIdPath',$NativeNgxProjectIdPath),@('NativeNgxSdkLibraryDirectory',$NativeNgxSdkLibraryDirectory),@('NativeNgxRuntimePath',$NativeNgxRuntimePath))) {
                if (-not $pair[1] -or -not (Test-Path -LiteralPath $pair[1])) {
                    $reason = "Native NGX dependency missing: $($pair[0])"
                    foreach ($record in $checks) { $record.reason = $reason }
                    throw $reason
                }
                $arguments[$pair[0]] = $pair[1]
            }
            if (-not $NativeNgxSrRuntimePath -or -not (Test-Path -LiteralPath $NativeNgxSrRuntimePath -PathType Leaf)) {
                $reason = 'Native NGX dependency missing: -NativeNgxSrRuntimePath must point to an existing SR DLL.'
                foreach ($record in $checks) { $record.reason = $reason }
                throw $reason
            }
            $arguments.NativeNgxSrRuntimePath = $NativeNgxSrRuntimePath
            $arguments.DevControlMode = 'Off'
            $previousCost = $env:NR_NEXUS_TEST_RESIZE_PREPARATION_COST
            $fullInput = $null
            foreach ($record in $checks) {
                $arguments.NativeCase = @($record.case)
                $arguments.OutputDirectory = $record.output
                $condition = $record['condition']
                try {
                    if ($condition) { $env:NR_NEXUS_TEST_RESIZE_PREPARATION_COST = $condition }
                    & (Join-Path $PSScriptRoot 'run-tests.ps1') @arguments
                    if ($condition) {
                        $expected = "PASS adaptive-preparation-cost evidence=NATIVE_GPU+NATIVE_NGX condition=$condition levels=3 passes=1 steady_per_tier=24 readbacks=2"
                        if (@(Get-Content -LiteralPath $record.stdout | Where-Object { $_ -ceq $expected }).Count -ne 1) {
                            throw "Expected exactly one '$expected' in $($record.stdout)."
                        }
                        $cost = Get-Content -LiteralPath $record.report -Raw | ConvertFrom-Json -AsHashtable
                        if ($cost['schema'] -cne 'nr-nexus-adaptive-preparation-cost/1' -or $cost['status'] -cne 'Passed' -or
                            $cost['evidence'] -cne 'NATIVE_GPU+NATIVE_NGX' -or $cost['condition'] -cne $condition -or
                            $cost['host_sr'] -cne 'NATIVE_NGX' -or $cost['requested_model'] -cne 'B' -or
                            $cost['passes'] -ne 1 -or $cost['plan']['levels'] -ne 3 -or -not $cost['input']) {
                            throw "Invalid adaptive preparation cost receipt for $condition."
                        }
                        $inputIdentity = ConvertTo-Json -InputObject $cost.input -Depth 8 -Compress
                        if ($condition -eq 'full') { $fullInput = $inputIdentity }
                        elseif ($inputIdentity -cne $fullInput) { throw 'Full and on-demand cost receipts have different inputs.' }
                    } else {
                        $handoff = Get-Content -LiteralPath $record.report -Raw | ConvertFrom-Json -AsHashtable
                        $expectedStages = @('Present', 'Render', 'Present', 'Upscaled', 'Present')
                        if ($handoff -isnot [Collections.IDictionary] -or
                            $handoff['schema'] -cne 'nr-nexus-xefg-inline-handoff/1' -or
                            $handoff['status'] -cne 'Passed' -or $handoff['evidence'] -cne $record.evidence -or
                            $handoff['case'] -cne 'xefg-held-inline-handoff' -or
                            $handoff['phases'] -isnot [array] -or $handoff.phases.Count -ne $expectedStages.Count) {
                            throw 'Native FG requires a Passed xefg-held-inline-handoff report with all five phases.'
                        }
                        if ($handoff['sr_evidence'] -cne 'NATIVE_NGX' -or
                            $handoff['sdk_fg']['on_samples'] -lt 10 -or $handoff['sdk_fg']['off_samples'] -ne 0) {
                            throw 'Native handoff requires real SR and FG held on across all delivered frames.'
                        }
                        $previousSequence = 0L
                        for ($phaseIndex = 0; $phaseIndex -lt $expectedStages.Count; $phaseIndex++) {
                            $phase = $handoff.phases[$phaseIndex]
                            if ($phase -isnot [Collections.IDictionary] -or $phase['stage'] -isnot [string] -or
                                $phase.stage -cne $expectedStages[$phaseIndex] -or $phase['frames'] -isnot [array] -or
                                $phase.frames.Count -ne 2) {
                                throw "Native FG handoff phase $($phaseIndex + 1) must be $($expectedStages[$phaseIndex])."
                            }
                            foreach ($frame in $phase.frames) {
                                if ($frame['accepted_evaluate_sequence'] -le $previousSequence -or
                                    $frame['delivery_complete'] -ne $true -or $frame['readback_complete'] -ne $true -or
                                    $frame['readback_pixels'] -le 0 -or $frame['gpu_fence_healthy'] -ne $true -or
                                    $frame['readback_fence_value'] -le 0 -or
                                    $frame['readback_fence_completed'] -lt $frame.readback_fence_value -or
                                    $frame['sdk_fg_enabled'] -ne $true) {
                                    throw 'Native FG handoff lacks a fresh accepted Evaluate, complete delivery or healthy readback.'
                                }
                                $previousSequence = $frame.accepted_evaluate_sequence
                            }
                        }
                        $record.stages = @($handoff.phases | ForEach-Object stage)
                    }
                    $record.reportSha256 = (Get-FileHash -LiteralPath $record.report).Hash
                    $caseDirectory = Join-Path $record.output 'fg-native'
                    $result = Get-Content -LiteralPath (Join-Path $caseDirectory 'result.json') -Raw | ConvertFrom-Json -AsHashtable
                    $identity = $result['buildIdentity']
                    if ($result['build'] -cne 'passed' -or $result['run'] -cne 'passed' -or
                        $identity['mode'] -cne 'Off' -or $identity['executable'] -cne 'fg_native_tests.exe') {
                        throw 'Native NGX run has no completed ordinary EXE identity.'
                    }
                    $executable = Join-Path $caseDirectory $identity.executable
                    if ((Get-FileHash -LiteralPath $executable).Hash -ine $identity.executableSha256) {
                        throw 'Native NGX EXE SHA-256 differs from its run result.'
                    }
                    $archive = Join-Path $caseDirectory 'inputs/fg_native_tests.exe'
                    Assert-TestArtifactPath $archive
                    [void][IO.Directory]::CreateDirectory((Split-Path $archive -Parent))
                    [IO.File]::Move($executable, $archive)
                    $record.executable = @{path=$archive; sha256=$identity.executableSha256}
                    $record.status = 'Passed'
                    [void]$record.Remove('reason')
                } catch {
                    $record.status = 'Failed'
                    $record.reason = $_.Exception.Message
                    throw
                } finally {
                    $env:NR_NEXUS_TEST_RESIZE_PREPARATION_COST = $previousCost
                }
            }
        }
    }
    if ((& git --no-optional-locks -C $repo rev-parse HEAD).Trim() -ne $source) { throw 'Source commit changed during preflight.' }
    $outcome = 'Passed'
} catch {
    $summary['error'] = $_.Exception.Message
    throw
} finally {
    $env:CL = $previousCl
    $summary.status = $outcome
    $summary['finishedUtc'] = [DateTimeOffset]::UtcNow.ToString('o')
    $summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    Complete-TestOutput $OutputDirectory -Outcome $(if ($outcome -eq 'Passed') { 'Passed' } else { 'Failed' })
    if ($Mode -ne 'CandidateAcceptance') {
        try { Invoke-TestWorkspaceMaintenanceIfDue }
        catch { Write-Warning "Historical test maintenance could not run: $($_.Exception.Message)" }
    }
}
