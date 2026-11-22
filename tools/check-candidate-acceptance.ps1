<#
.SYNOPSIS
Check one explicitly selected ordinary Release candidate and acceptance attempt.
.DESCRIPTION
The nr-nexus-candidate-acceptance/1 receipt contains runId, sourceCommit,
sourceTree, dllSha256, build, offline, costs, and optionally host. offline, costs,
and host are evidence references {path, sha256}. Relative paths resolve against
the JSON file containing them. Use one explicit RunId for the current attempt.

build has status, directory (with Release DLL, dlss5 tlogs and CMakeCache),
cmakeCacheSha256, compilerCommandSha256, compilerReadSha256, and inputs
(an evidence reference). The inputs JSON is an array of {path, sha256} covering
the compiled addon.cpp, its CL.read.1.tlog dependencies and host/lock.json.

The harness or its aggregator writes nr-nexus-candidate-trace/1 JSON with
status, kind (NATIVE_NGX or IN_GAME), runId, sourceTree, candidateDllSha256,
inputsSha256, case, fgProvider, fgOnSamples, fgOffSamples, and ordered phases.
FG samples cover the case including transitions. Each phase records stage,
evaluateCalls (positive accepted real NR calls), evaluateSequence (one of those
calls), and deliveredSequence (completed output from that same call).
NATIVE_NGX includes executable {path, sha256} and per-phase readbackSequence
(verified GPU readback of that output). MockXeFG is permitted in this trace.
candidateDllSha256 binds that native EXE run to the intended candidate; only the
host's runner can establish that the DLL was actually loaded.
IN_GAME uses XeFG or NativeDLSSFG, buildFlavor Ordinary, pid, processCreated,
and loadedModule {path, sha256} for the protected runner's loaded-modules.json.

costs references the actual nr-nexus-adaptive-preparation-cost/1 harness JSON
with the same runId, sourceTree, candidateDllSha256 and inputsSha256 binding.
The checker reads the first completed NR source, actual preparation samples,
and upper-steady GPU timestamps from that report. It runs no workloads.
#>
[CmdletBinding()]
param(
    [string]$ReceiptPath,
    [string]$DllPath,
    [string]$RunId,
    [string]$OfflineCase,
    [string[]]$OfflineStages,
    [string]$HostCase,
    [string[]]$HostStages
)

function Get-CandidateEvidencePath {
    param($Reference, [string]$Base, [string]$Label)
    if ($Reference -isnot [Collections.IDictionary] -or
        $Reference['path'] -isnot [string] -or [string]::IsNullOrWhiteSpace($Reference.path) -or
        $Reference['sha256'] -isnot [string] -or $Reference.sha256 -notmatch '\A[0-9a-f]{64}\z') {
        throw "$Label requires a path and SHA-256."
    }
    $path = [IO.Path]::GetFullPath($Reference.path, $Base)
    if ((Get-FileHash -LiteralPath $path -ErrorAction Stop).Hash -ine $Reference.sha256) {
        throw "$Label SHA-256 mismatch: $path"
    }
    return $path
}

function Read-CandidateEvidenceJson {
    param($Reference, [string]$Base, [string]$Label)
    $path = Get-CandidateEvidencePath $Reference $Base $Label
    $data = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json -AsHashtable -NoEnumerate
    if ($data -isnot [Collections.IDictionary]) { throw "$Label must be a JSON object." }
    return @{data=$data; base=(Split-Path $path -Parent); path=$path}
}

function Assert-CandidateBinding {
    param($Data, $Binding, [string]$Label)
    foreach ($key in $Binding.Keys) {
        $bindingMatches = if ($key -eq 'runId') { $Data[$key] -ceq $Binding[$key] } else { $Data[$key] -ieq $Binding[$key] }
        if ($Data[$key] -isnot [string] -or -not $bindingMatches) {
            throw "$Label binding mismatch: $key."
        }
    }
}

function Assert-CandidateTrace {
    param($Trace, [string]$Kind, [string]$Case, [string[]]$Stages, $Binding, [string]$Base)
    if ($Trace['status'] -cne 'Passed') {
        throw "$Kind trace requires a Passed terminal record; got '$($Trace['status'])'."
    }
    if ($Trace['schema'] -cne 'nr-nexus-candidate-trace/1' -or $Trace['kind'] -cne $Kind -or $Trace['case'] -cne $Case) {
        throw "$Kind trace has the wrong schema, evidence kind or case; expected '$Case'."
    }
    Assert-CandidateBinding $Trace $Binding "$Kind trace"
    $providers = @('XeFG', 'NativeDLSSFG')
    if ($Kind -eq 'NATIVE_NGX') {
        $providers += 'MockXeFG'
        $null = Get-CandidateEvidencePath $Trace['executable'] $Base 'Native NGX executable'
    } elseif ($Trace['buildFlavor'] -cne 'Ordinary') {
        throw 'IN_GAME trace requires the Ordinary host build.'
    }
    if ($Trace['fgProvider'] -cnotin $providers -or $Trace['fgOnSamples'] -isnot [long] -or
        $Trace.fgOnSamples -lt $Stages.Count -or $Trace['fgOffSamples'] -isnot [long] -or $Trace.fgOffSamples -ne 0) {
        throw "$Kind trace does not establish that FG stayed on."
    }
    if ($Trace['phases'] -isnot [array] -or $Trace.phases.Count -ne $Stages.Count) {
        throw "$Kind trace has the wrong phase count."
    }
    $previousSequence = 0L
    for ($index = 0; $index -lt $Stages.Count; $index++) {
        $phase = $Trace.phases[$index]
        if ($phase -isnot [Collections.IDictionary] -or $phase['stage'] -cne $Stages[$index]) {
            throw "$Kind trace phase $($index + 1) must be $($Stages[$index])."
        }
        if ($phase['evaluateCalls'] -isnot [long] -or $phase.evaluateCalls -le 0 -or
            $phase['evaluateSequence'] -isnot [long] -or $phase.evaluateSequence -le $previousSequence -or
            $phase['deliveredSequence'] -isnot [long] -or $phase.deliveredSequence -ne $phase.evaluateSequence) {
            throw "$Kind phase $($phase.stage) requires a new accepted Evaluate and its completed delivery."
        }
        if ($Kind -eq 'NATIVE_NGX' -and ($phase['readbackSequence'] -isnot [long] -or
            $phase.readbackSequence -ne $phase.evaluateSequence)) {
            throw "$Kind phase $($phase.stage) requires readback of the delivered output."
        }
        $previousSequence = $phase.evaluateSequence
    }
}

function Get-CandidateCostSummary {
    param($Cost, $Binding)
    $data = $Cost.data
    if ($data['schema'] -cne 'nr-nexus-adaptive-preparation-cost/1' -or $data['status'] -cne 'Passed' -or
        $data['evidence'] -cne 'NATIVE_GPU+NATIVE_NGX' -or $data['stage'] -cne 'Upscaled' -or
        $data['condition'] -cnotin @('full', 'on-demand') -or $data['frames'] -isnot [array] -or
        $data['milestones'] -isnot [Collections.IDictionary]) {
        throw 'Costs require a Passed real NGX adaptive preparation cost report.'
    }
    Assert-CandidateBinding $data $Binding 'Cost report'
    $frames = $data.frames
    $first = $data.milestones['first_completed_active_nr']
    if ($first -isnot [long] -or $first -lt 0 -or $first -ge $frames.Count -or
        $frames[$first].source -ne $first -or $frames[$first].active_evaluate_count -ne 1 -or
        $frames[$first].source_successes -ne 1 -or $frames[$first].fence -le 0 -or
        $frames[$first].completed_fence -lt $frames[$first].fence) {
        throw 'Invalid first completed NR cost source.'
    }
    $steady = @($frames | Where-Object phase -CEQ 'upper-steady')
    if (@($steady | Where-Object { $_.active_evaluate_count -ne 1 -or $_.source_successes -ne 1 -or
        $_.fence -le 0 -or $_.completed_fence -lt $_.fence }).Count) {
        throw 'Invalid completed NR samples in upper-steady cost source.'
    }
    $definitions = @(
        @{name='firstFrame'; metric='first completed active NR source hook CPU'; samples=@($frames[$first].hook_cpu_ms)},
        @{name='tierPreparation'; metric='sum of positive per-source preparation CPU'; samples=@(
            $frames | ForEach-Object preparation_cpu_ms | Where-Object { $_ -gt 0 })},
        @{name='steadyState'; metric='mean upper-steady hook GPU'; samples=@($steady | ForEach-Object { $_.hook_gpu.ms })}
    )
    $costs = [ordered]@{source=$Cost.path; case='adaptive-preparation-cost'; condition=$data.condition}
    foreach ($definition in $definitions) {
        if (-not $definition.samples.Count -or @($definition.samples | Where-Object {
            ($_ -isnot [long] -and $_ -isnot [double]) -or -not [double]::IsFinite($_) -or $_ -le 0
        }).Count) { throw "Missing or invalid measured cost: $($definition.name)." }
        $measured = $definition.samples | Measure-Object -Sum -Average
        $value = if ($definition.name -eq 'tierPreparation') { $measured.Sum } else { $measured.Average }
        $costs[$definition.name] = @{metric=$definition.metric; sampleCount=$measured.Count; valueMs=$value}
    }
    return $costs
}

function Test-CandidateAcceptance {
    param(
        [string]$ReceiptPath, [string]$DllPath, [string]$RunId,
        [string]$OfflineCase, [string[]]$OfflineStages,
        [string]$HostCase, [string[]]$HostStages
    )
    $ErrorActionPreference = 'Stop'
    $repo = Split-Path $PSScriptRoot -Parent
    $result = [ordered]@{ status='Failed'; candidateComplete=$false; runId=$RunId }
    try {
        if (-not $ReceiptPath -or -not $DllPath -or [string]::IsNullOrWhiteSpace($RunId) -or
            [string]::IsNullOrWhiteSpace($OfflineCase) -or [string]::IsNullOrWhiteSpace($HostCase) -or
            -not $OfflineStages -or -not $HostStages -or
            @($OfflineStages + $HostStages | Where-Object { $_ -cnotin @('Render', 'Upscaled', 'Present') }).Count) {
            throw 'Candidate acceptance requires ReceiptPath, DllPath, RunId, exact offline/host cases and ordered stages.'
        }
        $receiptPath = (Get-Item -LiteralPath $ReceiptPath).FullName
        $base = Split-Path $receiptPath -Parent
        $receipt = Get-Content -LiteralPath $receiptPath -Raw | ConvertFrom-Json -AsHashtable -NoEnumerate
        if ($receipt -isnot [Collections.IDictionary] -or $receipt.schema -cne 'nr-nexus-candidate-acceptance/1' -or
            $receipt.runId -cne $RunId) {
            throw 'Candidate receipt has the wrong schema or runId.'
        }
        $sourceTree = & git --no-optional-locks -C $repo rev-parse --verify 'HEAD^{tree}'
        if ($LASTEXITCODE -ne 0 -or $receipt.sourceTree -cne $sourceTree -or
            $receipt.sourceCommit -cnotmatch '\A(?:[0-9a-f]{40}|[0-9a-f]{64})\z') {
            throw 'Candidate receipt does not match the current source tree.'
        }
        $recordedTree = & git --no-optional-locks -C $repo rev-parse --verify --quiet "$($receipt.sourceCommit)^{commit}^{tree}"
        if ($LASTEXITCODE -ne 0 -or $recordedTree -cne $sourceTree) {
            throw 'Candidate sourceCommit does not identify the recorded source tree.'
        }
        $dll = (Get-Item -LiteralPath $DllPath).FullName
        $dllSha256 = (Get-FileHash -LiteralPath $dll).Hash
        if ($dllSha256 -ine $receipt.dllSha256) { throw 'Candidate DLL SHA-256 mismatch.' }
        $result.sourceTree = $sourceTree
        $result.dllSha256 = $dllSha256

        $build = $receipt.build
        if ($build -isnot [Collections.IDictionary] -or $build.status -cne 'Passed' -or
            $build.directory -isnot [string] -or [string]::IsNullOrWhiteSpace($build.directory)) {
            throw 'Candidate requires a Passed build receipt with its recorded build directory.'
        }
        $buildDirectory = [IO.Path]::GetFullPath($build.directory, $base)
        $null = Get-CandidateEvidencePath @{path='Release/renodx-dlss5.addon64'; sha256=$dllSha256} $buildDirectory 'Build output DLL'
        $cache = Get-CandidateEvidencePath @{path='CMakeCache.txt'; sha256=$build.cmakeCacheSha256} $buildDirectory 'Build CMake cache'
        $cacheLines = Get-Content -LiteralPath $cache
        $platform = @($cacheLines | Where-Object { $_ -clike 'CMAKE_GENERATOR_PLATFORM:*' })
        if ($platform.Count -ne 1 -or $platform[0] -cne 'CMAKE_GENERATOR_PLATFORM:INTERNAL=x64') {
            throw 'Candidate build requires the actual x64 CMake platform.'
        }
        . (Join-Path $PSScriptRoot 'check-publishing.ps1') -BuildDirectory $buildDirectory
        $isolation = Assert-PublishingIsolation -BuildDirectory $buildDirectory -BinaryPath $dll
        if ($isolation.CompilerCommandSha256 -ine $build.compilerCommandSha256) {
            throw 'Candidate Release compiler command SHA-256 mismatch.'
        }
        if ($isolation.DllSha256 -ine $dllSha256) { throw 'Candidate DLL changed during verification.' }
        $commandPath = Join-Path $buildDirectory 'dlss5.dir/Release/dlss5.tlog/CL.command.1.tlog'
        $command = Get-Content -LiteralPath $commandPath -Encoding Unicode -Raw
        $entries = [regex]::Matches($command, '(?im)^\^([^\r\n]+[\\/]addon\.cpp)[ \t]*\r?$')
        if ($entries.Count -ne 1) { throw 'Cannot identify the actual compiled addon.cpp.' }
        $compiledAddon = $entries[0].Groups[1].Value
        if (-not [IO.Path]::IsPathFullyQualified($compiledAddon)) { throw 'Compiled addon.cpp path must be absolute.' }
        $readTlog = Get-CandidateEvidencePath @{
            path='dlss5.dir/Release/dlss5.tlog/CL.read.1.tlog'; sha256=$build['compilerReadSha256']
        } $buildDirectory 'Build compiler dependency log'
        $dependencies = @(Get-Content -LiteralPath $readTlog -Encoding Unicode | Where-Object { $_ })
        if ($dependencies.Count -lt 2 -or $dependencies[0] -ine "^$compiledAddon") {
            throw 'Compiler dependency log must contain the actual addon.cpp dependency group.'
        }
        $inputsPath = Get-CandidateEvidencePath $build.inputs $base 'Build input manifest'
        $inputs = Get-Content -LiteralPath $inputsPath -Raw | ConvertFrom-Json -AsHashtable -NoEnumerate
        if ($inputs -isnot [array] -or $inputs.Count -eq 0) { throw 'Build input manifest must contain actual input records.' }
        $inputBase = Split-Path $inputsPath -Parent
        $verifiedInputs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($inputRecord in $inputs) {
            $path = Get-CandidateEvidencePath $inputRecord $inputBase 'Build input'
            if (-not $verifiedInputs.Add($path)) { throw "Duplicate build input: $path" }
        }
        foreach ($dependency in ($dependencies | Select-Object -Skip 1)) {
            if (-not [IO.Path]::IsPathFullyQualified($dependency) -or
                -not $verifiedInputs.Contains([IO.Path]::GetFullPath($dependency))) {
                throw "Build input manifest is missing a compiler dependency: $dependency"
            }
        }
        if (-not $verifiedInputs.Contains([IO.Path]::GetFullPath($compiledAddon)) -or
            -not $verifiedInputs.Contains([IO.Path]::GetFullPath((Join-Path $repo 'host/lock.json'))) -or
            (Get-FileHash -LiteralPath $compiledAddon).Hash -ine (Get-FileHash -LiteralPath (Join-Path $repo 'addon.cpp')).Hash) {
            throw 'Build manifest must include the compiled addon.cpp matching this checkout and its host/lock.json.'
        }
        $changed = @(& git --no-optional-locks -C $repo -c core.quotePath=false diff --name-only HEAD --)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot check working-tree changes to recorded build inputs.' }
        $compiledRoot = Split-Path $compiledAddon -Parent
        foreach ($relative in $changed) {
            if ($verifiedInputs.Contains([IO.Path]::GetFullPath($relative, $repo)) -or
                $verifiedInputs.Contains([IO.Path]::GetFullPath($relative, $compiledRoot))) {
                throw "Recorded build input differs from the committed source tree: $relative"
            }
        }
        $projectFiles = @(& git --no-optional-locks -C $repo -c core.quotePath=false ls-tree -r --name-only $sourceTree)
        if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate the recorded source tree.' }
        foreach ($relative in $projectFiles) {
            $projected = [IO.Path]::GetFullPath($relative, $compiledRoot)
            if ($verifiedInputs.Contains($projected) -and
                (Get-FileHash -LiteralPath $projected).Hash -ine
                    (Get-FileHash -LiteralPath ([IO.Path]::GetFullPath($relative, $repo))).Hash) {
                throw "Compiled source projection does not match the committed source tree: $relative"
            }
        }
        $result.verifiedInputs = $verifiedInputs.Count

        $binding = @{runId=$RunId; sourceTree=$sourceTree; candidateDllSha256=$receipt.dllSha256; inputsSha256=$build.inputs.sha256}
        $offline = Read-CandidateEvidenceJson $receipt['offline'] $base 'NATIVE_NGX trace source'
        Assert-CandidateTrace $offline.data 'NATIVE_NGX' $OfflineCase $OfflineStages $binding $offline.base
        $result.offlineCase = $OfflineCase
        $result.offlineStages = $OfflineStages
        $result.offlineFgProvider = $offline.data.fgProvider
        $costs = Read-CandidateEvidenceJson $receipt['costs'] $base 'Offline cost source'
        $result.costs = Get-CandidateCostSummary $costs $binding

        if ($null -eq $receipt['host']) {
            $result.status = 'PendingHost'
            $result.reason = 'The selected in-game trace and actual module load have not been supplied.'
            return $result
        }
        $hostEvidence = Read-CandidateEvidenceJson $receipt.host $base 'IN_GAME trace source'
        $hostTrace = $hostEvidence.data
        if ($hostTrace['status'] -cin @('NotRun', 'PendingHost')) {
            Assert-CandidateBinding $hostTrace $binding 'IN_GAME trace'
            $result.status = 'PendingHost'
            $result.reason = 'The report has no completed in-game trace.'
            return $result
        }
        Assert-CandidateTrace $hostTrace 'IN_GAME' $HostCase $HostStages $binding $hostEvidence.base
        $load = $hostTrace['loadedModule']
        if ($null -eq $load -or ($load -is [Collections.IDictionary] -and $load['path'] -is [string] -and
            $load.path -and -not (Test-Path -LiteralPath ([IO.Path]::GetFullPath($load.path, $hostEvidence.base)) -PathType Leaf))) {
            $result.status = 'PendingHost'
            $result.reason = 'Actual module-load evidence has not been supplied.'
            return $result
        }
        $module = (Read-CandidateEvidenceJson $load $hostEvidence.base 'Host module-load evidence').data
        if ($module -isnot [Collections.IDictionary] -or $hostTrace.pid -isnot [long] -or $hostTrace.pid -le 0 -or
            $hostTrace.processCreated -isnot [long] -or $hostTrace.processCreated -le 0 -or
            $module.process.pid -ne $hostTrace.pid -or $module.process.created -ne $hostTrace.processCreated -or
            $module.expected_path -isnot [string] -or -not [IO.Path]::IsPathFullyQualified($module.expected_path) -or
            [IO.Path]::GetFileName($module.expected_path) -ine 'renodx-dlss5.addon64' -or
            $module.matching_paths -isnot [array] -or $module.matching_paths.Count -ne 1 -or
            $module.matching_paths[0] -ine $module.expected_path -or $module.expected_path -notin $module.modules -or
            $module.actual_sha256 -ine $dllSha256 -or $module.expected_sha256 -ine $dllSha256) {
            throw 'Actual loaded module path, process identity or DLL SHA-256 mismatch.'
        }
        $currentTree = & git --no-optional-locks -C $repo rev-parse --verify 'HEAD^{tree}'
        if ($LASTEXITCODE -ne 0 -or $currentTree -cne $sourceTree) { throw 'Source tree changed during candidate verification.' }
        $result.hostCase = $HostCase
        $result.hostStages = $HostStages
        $result.hostFgProvider = $hostTrace.fgProvider
        $result.status = 'Passed'
        $result.candidateComplete = $true
    } catch {
        $result.reason = $_.Exception.Message
    }
    return $result
}

if ($MyInvocation.InvocationName -eq '.') { return }
$result = Test-CandidateAcceptance @PSBoundParameters
$result | ConvertTo-Json -Depth 5
if (-not $result.candidateComplete) { exit 1 }
