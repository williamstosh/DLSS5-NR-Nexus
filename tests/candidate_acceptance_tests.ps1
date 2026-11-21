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

function Set-CandidateAssemblyFixtureArguments($Build, [string[]]$Arguments) {
    Write-CandidateFixtureJson $Build.argumentsPath $Arguments
    $Build.result.buildIdentity.argumentsSha256 = (Get-FileHash -LiteralPath $Build.argumentsPath).Hash
    Write-CandidateFixtureJson $Build.resultPath $Build.result
}

function New-CandidateAssemblyFixture($Directory, $ExeBytes, $Source, $Header, $Compiler) {
    [void][IO.Directory]::CreateDirectory((Join-Path $Directory 'inputs'))
    $exe = Join-Path $Directory 'fg_native_tests.exe'
    $dependencies = Join-Path $Directory 'dependencies.json'
    $inputs = Join-Path $Directory 'inputs.json'
    Write-CandidateFixtureJson $dependencies @{Data=@{Source=$Source; Includes=@($Header)}}
    Write-CandidateFixtureJson $inputs @((New-CandidateFixtureReference $Source), (New-CandidateFixtureReference $Header))
    $build = @{
        argumentsPath=(Join-Path $Directory 'arguments.json'); resultPath=(Join-Path $Directory 'result.json')
        original=$exe; archive=(Join-Path $Directory 'inputs/fg_native_tests.exe')
        result=@{suite='fg-native'; build='passed'; run='passed'; buildIdentity=@{
            mode='Off'; softwareOnly=$false; executable='fg_native_tests.exe'; executableSha256=(Get-FileHash -LiteralPath $ExeBytes).Hash
            compiler=$Compiler.path; compilerSha256=$Compiler.sha256; compilerEnvironment=@{CL=$null; _CL_=$null}
            inputsSha256=(Get-FileHash -LiteralPath $inputs).Hash; dependenciesSha256=(Get-FileHash -LiteralPath $dependencies).Hash
        }}
    }
    $build.arguments = @('/O2', '/DNR_NEXUS_TEST_NATIVE_NGX', '/UNR_NEXUS_DEV_CONTROL', $Source,
        ('/Fo' + (Join-Path $Directory 'fg_native_tests.obj')), "/Fe$exe", '/sourceDependencies', $dependencies)
    Set-CandidateAssemblyFixtureArguments $build $build.arguments
    return $build
}

function Assert-CandidateAssemblyRejected([scriptblock]$Check, [string]$Reason) {
    try { $null = & $Check }
    catch {
        if ($_.Exception.Message -notmatch $Reason) { throw }
        return
    }
    throw "Candidate assembly accepted a fixture that should fail with $Reason."
}

function Test-CandidateAssemblyContracts($Fixture, $ExeBytes, $DifferentBytes, $Source, $Header) {
    $tokens = $null
    $parseErrors = $null
    $assembler = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $repo 'tools/new-candidate-acceptance-receipt.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw $parseErrors[0] }
    foreach ($name in @('Read-AssemblyJson', 'Get-AssemblyFile', 'Read-AssemblyBuild')) {
        $definition = $assembler.Find({param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name
        }, $false)
        if (-not $definition) { throw "Missing candidate assembler function: $name" }
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    $comparison = @($assembler.EndBlock.Statements | Where-Object {
        $_ -is [Management.Automation.Language.IfStatementAst] -and
            $_.Extent.Text.Contains('must share the compiler, normalized commands and actual dependency closure')
    })
    if ($comparison.Count -ne 1) { throw 'Cannot locate the production assembly compiler equivalence check.' }
    $compare = [scriptblock]::Create($comparison[0].Extent.Text)

    $compiler = New-CandidateFixtureReference $Header
    $nativeDirectory = Join-Path $Fixture 'assembly-native'
    $costDirectory = Join-Path $Fixture 'assembly-cost'
    $nativeFixture = New-CandidateAssemblyFixture $nativeDirectory $ExeBytes $Source $Header $compiler
    $costFixture = New-CandidateAssemblyFixture $costDirectory $DifferentBytes $Source $Header $compiler
    Assert-CandidateAssemblyRejected { Read-AssemblyBuild $nativeDirectory } 'inputs[\\/]fg_native_tests\.exe'
    Copy-Item -LiteralPath $DifferentBytes -Destination $nativeFixture.archive
    Assert-CandidateAssemblyRejected { Read-AssemblyBuild $nativeDirectory } 'executable SHA-256 mismatch'
    Copy-Item -LiteralPath $ExeBytes -Destination $nativeFixture.archive -Force
    $native = Read-AssemblyBuild $nativeDirectory
    if ($native.files.executable.path -ine $nativeFixture.archive -or
        $native.files.originalExecutablePath -ine $nativeFixture.original) {
        throw 'Candidate assembly must keep the verified archive and original /Fe path separately.'
    }
    $archiveArguments = $nativeFixture.arguments.Replace("/Fe$($nativeFixture.original)", "/Fe$($nativeFixture.archive)")
    Set-CandidateAssemblyFixtureArguments $nativeFixture $archiveArguments
    Assert-CandidateAssemblyRejected { Read-AssemblyBuild $nativeDirectory } 'ordinary NGX build'

    Copy-Item -LiteralPath $DifferentBytes -Destination $costFixture.original
    $costBuild = Read-AssemblyBuild $costDirectory
    if ($native.files.executable.sha256 -ieq $costBuild.files.executable.sha256) { throw 'Expected distinct synthetic EXEs.' }
    & $compare
    Set-CandidateAssemblyFixtureArguments $costFixture ($costFixture.arguments + '/DNR_NEXUS_DEV_CONTROL=0')
    Assert-CandidateAssemblyRejected { Read-AssemblyBuild $costDirectory } 'ordinary NGX build'
    Set-CandidateAssemblyFixtureArguments $costFixture ($costFixture.arguments.Replace('/O2', '/Od'))
    $costBuild = Read-AssemblyBuild $costDirectory
    Assert-CandidateAssemblyRejected { & $compare } 'normalized commands'

    $preflight = [Management.Automation.Language.Parser]::ParseFile(
        (Join-Path $repo 'tools/run-release-preflight.ps1'), [ref]$tokens, [ref]$parseErrors)
    if ($parseErrors.Count) { throw $parseErrors[0] }
    $archiveFunction = $preflight.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Archive-NativeExecutable'
    }, $false)
    if (-not $archiveFunction) { throw 'Missing native input archive entry.' }
    . ([scriptblock]::Create($archiveFunction.Extent.Text))
    $library = Join-Path $Fixture 'Detours/lib.X64/detours.lib'
    [void][IO.Directory]::CreateDirectory((Split-Path $library -Parent))
    Set-Content -LiteralPath $library -Value 'synthetic original Detours bytes' -Encoding utf8
    $linkInput = New-CandidateFixtureReference $library
    $inputPath = Join-Path $costDirectory 'inputs.json'
    $recordedInputs = Read-AssemblyJson $inputPath
    Write-CandidateFixtureJson $inputPath (@($recordedInputs) + @($linkInput))
    $costFixture.result.buildIdentity.inputsSha256 = (Get-FileHash -LiteralPath $inputPath).Hash
    Write-CandidateFixtureJson $costFixture.resultPath $costFixture.result
    Set-Content -LiteralPath $library -Value 'overwritten before archival' -Encoding utf8
    Assert-CandidateAssemblyRejected { Archive-NativeExecutable $costDirectory } 'Detours SHA-256 differs'
    Set-Content -LiteralPath $library -Value 'synthetic original Detours bytes' -Encoding utf8
    $archived = Archive-NativeExecutable $costDirectory
    $FrozenInputsManifest = Get-CandidateEvidencePath $archived.frozenInputsManifest $costDirectory 'Native archive manifest'
    $manifest = Read-AssemblyJson $FrozenInputsManifest
    if ($manifest.path -ine $library -or $manifest.sha256 -ine $linkInput.sha256 -or
        $manifest.frozenSha256 -ine $linkInput.sha256 -or
        (Get-FileHash -LiteralPath $manifest.frozenPath).Hash -ine $linkInput.sha256) {
        throw 'Archived Detours bytes must match the actual compiler input SHA-256.'
    }
    $archivedBuild = Read-AssemblyBuild $costDirectory
    if ($archivedBuild.files.executable.sha256 -ine $archived.executable.sha256) {
        throw 'Detours archival must retain the original EXE identity.'
    }
    $statements = @($assembler.EndBlock.Statements)
    $start = 0
    while ($start -lt $statements.Count -and $statements[$start].Extent.Text -cne '$frozenInput = $null') { $start++ }
    $end = $start
    while ($end -lt $statements.Count -and -not $statements[$end].Extent.Text.StartsWith('$trackedChanges =')) { $end++ }
    if ($start -eq $statements.Count -or $end -eq $statements.Count) { throw 'Missing frozen native input validation.' }
    $frozenChecks = [scriptblock]::Create(($statements[$start..($end - 1)].Extent.Text -join "`n"))
    Set-Content -LiteralPath $library -Value 'overwritten by a later build' -Encoding utf8
    $nativeHashes = $archivedBuild.inputHashes
    . $frozenChecks
    if (-not $usedFrozenInput -or $frozenInput.sha256 -ine $linkInput.sha256) {
        throw 'The exact Detours archive must remain usable after the shared source is overwritten.'
    }
    Set-Content -LiteralPath $manifest.frozenPath -Value 'incorrect archived bytes' -Encoding utf8
    Assert-CandidateAssemblyRejected { & $frozenChecks } 'Frozen native input SHA-256 mismatch'
    Write-Output 'PASS native link input CPU_SYNTHETIC: exact Detours archive survives source overwrite and rejects wrong bytes.'
}

function Test-CandidateNestedSrContracts {
    $baseline = @{
        nested_sr_deliveries=@(
            @{delivery='DELIVERED'; accepted_evaluate_sequence=101; completed_group_sequence=51
                readback_pixels=64; readback_fence_value=11; readback_fence_completed=11
                gpu_fence_healthy=$true; sdk_fg_enabled=$true; outer_enhanced_output_matches=$true},
            @{delivery='DELIVERED'; accepted_evaluate_sequence=103; completed_group_sequence=52
                readback_pixels=64; readback_fence_value=12; readback_fence_completed=12
                gpu_fence_healthy=$true; sdk_fg_enabled=$true; outer_enhanced_output_matches=$true}
        )
        nested_sr_failure=@{evidence='NATIVE_GPU+MOCK_SR_FAILURE'; nr_evaluate_count=0; new_completed_groups=0; output_unchanged=$true}
        nested_source_output=@{status='NotRun'; reason='SR RGBA16F and SDK RGBA8 backbuffer formats differ'}
    } | ConvertTo-Json -Depth 8
    $cases = @(
        @{reason='two actual delivered frames'; edit={param($h) $h.Remove('nested_sr_deliveries')}},
        @{reason='two actual delivered frames'; edit={param($h) $h.nested_sr_deliveries=@($h.nested_sr_deliveries[0])}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].delivery='PENDING'}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].accepted_evaluate_sequence=101}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].completed_group_sequence=51}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].readback_fence_completed=11}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].gpu_fence_healthy=$false}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].sdk_fg_enabled=$false}},
        @{reason='distinct accepted and completed'; edit={param($h) $h.nested_sr_deliveries[1].outer_enhanced_output_matches=$false}},
        @{reason='zero NR Evaluate'; edit={param($h) $h.nested_sr_failure.evidence='WARP+MOCK'}},
        @{reason='zero NR Evaluate'; edit={param($h) $h.nested_sr_failure.nr_evaluate_count=1}},
        @{reason='zero NR Evaluate'; edit={param($h) $h.nested_sr_failure.new_completed_groups=1}},
        @{reason='zero NR Evaluate'; edit={param($h) $h.nested_sr_failure.output_unchanged=$false}},
        @{reason='SourceOutput must remain NotRun'; edit={param($h) $h.nested_source_output.status='Passed'}}
    )
    foreach ($path in @('tools/run-release-preflight.ps1', 'tools/new-candidate-acceptance-receipt.ps1')) {
        $tokens = $null
        $parseErrors = $null
        $parser = [Management.Automation.Language.Parser]::ParseFile(
            (Join-Path $repo $path), [ref]$tokens, [ref]$parseErrors)
        if ($parseErrors.Count) { throw $parseErrors[0] }
        $first = $parser.Find({param($node)
            $node -is [Management.Automation.Language.AssignmentStatementAst] -and
                $node.Extent.Text.StartsWith('$nestedDeliveries =')
        }, $true)
        if (-not $first) { throw "Missing native nested SR receipt checks: $path" }
        $statements = @($first.Parent.Statements)
        $start = [Array]::IndexOf($statements, $first)
        $end = $start
        while ($end -lt $statements.Count -and
            -not $statements[$end].Extent.Text.Contains('Native nested SourceOutput must remain NotRun')) { $end++ }
        if ($start -lt 0 -or $end -eq $statements.Count) { throw "Incomplete native nested SR receipt checks: $path" }
        $nestedChecks = [scriptblock]::Create(($statements[$start..$end].Extent.Text -join "`n"))
        $handoff = $baseline | ConvertFrom-Json -AsHashtable
        . $nestedChecks
        if ($nestedDeliveries.Count -ne 2 -or $nestedFailure.nr_evaluate_count -ne 0 -or
            $nestedSourceOutput.status -cne 'NotRun' -or
            $nestedSourceOutput.reason -cne 'SR RGBA16F and SDK RGBA8 backbuffer formats differ') {
            throw "Native nested SR checks failed to retain the report facts: $path"
        }
        foreach ($case in $cases) {
            $handoff = $baseline | ConvertFrom-Json -AsHashtable
            & $case.edit $handoff | Out-Null
            Assert-CandidateAssemblyRejected { & $nestedChecks } $case.reason
        }
    }
    Write-Output "PASS nested SR CPU_SYNTHETIC: both native receipt consumers reject $($cases.Count) missing or invalid evidence variants."
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
    Test-CandidateAssemblyContracts $fixture $nativeExe $wrongDll $addon $inputFile
    Test-CandidateNestedSrContracts
    $outcome = 'Passed'
    Write-Output 'PASS candidate assembly CPU_SYNTHETIC: archive SHA, original /Fe identity and equivalent compiler conditions.'
    Write-Output "PASS candidate acceptance: $($cases.Count) receipt negatives, input drift and preflight integration."
} finally {
    Complete-TestOutput $output -Outcome $outcome
}
