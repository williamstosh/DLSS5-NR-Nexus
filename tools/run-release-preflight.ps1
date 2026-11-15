<#
.SYNOPSIS
Local release checks. Software is the default; native modes require a separately
coordinated device window and never start a game. GitHub CI keeps its own scope.
.DESCRIPTION
Software retains Suite all, then reuses its executables for existing independent
stage, resolution, source-lifecycle and pre-FG cases. ReuseAllOutputDirectory may
reuse a completed all run only when its source and recorded inputs still match.
NativeGpu runs real D3D12 with model stand-ins. NativeNgx runs the existing real
Init/Create/Evaluate FG case; a bounded ngx_probe is not functional evidence.
#>
[CmdletBinding()]
param(
    [ValidateSet('Software','NativeGpu','NativeNgx')][string]$Mode = 'Software',
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/release-preflight/' + [guid]::NewGuid().ToString('N'))),
    [string]$ReuseAllOutputDirectory,
    [switch]$RefreshFg,
    [string]$PythonExecutable,
    [string]$VulkanIncludeDirectory,
    [string]$NativeNgxProjectIdPath,
    [string]$NativeNgxSdkLibraryDirectory,
    [string]$NativeNgxRuntimePath
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
    'native-guide-contract','present-guide-models','native-present-decision','present-chain')
$bridgeCases = @('stage-resolution','same-stage-resolution','render-stage-resolution','present-stage-resolution')
$summary = [ordered]@{
    schema='nr-nexus-local-preflight/1'; mode=$Mode; sourceCommit=$source
    workingTreeFiles=@(& git --no-optional-locks -C $repo diff --name-only)
    startedUtc=[DateTimeOffset]::UtcNow.ToString('o'); status='Running'
    entrySha256=(Get-FileHash $PSCommandPath).Hash; checks=$checks
    limits=@('No game or dynamic visual acceptance.',
        'Software uses NR/FG model stand-ins; native modes are separate.',
        'Bridge and native-present-decision fixtures do not assert all InfoQueue messages.',
        'Adaptive inline retains its existing initial phase; no new phase matrix.')
}
try {
    if ($Mode -eq 'Software') {
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
            # No NativeCase: preserve all five default groups before additions.
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
        foreach ($case in @('test','f5-mixed','control-mask','adaptive-fg-service','adaptive-inline')) {
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
            $arguments.NativeCase = @('control-mask-native-ngx-fg')
            $checks.Add(@{name=$arguments.NativeCase[0];status='NotRun';evidence='NATIVE_NGX';reason='dependencies not yet verified'})
            foreach ($pair in @(@('NativeNgxProjectIdPath',$NativeNgxProjectIdPath),@('NativeNgxSdkLibraryDirectory',$NativeNgxSdkLibraryDirectory),@('NativeNgxRuntimePath',$NativeNgxRuntimePath))) {
                if (-not $pair[1] -or -not (Test-Path -LiteralPath $pair[1])) {
                    $checks[0].reason = "Native NGX dependency missing: $($pair[0])"
                    throw $checks[0].reason
                }
                $arguments[$pair[0]] = $pair[1]
            }
            $evidence = 'NATIVE_NGX'
            $record = @{name=$arguments.NativeCase[0];status='NotRun';evidence=$evidence;reason='not started'}
            $checks[0] = $record
            try {
                & (Join-Path $PSScriptRoot 'run-tests.ps1') @arguments
                $record.status='Passed'; [void]$record.Remove('reason')
            } catch { $record.status='Failed'; $record.reason=$_.Exception.Message; throw }
        }
        $summary['softwareDefaultGroups'] = 'NotRun in this native branch; use the separate Software result.'
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
    Complete-TestOutput $OutputDirectory -Outcome $outcome
    try { Invoke-TestWorkspaceMaintenanceIfDue }
    catch { Write-Warning "Historical test maintenance could not run: $($_.Exception.Message)" }
}
