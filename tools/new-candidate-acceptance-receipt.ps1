<#
.SYNOPSIS
Assemble a candidate receipt from a retained ordinary Release build and native execution evidence.
.DESCRIPTION
NativeRunDirectory and CostBuildDirectory point at fg-native build results.
Each run uses its result.json, artifact lifecycle, case stdout and raw report.
A cost run in a separate working directory uses its original build-ref.json.
An EXE archived under inputs keeps the original compiler output path identity.
FrozenInputsManifest can supply an archived native link input with path,
sha256, frozenPath and frozenSha256. Both hashes must match the native inputs.
HostTrace, when supplied, is a candidate-trace/1 report. A DEV trace is kept as
an observation. With no ordinary host proof the assembled receipt is PendingHost.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$CandidateDll,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$NativeRunDirectory,
    [Parameter(Mandatory)][string]$CostReport,
    [Parameter(Mandatory)][string]$CostBuildDirectory,
    [Parameter(Mandatory)][ValidatePattern('\S')][string]$RunId,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [string]$FrozenInputsManifest,
    [string]$HostTrace
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'check-candidate-acceptance.ps1') -RunId $RunId
. (Join-Path $PSScriptRoot 'check-publishing.ps1') -BuildDirectory $BuildDirectory -OutputDirectory $OutputDirectory
. (Join-Path $PSScriptRoot 'check-release-experiment-parity.ps1') -SourceRoot $repo

function Read-AssemblyJson([string]$Path) {
    Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json -AsHashtable -NoEnumerate
}

function Get-AssemblyFile([string]$Path) {
    $file = Get-Item -LiteralPath $Path
    if ($file.PSIsContainer) { throw "Expected a file: $Path" }
    return @{path=$file.FullName; sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}

function Read-AssemblyBuild([string]$Directory) {
    $Directory = (Get-Item -LiteralPath $Directory).FullName
    $result = Get-AssemblyFile (Join-Path $Directory 'result.json')
    $state = Read-AssemblyJson $result.path
    $identity = $state['buildIdentity']
    if ($state['suite'] -cne 'fg-native' -or $state['build'] -cne 'passed' -or
        $state['run'] -cne 'passed' -or $identity['mode'] -cne 'Off' -or
        $identity['softwareOnly'] -isnot [bool] -or $identity.softwareOnly -or
        $identity['executable'] -cne 'fg_native_tests.exe') {
        throw "Expected a completed ordinary native FG build: $Directory"
    }
    $originalExe = Join-Path $Directory $identity.executable
    $retainedExe = if (Test-Path -LiteralPath $originalExe -PathType Leaf) { $originalExe }
        else { Join-Path $Directory "inputs/$($identity.executable)" }
    $files = @{result=$result; originalExecutablePath=$originalExe}
    foreach ($part in @('arguments','inputs','dependencies','executable')) {
        $path = if ($part -eq 'executable') { $retainedExe } else { Join-Path $Directory "$part.json" }
        $files[$part] = Get-AssemblyFile $path
        if ($files[$part].sha256 -ine $identity["${part}Sha256"]) {
            throw "Native build $part SHA-256 mismatch in $Directory."
        }
    }
    $cmd = Read-AssemblyJson $files.arguments.path
    $exeFlags = @($cmd | Where-Object { $_ -clike '/Fe*' })
    if ($cmd -isnot [array] -or '/DNR_NEXUS_TEST_NATIVE_NGX' -cnotin $cmd -or
        $exeFlags.Count -ne 1 -or [IO.Path]::GetFullPath($exeFlags[0].Substring(3)) -ine $originalExe -or
        (Get-AssemblyFile $identity.compiler).sha256 -ine $identity.compilerSha256 -or
        @($cmd + @($identity.compilerEnvironment.CL, $identity.compilerEnvironment._CL_) |
            Where-Object { $_ -match '(?:^|\s)["]?[/-]D\s*["]?(?:NR_NEXUS_DEV_CONTROL|NR_NEXUS_TEST_WARP_ONLY)\b' }).Count) {
        throw "Native compile arguments do not describe an ordinary NGX build: $Directory"
    }
    $dependencyIndex = [Array]::IndexOf($cmd, '/sourceDependencies')
    if ($dependencyIndex -lt 0 -or $dependencyIndex + 1 -ge $cmd.Count -or
        [IO.Path]::GetFullPath($cmd[$dependencyIndex + 1]) -ine $files.dependencies.path) {
        throw "Native compile arguments do not identify the actual dependency output: $Directory"
    }
    # Match the parity comparison by excluding just the per-build output paths.
    $commonArguments = @($cmd | Where-Object { $_ -cnotmatch '^/F[eo]' -and $_ -cne $cmd[$dependencyIndex + 1] })
    $inputHashes = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    $records = Read-AssemblyJson $files.inputs.path
    if ($records -isnot [array] -or -not $records.Count) { throw "Native build input manifest is empty: $Directory" }
    foreach ($record in $records) {
        if ($record['path'] -isnot [string] -or $record['sha256'] -notmatch '\A[0-9a-f]{64}\z') {
            throw "Invalid native build input record: $Directory"
        }
        $inputHashes.Add([IO.Path]::GetFullPath($record.path, $Directory), $record.sha256)
    }
    $closure = (Read-AssemblyJson $files.dependencies.path).Data
    $closurePaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($file in @($closure.Source) + @($closure.Includes)) {
        $path = [IO.Path]::GetFullPath($file, $Directory)
        if (-not $inputHashes.ContainsKey($path)) { throw "Native input manifest omits compiler dependency: $file" }
        [void]$closurePaths.Add($path)
    }
    return @{files=$files; identity=$identity; directory=$Directory; commonArguments=$commonArguments;
        inputHashes=$inputHashes; closureSource=[IO.Path]::GetFullPath($closure.Source, $Directory); closurePaths=$closurePaths}
}

function Read-AssemblyRun($Report, $Build, [string]$Case, [string]$PassLine, [string]$Condition, $RuntimeInputs) {
    $directory = Split-Path $Report.path -Parent
    $lifecycle = Get-AssemblyFile (Join-Path $directory 'artifact-lifecycle.json')
    $state = Read-AssemblyJson $lifecycle.path
    $stdout = Get-AssemblyFile (Join-Path $directory "$Case.stdout.log")
    if ($state['schema'] -cne 'nr-nexus-test-output/1' -or $state['status'] -cne 'Passed' -or
        @($state['cleanupErrors']).Count -or
        @(Get-Content -LiteralPath $stdout.path | Where-Object { $_ -ceq $PassLine }).Count -ne 1) {
        throw "$Case requires a Passed artifact lifecycle and the exact case PASS in stdout."
    }
    $started = ([DateTimeOffset]$state.startedUtc).UtcDateTime
    $finished = ([DateTimeOffset]$state.finishedUtc).UtcDateTime
    $execution = @{lifecycle=$lifecycle; stdout=$stdout}
    $runFiles = @($Report, $stdout)
    $recordedRuntimes = $null
    if ($directory -ieq $Build.directory) {
        if ($state['sourceCommit'] -cne $Build.identity.source.commit -or
            @($Build.identity['cases']).Count -ne 1 -or $Build.identity.cases[0] -cne $Case) {
            throw 'Artifact lifecycle and result.json must identify this source and exact native case.'
        }
        $runFiles += $Build.files.result
        $execution['result'] = $Build.files.result
        if ($Condition) {
            $runtimeFile = Get-AssemblyFile (Join-Path $directory 'native-ngx-runtime-inputs.json')
            $recordedRuntimes = Read-AssemblyJson $runtimeFile.path
            $execution['runtimeInputs'] = $runtimeFile
            $runFiles += $runtimeFile
        }
    } elseif ($Condition) {
        $buildRef = Get-AssemblyFile (Join-Path $directory 'build-ref.json')
        $reference = Read-AssemblyJson $buildRef.path
        $arguments = $reference['arguments']
        if ($reference['buildDirectory'] -isnot [string] -or
            [IO.Path]::GetFullPath($reference.buildDirectory, $directory) -ine $Build.directory -or
            $reference['executableSha256'] -ine $Build.files.executable.sha256 -or $reference['condition'] -cne $Condition -or
            $arguments -isnot [array] -or $arguments.Count -ne 2 -or $arguments[0] -cne "--$Case" -or
            $arguments[1] -isnot [string] -or -not [IO.Path]::IsPathFullyQualified($arguments[1])) {
            throw "Cost build-ref does not identify the actual $Condition run with the selected native EXE."
        }
        $recordedRuntimes = $reference['runtimeInputs']
        $runFiles += $buildRef
        $execution['buildReference'] = $buildRef
    } else {
        throw 'Native handoff report must come from the recorded test run directory.'
    }
    if ($Condition) {
        foreach ($runtime in $RuntimeInputs) {
            $expectedPath = [IO.Path]::GetFullPath($runtime.path)
            $matchingInputs = @($recordedRuntimes | Where-Object {
                [IO.Path]::GetFullPath($_.path) -ieq $expectedPath -and $_.sha256 -ieq $runtime.sha256
            })
            if ($matchingInputs.Count -ne 1) { throw 'Cost run has different recorded Runtime inputs.' }
        }
    }
    foreach ($runFile in $runFiles) {
        $written = (Get-Item -LiteralPath $runFile.path).LastWriteTimeUtc
        if ($written -lt $started -or $written -gt $finished) {
            throw "$Case artifact is outside the recorded run window: $($runFile.path)"
        }
    }
    return $execution
}

function Get-AssemblyTrackedPaths([string]$Path) {
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ([string]::IsNullOrWhiteSpace($line)) { continue }
        foreach ($file in ($line.TrimStart('^') -split '\|')) {
            if (-not [IO.Path]::IsPathFullyQualified($file)) { throw "Invalid dependency in $Path : $file" }
            [IO.Path]::GetFullPath($file)
        }
    }
}

function Read-AssemblyTrackedGroups([string]$Path) {
    $groups = @{}
    $rule = $null
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line.StartsWith('^')) {
            $rule = $line
            $groups[$rule] = [Collections.Generic.List[string]]::new()
        } elseif ($line) {
            if (-not $rule) { throw "Dependency tracking starts without a rule in $Path." }
            $groups[$rule].Add($line)
        }
    }
    return $groups
}

$CandidateDll = (Get-AssemblyFile $CandidateDll).path
$BuildDirectory = (Get-Item -LiteralPath $BuildDirectory).FullName
$cache = Get-AssemblyFile (Join-Path $BuildDirectory 'CMakeCache.txt')
$cacheText = Get-Content -LiteralPath $cache.path -Raw
if ($cacheText -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\r?$') {
    throw 'Candidate build is missing the Release x64 CMake platform.'
}
$settings = @{}
foreach ($key in @('CMAKE_HOME_DIRECTORY','RENODX_SHADER_SOURCE_ROOT_dlss5','CMAKE_PROJECT_renodx_INCLUDE','FXC_BIN','DXC_BIN','SLANGC_BIN')) {
    $entry = [regex]::Matches($cacheText, "(?m)^${key}:[^=\r\n]+=(.+?)\r?$")
    if ($entry.Count -ne 1 -or -not [IO.Path]::IsPathFullyQualified($entry[0].Groups[1].Value)) {
        throw "Cannot identify actual CMake input: $key"
    }
    $settings[$key] = [IO.Path]::GetFullPath($entry[0].Groups[1].Value)
}
if ($settings.RENODX_SHADER_SOURCE_ROOT_dlss5 -ine (Join-Path $repo 'shaders') -or
    $settings.CMAKE_PROJECT_renodx_INCLUDE -ine ([IO.Path]::GetFullPath((Join-Path $repo 'cmake/nexus-version.cmake')))) {
    throw 'Candidate shader or version-resource source does not belong to this checkout.'
}
$context = Get-ReleaseParityBuildContext $repo $settings.CMAKE_HOME_DIRECTORY
$builtDll = Get-AssemblyFile (Join-Path $BuildDirectory 'Release/renodx-dlss5.addon64')
$isolation = Assert-PublishingIsolation -BuildDirectory $BuildDirectory -BinaryPath $CandidateDll
if ($isolation.DllSha256 -ine $builtDll.sha256) { throw 'Candidate DLL differs from the actual Release build output.' }

$native = Read-AssemblyBuild $NativeRunDirectory
$costBuild = Read-AssemblyBuild $CostBuildDirectory
if (@($native.identity['cases']).Count -ne 1 -or $native.identity.cases[0] -cne 'control-mask-native-ngx-fg') {
    throw 'NativeRunDirectory must name the actual control-mask-native-ngx-fg run.'
}
if ($native.identity.compilerSha256 -ine $costBuild.identity.compilerSha256 -or
    $native.identity.compiler -ine $costBuild.identity.compiler -or
    $native.identity.compilerEnvironment.CL -cne $costBuild.identity.compilerEnvironment.CL -or
    $native.identity.compilerEnvironment._CL_ -cne $costBuild.identity.compilerEnvironment._CL_ -or
    ($native.commonArguments -join "`n") -cne ($costBuild.commonArguments -join "`n") -or
    $native.closureSource -ine $costBuild.closureSource -or -not $native.closurePaths.SetEquals($costBuild.closurePaths)) {
    throw 'Cost and handoff builds must share the compiler, normalized commands and actual dependency closure.'
}
$nativeHashes = $native.inputHashes
if ($nativeHashes.Count -ne $costBuild.inputHashes.Count) { throw 'Cost and handoff build input sets differ.' }
foreach ($path in $nativeHashes.Keys) {
    if (-not $costBuild.inputHashes.ContainsKey($path) -or $costBuild.inputHashes[$path] -ine $nativeHashes[$path]) {
        throw "Cost and handoff build input bytes differ: $path"
    }
}
foreach ($key in @('hostCommit','hostLockSha256','hostPatchSha256','toolsLockSha256')) {
    if ($native.identity.source[$key] -ine $context[$key] -or $costBuild.identity.source[$key] -ine $context[$key]) {
        throw "Native build and candidate checkout disagree on $key."
    }
}
foreach ($key in $context.dependencies.Keys) {
    if ($native.identity.source.dependencies[$key] -cne $context.dependencies[$key] -or
        $costBuild.identity.source.dependencies[$key] -cne $context.dependencies[$key]) {
        throw "Native build and candidate checkout disagree on dependency $key."
    }
}

$frozenInput = $null
$usedFrozenInput = $false
if ($FrozenInputsManifest) {
    $frozenManifest = Get-AssemblyFile $FrozenInputsManifest
    $frozenBase = Split-Path $frozenManifest.path -Parent
    $frozenRecord = Read-AssemblyJson $frozenManifest.path
    if ($frozenRecord['path'] -isnot [string] -or -not [IO.Path]::IsPathFullyQualified($frozenRecord.path) -or
        $frozenRecord['sha256'] -ine $frozenRecord['frozenSha256']) {
        throw 'Frozen input manifest must identify the absolute original path and matching hashes.'
    }
    $frozenPath = Get-CandidateEvidencePath @{path=$frozenRecord['frozenPath']; sha256=$frozenRecord['sha256']} $frozenBase 'Frozen native input'
    $frozenInput = @{originalPath=[IO.Path]::GetFullPath($frozenRecord.path); sha256=$frozenRecord.sha256
        archive=@{path=$frozenPath; sha256=$frozenRecord.sha256}; manifest=$frozenManifest}
}
foreach ($file in $nativeHashes.Keys) {
    $expectedHash = $nativeHashes[$file]
    if (-not (Test-Path -LiteralPath $file -PathType Leaf) -or
        (Get-FileHash -LiteralPath $file).Hash -ine $expectedHash) {
        if ($null -eq $frozenInput -or $frozenInput.originalPath -ine $file -or $frozenInput.sha256 -ine $expectedHash) {
            throw "Native build input SHA-256 mismatch: $file"
        }
        $usedFrozenInput = $true
    }
}
if ($frozenInput -and (-not $nativeHashes.ContainsKey($frozenInput.originalPath) -or
    $nativeHashes[$frozenInput.originalPath] -ine $frozenInput.sha256)) {
    throw 'Frozen input does not match any recorded native build input.'
}
$trackedChanges = @(& git --no-optional-locks -C $repo -c core.quotePath=false diff --name-only HEAD --)
if ($LASTEXITCODE -ne 0) { throw 'Cannot check current tracked inputs.' }
$trackedChanges += @(& git --no-optional-locks -C $repo -c core.quotePath=false ls-files --others --exclude-standard)
if ($LASTEXITCODE -ne 0) { throw 'Cannot check current untracked inputs.' }
foreach ($relative in $trackedChanges) {
    if ($nativeHashes.ContainsKey([IO.Path]::GetFullPath($relative, $repo))) {
        throw "Native input has not been committed to the candidate tree: $relative"
    }
}

$tlogs = Join-Path $BuildDirectory 'dlss5.dir/Release/dlss5.tlog'
$shaderTlogs = Join-Path $BuildDirectory 'x64/Release/dlss5-shaders/dlss5-shaders.tlog'
$linkRead = Join-Path $tlogs 'link.read.1.tlog'
$rcRead = Join-Path $tlogs 'rc.read.1.tlog'
$rcWrite = Get-AssemblyFile (Join-Path $tlogs 'rc.write.1.tlog')
$linkedResources = @(Get-AssemblyTrackedPaths $linkRead | Where-Object { [IO.Path]::GetExtension($_) -ieq '.res' } | Sort-Object -Unique)
$rcReads = Read-AssemblyTrackedGroups $rcRead
$rcWrites = Read-AssemblyTrackedGroups $rcWrite.path
# Incremental MSBuild keeps rules for resource sources removed from the target.
$rcSource = [IO.Path]::GetFullPath((Join-Path $repo 'cmake/nexus-version.rc'))
$rcRule = "^$rcSource"
if ($linkedResources.Count -ne 1 -or -not $rcReads.ContainsKey($rcRule) -or
    $linkedResources[0] -notin $rcWrites[$rcRule]) {
    throw 'The linked Release resource must come from the tracked cmake/nexus-version.rc rule.'
}
$groups = @(
    @{log=(Join-Path $tlogs 'CL.read.1.tlog'); output=(Join-Path $BuildDirectory 'dlss5.dir/Release/addon.obj')},
    @{log=$rcRead; output=$linkedResources[0]; inputs=@($rcSource) + @($rcReads[$rcRule])},
    @{log=$linkRead; output=$builtDll.path},
    @{log=(Join-Path $tlogs 'CustomBuild.read.1.tlog'); output=(Join-Path $BuildDirectory 'CMakeFiles/generate.stamp')},
    @{log=(Join-Path $shaderTlogs 'CustomBuild.read.1.tlog'); output=$null}
)
$actualInputs = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$buildLogs = foreach ($group in $groups) {
    $log = Get-AssemblyFile $group.log
    $paths = if ($group.ContainsKey('inputs')) { $group.inputs } else { @(Get-AssemblyTrackedPaths $log.path) }
    if (-not $paths.Count) { throw "Empty candidate dependency log: $($log.path)" }
    $latest = if ($group.output) { (Get-Item -LiteralPath $group.output).LastWriteTimeUtc }
        else { (Get-Item -LiteralPath $log.path).LastWriteTimeUtc }
    foreach ($path in $paths) {
        if ((Get-Item -LiteralPath $path).LastWriteTimeUtc -gt $latest) {
            throw "Build input is newer than its recorded build step: $path"
        }
        [void]$actualInputs.Add($path)
    }
    $log
}
$shaderWriteLog = Get-AssemblyFile (Join-Path $shaderTlogs 'CustomBuild.write.1.tlog')
$shaderReads = Read-AssemblyTrackedGroups $buildLogs[-1].path
$shaderWrites = Read-AssemblyTrackedGroups $shaderWriteLog.path
foreach ($rule in $shaderReads.Keys) {
    $shaderOutputs = @($shaderWrites[$rule] | Where-Object { $_ -match '[\\/]dlss5\.include[\\/]embed[\\/]' })
    foreach ($output in $shaderOutputs) {
        $outputTime = (Get-Item -LiteralPath $output).LastWriteTimeUtc
        foreach ($inputFile in $shaderReads[$rule]) {
            if ((Get-Item -LiteralPath $inputFile).LastWriteTimeUtc -gt $outputTime) {
                throw "Shader input is newer than its tracked output: $inputFile"
            }
        }
    }
}
$buildLogs += @($shaderWriteLog, $rcWrite)
foreach ($key in @('CMAKE_PROJECT_renodx_INCLUDE','FXC_BIN','DXC_BIN','SLANGC_BIN')) {
    [void]$actualInputs.Add($settings[$key])
}
foreach ($path in @('host/lock.json','host/tools.lock.json','host/renodx-compat.patch')) {
    [void]$actualInputs.Add([IO.Path]::GetFullPath($path, $repo))
}
foreach ($relative in $trackedChanges) {
    if ($actualInputs.Contains([IO.Path]::GetFullPath($relative, $repo))) {
        throw "Candidate build input has not been committed to the candidate tree: $relative"
    }
}
$embed = [IO.Path]::GetFullPath((Join-Path $BuildDirectory 'dlss5.include/embed/shaders.h'))
$embeddedHash = (Get-AssemblyFile $embed).sha256
$nativeEmbed = @($nativeHashes.Keys | Where-Object { $_ -match '[\\/]dlss5\.include[\\/]embed[\\/]shaders\.h$' })
if (-not $actualInputs.Contains($embed) -or $nativeEmbed.Count -ne 1 -or $nativeHashes[$nativeEmbed[0]] -ine $embeddedHash) {
    throw 'Candidate and native EXE do not share the actual embedded Shader inputs.'
}
$buildInputs = @($actualInputs | Sort-Object | ForEach-Object { Get-AssemblyFile $_ })

$nativeReport = Get-AssemblyFile (Join-Path $NativeRunDirectory 'xefg-inline-handoff-native.json')
$handoff = Read-AssemblyJson $nativeReport.path
$stages = @('Present','Render','Present','Upscaled','Present')
if ($handoff['schema'] -cne 'nr-nexus-xefg-inline-handoff/1' -or $handoff['status'] -cne 'Passed' -or
    $handoff['evidence'] -cne 'NATIVE_GPU+NATIVE_NGX+MOCK_XeFG' -or $handoff['case'] -cne 'xefg-held-inline-handoff' -or
    $handoff['sr_evidence'] -cne 'NATIVE_NGX' -or $handoff['phases'] -isnot [array] -or $handoff.phases.Count -ne $stages.Count -or
    $handoff.sdk_fg.evidence -cne 'MOCK_XeFG' -or $handoff.sdk_fg.on_samples -isnot [long] -or
    $handoff.sdk_fg.off_samples -isnot [long] -or $handoff.sdk_fg.off_samples -ne 0) {
    throw 'Native FG report lacks the expected real NGX five-stage handoff and FG observations.'
}
$previousSequence = 0L
$samples = 0
$phases = for ($i = 0; $i -lt $stages.Count; $i++) {
    $phase = $handoff.phases[$i]
    if ($phase['stage'] -cne $stages[$i] -or $phase['frames'] -isnot [array] -or -not $phase.frames.Count) {
        throw "Native FG report is missing the expected $($stages[$i]) phase or frames."
    }
    foreach ($frame in $phase.frames) {
        foreach ($flag in @('sdk_fg_enabled','gpu_fence_healthy','delivery_complete','readback_complete')) {
            if ($frame[$flag] -isnot [bool] -or -not $frame[$flag]) { throw "Native FG frame lacks $flag." }
        }
        foreach ($field in @('accepted_evaluate_sequence','delivery_sequence','delivery_fence_value','readback_fence_value','readback_fence_completed','readback_pixels')) {
            if ($frame[$field] -isnot [long] -or $frame[$field] -le 0) { throw "Native FG frame lacks a valid $field." }
        }
        if ($frame.accepted_evaluate_sequence -le $previousSequence -or
            $frame.readback_fence_completed -lt $frame.readback_fence_value) {
            throw 'Native FG frame has an old Evaluate or uncompleted readback.'
        }
        $previousSequence = $frame.accepted_evaluate_sequence
        $samples++
    }
    # The verifier uses a shared frame identity. Raw Evaluate, delivery and fence counters use distinct sequences.
    @{stage=$phase.stage; evaluateCalls=$phase.frames.Count; evaluateSequence=$previousSequence;
        deliveredSequence=$previousSequence; readbackSequence=$previousSequence; rawFrame=$phase.frames[-1]}
}
if ($handoff.sdk_fg.on_samples -lt $samples) { throw 'Native FG sample count does not cover the accepted phase frames.' }

$costSource = Get-AssemblyFile $CostReport
$cost = Read-AssemblyJson $costSource.path
if ($cost['nr_sha256'] -ine $handoff['runtime_sha256'] -or $cost['host_sr'] -cne 'NATIVE_NGX') {
    throw 'Cost run and native handoff do not share the observed NR Runtime or real SR evidence.'
}
$runtimeManifest = Get-AssemblyFile (Join-Path $NativeRunDirectory 'native-ngx-runtime-inputs.json')
$runtimeInputs = Read-AssemblyJson $runtimeManifest.path
foreach ($runtimeName in @('nvngx_dlssnr.dll','nvngx_dlss.dll')) {
    $expectedRuntime = @($runtimeInputs | Where-Object { [IO.Path]::GetFileName($_.path) -ieq $runtimeName })
    $observedRuntime = @($cost['loaded_ngx_modules'] | Where-Object name -IEQ $runtimeName)
    if ($expectedRuntime.Count -ne 1 -or $observedRuntime.Count -ne 1 -or
        $expectedRuntime[0].sha256 -ine $observedRuntime[0].sha256 -or
        ($runtimeName -eq 'nvngx_dlssnr.dll' -and $observedRuntime[0].sha256 -ine $handoff.runtime_sha256)) {
        throw "Cost run does not use the native run's recorded $runtimeName."
    }
}
$nativePass = 'PASS xefg-held-inline-handoff evidence=NATIVE_GPU+NATIVE_NGX+MOCK_XeFG stages=Present,Render,Present,Upscaled,Present'
$condition = $cost['condition']
if ($condition -cnotin @('full','on-demand')) { throw 'Unknown adaptive preparation cost condition.' }
$costPass = "PASS adaptive-preparation-cost evidence=NATIVE_GPU+NATIVE_NGX condition=$condition levels=3 passes=1 steady_per_tier=24 readbacks=2"
$nativeExecution = Read-AssemblyRun $nativeReport $native 'control-mask-native-ngx-fg' $nativePass
$costExecution = Read-AssemblyRun $costSource $costBuild 'control-mask-native-ngx-resize' $costPass $condition $runtimeInputs

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory, (Get-Location).Path)
if ((Test-Path -LiteralPath $OutputDirectory) -and (Get-ChildItem -LiteralPath $OutputDirectory -Force | Select-Object -First 1)) {
    throw "Refusing to overwrite a receipt directory: $OutputDirectory"
}
[void][IO.Directory]::CreateDirectory($OutputDirectory)
$manifestPath = Join-Path $OutputDirectory 'build-inputs.json'
$nativePath = Join-Path $OutputDirectory 'offline-trace.json'
$costPath = Join-Path $OutputDirectory 'cost-report.json'
$receiptPath = Join-Path $OutputDirectory 'candidate-acceptance.json'
$pendingPath = Join-Path $OutputDirectory 'candidate-acceptance.unverified.json'
ConvertTo-Json -InputObject $buildInputs -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding utf8
$manifest = Get-AssemblyFile $manifestPath
$binding = @{runId=$RunId; sourceTree=$context.tree; candidateDllSha256=$builtDll.sha256; inputsSha256=$manifest.sha256}
$offline = @{
    schema='nr-nexus-candidate-trace/1'; status=$handoff.status; kind='NATIVE_NGX'; case=$handoff.case
    fgProvider='MockXeFG'; fgOnSamples=$handoff.sdk_fg.on_samples; fgOffSamples=$handoff.sdk_fg.off_samples
    phases=@($phases); executable=$native.files.executable; rawReport=$nativeReport; execution=$nativeExecution
    nativeBuild=$native.files; runtimeInputs=$runtimeManifest
} + $binding
if ($usedFrozenInput) { $offline['frozenNativeInput'] = $frozenInput }
foreach ($key in $binding.Keys) {
    if ($cost.ContainsKey($key) -and $cost[$key] -cne $binding[$key]) { throw "Cost source already has a different $key." }
    $cost[$key] = $binding[$key]
}
$cost['rawReport'] = $costSource
$cost['execution'] = $costExecution
$cost['nativeBuild'] = $costBuild.files
$offline | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $nativePath -Encoding utf8
$cost | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $costPath -Encoding utf8
$receipt = [ordered]@{
    schema='nr-nexus-candidate-acceptance/1'; runId=$RunId; sourceCommit=$context.commit; sourceTree=$context.tree
    dllSha256=$builtDll.sha256
    build=@{status='Passed'; directory=$BuildDirectory; cmakeCacheSha256=$cache.sha256
        compilerCommandSha256=$isolation.CompilerCommandSha256; compilerReadSha256=$buildLogs[0].sha256
        inputs=$manifest; dependencyLogs=$buildLogs}
    offline=(Get-AssemblyFile $nativePath); costs=(Get-AssemblyFile $costPath)
}
if ($HostTrace) {
    $reference = Get-AssemblyFile $HostTrace
    $hostReport = Read-AssemblyJson $reference.path
    if ($hostReport['kind'] -cne 'IN_GAME' -or $hostReport['buildFlavor'] -cnotin @('Ordinary','DEV')) {
        throw 'HostTrace must identify its IN_GAME Ordinary or DEV build flavor.'
    }
    if ($hostReport.buildFlavor -ceq 'DEV') { $receipt['devHostObservation'] = $reference }
    else { $receipt['host'] = $reference }
}
$receipt | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $pendingPath -Encoding utf8
$validation = Test-CandidateAcceptance -ReceiptPath $pendingPath -DllPath $CandidateDll -RunId $RunId `
    -OfflineCase 'xefg-held-inline-handoff' -OfflineStages $stages -HostCase 'xefg-held-inline-handoff' -HostStages $stages
if ($validation.status -cnotin @('Passed', 'PendingHost')) {
    throw "Assembled receipt failed verification: $($validation.reason)"
}
Move-Item -LiteralPath $pendingPath -Destination $receiptPath
$validation['receiptPath'] = $receiptPath
$validation | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'assembly-result.json') -Encoding utf8
$validation
