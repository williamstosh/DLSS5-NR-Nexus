$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repo = Split-Path $PSScriptRoot -Parent
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path $repo 'tools/check-candidate-acceptance.ps1')

function Write-CandidateFixtureJson {
    param([string]$Path, $Value)
    ConvertTo-Json -InputObject $Value -Depth 12 | Set-Content -LiteralPath $Path -Encoding utf8
}

function New-CandidateFixtureReference {
    param([string]$Path)
    return @{path=$Path; sha256=(Get-FileHash -LiteralPath $Path).Hash}
}

function New-CandidateFixtureBuild {
    param([string]$Directory, [string]$Dll, [string]$Addon, [string]$Header, $Inputs,
        [string]$Dev = 'OFF', [string]$ExtraDefine = '')
    $binary = Join-Path $Directory 'Release/renodx-dlss5.addon64'
    $command = Join-Path $Directory 'dlss5.dir/Release/dlss5.tlog/CL.command.1.tlog'
    $readLog = Join-Path (Split-Path $command -Parent) 'CL.read.1.tlog'
    $cache = Join-Path $Directory 'CMakeCache.txt'
    [void][IO.Directory]::CreateDirectory((Split-Path $binary -Parent))
    [void][IO.Directory]::CreateDirectory((Split-Path $command -Parent))
    Copy-Item -LiteralPath $Dll -Destination $binary
    @('CMAKE_GENERATOR_PLATFORM:INTERNAL=x64', "NR_NEXUS_DEV_CONTROL:BOOL=$Dev", 'NR_NEXUS_IME_DIAGNOSTICS:BOOL=OFF') |
        Set-Content -LiteralPath $cache -Encoding utf8
    @("^$Addon", "/c /O2 $ExtraDefine") | Set-Content -LiteralPath $command -Encoding Unicode
    @("^$Addon", $Header) | Set-Content -LiteralPath $readLog -Encoding Unicode
    return @{
        status='Passed'; directory=$Directory; inputs=$Inputs
        cmakeCacheSha256=(Get-FileHash -LiteralPath $cache).Hash
        compilerCommandSha256=(Get-FileHash -LiteralPath $command).Hash
        compilerReadSha256=(Get-FileHash -LiteralPath $readLog).Hash
    }
}

function Write-CandidateFixtureAttempt {
    param($Attempt, [string]$ReceiptPath)
    foreach ($section in @('offline','host','costs')) {
        $path = Join-Path (Split-Path $ReceiptPath -Parent) "$section.json"
        Write-CandidateFixtureJson $path $Attempt[$section]
        if ($Attempt.receipt.ContainsKey($section)) {
            $Attempt.receipt[$section] = New-CandidateFixtureReference $path
        }
    }
    Write-CandidateFixtureJson $ReceiptPath $Attempt.receipt
}

$output = Initialize-TestOutput (Join-Path $repo ('tmp/candidate-acceptance-tests/' + [guid]::NewGuid().ToString('N')))
$fixture = Join-Path $output 'fixture'
$outcome = 'Failed'
try {
    [void][IO.Directory]::CreateDirectory($fixture)
    Register-TestDisposable $output $fixture
    $commit = & git --no-optional-locks -C $repo rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify test source commit.' }
    $tree = & git --no-optional-locks -C $repo rev-parse 'HEAD^{tree}'
    if ($LASTEXITCODE -ne 0) { throw 'Cannot identify test source tree.' }
    $dll = Join-Path $fixture 'candidate.addon64'
    $wrongDll = Join-Path $fixture 'other.addon64'
    $addon = Join-Path $fixture 'addon.cpp'
    Set-Content -LiteralPath $dll -Value 'synthetic candidate bytes' -Encoding utf8
    Set-Content -LiteralPath $wrongDll -Value 'other synthetic candidate bytes' -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $repo 'addon.cpp') -Destination $addon
    $includeDirectory = Join-Path $fixture 'includes'
    [void][IO.Directory]::CreateDirectory($includeDirectory)
    $inputFile = Join-Path $includeDirectory 'present_path.hpp'
    Set-Content -LiteralPath $inputFile -Value 'synthetic original input' -Encoding utf8
    $inputs = Join-Path $fixture 'inputs.json'
    $missingHeaderInputs = Join-Path $fixture 'missing-header-inputs.json'
    $inputRecords = @(
        (New-CandidateFixtureReference $addon),
        (New-CandidateFixtureReference (Join-Path $repo 'host/lock.json')),
        (New-CandidateFixtureReference $inputFile)
    )
    Write-CandidateFixtureJson $inputs $inputRecords
    Write-CandidateFixtureJson $missingHeaderInputs $inputRecords[0..1]
    $sha = (Get-FileHash -LiteralPath $dll).Hash
    $inputsRef = New-CandidateFixtureReference $inputs
    $build = New-CandidateFixtureBuild (Join-Path $fixture 'build') $dll $addon $inputFile $inputsRef
    $devBuild = New-CandidateFixtureBuild (Join-Path $fixture 'dev-build') $dll $addon $inputFile $inputsRef -Dev ON
    $devCommand = New-CandidateFixtureBuild (Join-Path $fixture 'dev-command') $dll $addon $inputFile $inputsRef -ExtraDefine '/DNR_NEXUS_DEV_CONTROL=0'
    $wrongBuildDll = New-CandidateFixtureBuild (Join-Path $fixture 'wrong-build-dll') $wrongDll $addon $inputFile $inputsRef
    $nativeExe = Join-Path $fixture 'native.exe'
    Set-Content -LiteralPath $nativeExe -Value 'synthetic separate native EXE' -Encoding utf8
    $loadedModule = Join-Path $fixture 'loaded-modules.json'
    $wrongLoadedModule = Join-Path $fixture 'wrong-loaded-modules.json'
    $notLoadedModule = Join-Path $fixture 'not-loaded-modules.json'
    $installed = Join-Path $fixture 'game/renodx-dlss5.addon64'
    $module = @{
        process=@{pid=2002; created=134391287367110585}; expected_path=$installed
        expected_sha256=$sha; actual_sha256=$sha; matching_paths=@($installed); modules=@($installed)
    }
    Write-CandidateFixtureJson $loadedModule $module
    $module.actual_sha256 = (Get-FileHash -LiteralPath $wrongDll).Hash
    Write-CandidateFixtureJson $wrongLoadedModule $module
    $module.actual_sha256 = $sha
    $module.matching_paths = @()
    Write-CandidateFixtureJson $notLoadedModule $module

    $receiptPath = Join-Path $fixture 'receipt.json'
    $parameters = @{
        ReceiptPath=$receiptPath; DllPath=$dll; RunId='current-attempt'
        OfflineCase='native-ngx-stage-switch'; OfflineStages=@('Upscaled','Render')
        HostCase='xefg-stage-hot-switch'; HostStages=@('Present','Upscaled','Render')
    }
    $binding = @{runId=$parameters.RunId; sourceTree=$tree; candidateDllSha256=$sha; inputsSha256=$inputsRef.sha256}
    $fixtureAttempt = @{
        receipt=@{
            schema='nr-nexus-candidate-acceptance/1'; runId=$parameters.RunId; sourceCommit=$commit; sourceTree=$tree
            dllSha256=$sha; build=$build; offline=$null; host=$null; costs=$null
        }
        offline=@{
            schema='nr-nexus-candidate-trace/1'; kind='NATIVE_NGX'; status='Passed'; case=$parameters.OfflineCase
            executable=(New-CandidateFixtureReference $nativeExe)
            fgProvider='MockXeFG'; fgOnSamples=8; fgOffSamples=0
            phases=@(
                @{stage='Upscaled'; evaluateCalls=1; evaluateSequence=100; deliveredSequence=100; readbackSequence=100},
                @{stage='Render'; evaluateCalls=1; evaluateSequence=105; deliveredSequence=105; readbackSequence=105}
            )
        } + $binding
        host=@{
            schema='nr-nexus-candidate-trace/1'; kind='IN_GAME'; status='Passed'; case=$parameters.HostCase
            buildFlavor='Ordinary'; fgProvider='XeFG'; fgOnSamples=9; fgOffSamples=0
            pid=2002; processCreated=134391287367110585
            loadedModule=(New-CandidateFixtureReference $loadedModule)
            phases=@(
                @{stage='Present'; evaluateCalls=1; evaluateSequence=200; deliveredSequence=200},
                @{stage='Upscaled'; evaluateCalls=1; evaluateSequence=210; deliveredSequence=210},
                @{stage='Render'; evaluateCalls=1; evaluateSequence=220; deliveredSequence=220}
            )
        } + $binding
        costs=@{
            schema='nr-nexus-adaptive-preparation-cost/1'; status='Passed'; evidence='NATIVE_GPU+NATIVE_NGX'
            stage='Upscaled'; condition='full'; milestones=@{first_completed_active_nr=0}
            frames=@(
                @{source=0; phase='startup'; active_evaluate_count=1; source_successes=1; fence=1; completed_fence=1
                    preparation_cpu_ms=15.0; hook_cpu_ms=82.0; hook_gpu=@{ms=10.0}},
                @{source=1; phase='startup'; active_evaluate_count=1; source_successes=1; fence=2; completed_fence=2
                    preparation_cpu_ms=76.5; hook_cpu_ms=98.0; hook_gpu=@{ms=10.0}},
                @{source=2; phase='upper-steady'; active_evaluate_count=1; source_successes=1; fence=3; completed_fence=3
                    preparation_cpu_ms=0.0; hook_cpu_ms=1.1; hook_gpu=@{ms=7.0}},
                @{source=3; phase='upper-steady'; active_evaluate_count=1; source_successes=1; fence=4; completed_fence=4
                    preparation_cpu_ms=0.0; hook_cpu_ms=1.0; hook_gpu=@{ms=7.25}}
            )
        } + $binding
    }
    $baseline = ConvertTo-Json -InputObject $fixtureAttempt -Depth 12
    Write-CandidateFixtureAttempt $fixtureAttempt $receiptPath
    $result = Test-CandidateAcceptance @parameters
    if ($result.status -cne 'Passed' -or -not $result.candidateComplete -or $result.verifiedInputs -ne 3) {
        throw "Valid candidate fixture did not pass: $($result | ConvertTo-Json -Compress)"
    }
    if ($result.costs.firstFrame.valueMs -ne 82 -or $result.costs.tierPreparation.valueMs -ne 91.5 -or
        $result.costs.steadyState.valueMs -ne 7.125) {
        throw 'Candidate costs must reflect the numeric harness samples.'
    }

    $cases = @(
        @{name='missing offline'; status='Failed'; reason='NATIVE_NGX trace source'; edit={ param($r) $r.receipt.Remove('offline') }},
        @{name='source failed despite claimed pass'; status='Failed'; reason='Passed terminal record'; edit={
            param($r) $r.receipt.status='Passed'; $r.offline.status='Failed' }},
        @{name='wrong tree'; status='Failed'; reason='current source tree'; edit={ param($r) $r.receipt.sourceTree='a' * 40 }},
        @{name='wrong commit'; status='Failed'; reason='recorded source tree'; edit={ param($r) $r.receipt.sourceCommit=$tree }},
        @{name='wrong DLL'; status='Failed'; reason='Candidate DLL SHA-256'; edit={ param($r) $r.receipt.dllSha256='b' * 64 }},
        @{name='wrong build DLL'; status='Failed'; reason='Build output DLL SHA-256'; edit={ param($r) $r.receipt.build=$wrongBuildDll }},
        @{name='wrong inputs'; status='Failed'; reason='Build input manifest SHA-256'; edit={ param($r) $r.receipt.build.inputs.sha256='c' * 64 }},
        @{name='missing actual compiler dependency'; status='Failed'; reason='missing a compiler dependency:.*present_path.hpp'; edit={
            param($r) $r.receipt.build.inputs=New-CandidateFixtureReference $missingHeaderInputs }},
        @{name='failed build'; status='Failed'; reason='Passed build receipt'; edit={ param($r) $r.receipt.build.status='Failed' }},
        @{name='DEV cache'; status='Failed'; reason='NR_NEXUS_DEV_CONTROL:BOOL=OFF'; edit={ param($r) $r.receipt.build=$devBuild }},
        @{name='DEV define 0'; status='Failed'; reason='compiler command contains NR_NEXUS_DEV_CONTROL'; edit={ param($r) $r.receipt.build=$devCommand }},
        @{name='substitute suite'; status='Failed'; reason='evidence kind or case'; edit={ param($r) $r.offline.case='all' }},
        @{name='source missing exact case'; status='Failed'; reason='evidence kind or case'; edit={ param($r) $r.offline.Remove('case') }},
        @{name='mock Evaluate'; status='Failed'; reason='evidence kind or case'; edit={ param($r) $r.offline.kind='WARP+MOCK' }},
        @{name='zero Evaluate'; status='Failed'; reason='new accepted Evaluate'; edit={ param($r) $r.offline.phases[0].evaluateCalls=0 }},
        @{name='wrong phase'; status='Failed'; reason='phase 2 must be Render'; edit={ param($r) $r.offline.phases[1].stage='Present' }},
        @{name='duplicate phase sequence'; status='Failed'; reason='new accepted Evaluate'; edit={
            param($r) $r.offline.phases[1].evaluateSequence=100; $r.offline.phases[1].deliveredSequence=100 }},
        @{name='unconfirmed delivery'; status='Failed'; reason='completed delivery'; edit={ param($r) $r.host.phases[1].Remove('deliveredSequence') }},
        @{name='readback of other frame'; status='Failed'; reason='readback of the delivered output'; edit={ param($r) $r.offline.phases[1].readbackSequence=100 }},
        @{name='FG off once'; status='Failed'; reason='FG stayed on'; edit={ param($r) $r.host.fgOffSamples=1 }},
        @{name='cost source failed'; status='Failed'; reason='Passed real NGX'; edit={ param($r) $r.costs.status='Failed' }},
        @{name='measured cost has no steady samples'; status='Failed'; reason='measured cost: steadyState'; edit={
            param($r) foreach ($frame in $r.costs.frames) { $frame.phase='startup' } }},
        @{name='cost binding wrong'; status='Failed'; reason='Cost report binding mismatch: runId'; edit={ param($r) $r.costs.runId='previous-attempt' }},
        @{name='missing cost source'; status='Failed'; reason='Offline cost source'; edit={ param($r) $r.receipt.Remove('costs') }},
        @{name='no host'; status='PendingHost'; reason='in-game trace'; edit={ param($r) $r.receipt.Remove('host') }},
        @{name='host not run'; status='PendingHost'; reason='in-game trace'; edit={ param($r) $r.host.status='NotRun' }},
        @{name='failed host'; status='Failed'; reason='Passed terminal record'; edit={ param($r) $r.host.status='Failed' }},
        @{name='DEV host trace'; status='Failed'; reason='Ordinary host build'; edit={ param($r) $r.host.buildFlavor='DEV' }},
        @{name='no actual load'; status='PendingHost'; reason='Actual module-load'; edit={ param($r) $r.host.Remove('loadedModule') }},
        @{name='load file absent'; status='PendingHost'; reason='Actual module-load'; edit={ param($r) $r.host.loadedModule.path='absent.json' }},
        @{name='candidate never loaded'; status='Failed'; reason='Actual loaded module'; edit={
            param($r) $r.host.loadedModule=New-CandidateFixtureReference $notLoadedModule }},
        @{name='different DLL loaded'; status='Failed'; reason='Actual loaded module'; edit={
            param($r) $r.host.loadedModule=New-CandidateFixtureReference $wrongLoadedModule }},
        @{name='different host process'; status='Failed'; reason='Actual loaded module'; edit={ param($r) $r.host.pid=2003 }},
        @{name='old receipt even with identical DLL'; status='Failed'; reason='wrong schema or runId'; edit={ param($r) $r.receipt.runId='previous-attempt' }},
        @{name='old successful section copied into current receipt'; status='Failed'; reason='binding mismatch: runId'; edit={
            param($r) $r.host.runId='previous-attempt' }},
        @{name='different trace inputs'; status='Failed'; reason='binding mismatch: inputsSha256'; edit={
            param($r) $r.offline.inputsSha256='d' * 64 }}
    )
    foreach ($case in $cases) {
        $attempt = $baseline | ConvertFrom-Json -AsHashtable
        & $case.edit $attempt | Out-Null
        Write-CandidateFixtureAttempt $attempt $receiptPath
        $result = Test-CandidateAcceptance @parameters
        if ($result.status -cne $case.status -or $result.candidateComplete -or $result.reason -notmatch $case.reason) {
            throw "$($case.name) did not return $($case.status): $($result | ConvertTo-Json -Compress)"
        }
    }
    Write-CandidateFixtureAttempt ($baseline | ConvertFrom-Json -AsHashtable) $receiptPath
    Set-Content -LiteralPath $inputFile -Value 'changed after the recorded build' -Encoding utf8
    $result = Test-CandidateAcceptance @parameters
    if ($result.status -cne 'Failed' -or $result.candidateComplete -or $result.reason -notmatch 'Build input SHA-256 mismatch') {
        throw 'A modified actual build input must invalidate the candidate.'
    }
    Set-Content -LiteralPath $inputFile -Value 'synthetic original input' -Encoding utf8

    $preflight = Join-Path $repo 'tools/run-release-preflight.ps1'
    $preflightParameters = @{
        Mode='CandidateAcceptance'; CandidateReceipt=$receiptPath; CandidateDll=$dll; CandidateRunId=$parameters.RunId
        CandidateOfflineCase=$parameters.OfflineCase; CandidateOfflineStages=$parameters.OfflineStages
        CandidateHostCase=$parameters.HostCase; CandidateHostStages=$parameters.HostStages
    }
    $preflightDirectory = Join-Path $fixture 'preflight-passed'
    & $preflight @preflightParameters -OutputDirectory $preflightDirectory
    $preflightResult = Get-Content -LiteralPath (Join-Path $preflightDirectory 'result.json') -Raw | ConvertFrom-Json
    if ($preflightResult.status -cne 'Passed' -or -not $preflightResult.candidateAcceptance.candidateComplete) {
        throw 'Candidate mode failed to report the valid candidate.'
    }
    $fixtureAttempt.receipt.Remove('host')
    Write-CandidateFixtureAttempt $fixtureAttempt $receiptPath
    $preflightDirectory = Join-Path $fixture 'preflight-pending-host'
    $rejected = $false
    try { & $preflight @preflightParameters -OutputDirectory $preflightDirectory }
    catch { if ($_.Exception.Message -notmatch 'Candidate acceptance PendingHost') { throw }; $rejected = $true }
    $preflightResult = Get-Content -LiteralPath (Join-Path $preflightDirectory 'result.json') -Raw | ConvertFrom-Json
    if (-not $rejected -or $preflightResult.status -cne 'PendingHost' -or $preflightResult.candidateAcceptance.candidateComplete) {
        throw 'Candidate mode must report PendingHost and fail when host evidence is absent.'
    }
    $outcome = 'Passed'
    Write-Output "PASS candidate acceptance: $($cases.Count) receipt negatives, input drift and preflight integration."
} finally {
    Complete-TestOutput $output -Outcome $outcome
}
