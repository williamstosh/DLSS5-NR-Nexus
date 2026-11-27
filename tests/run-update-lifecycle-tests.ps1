[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory,
    [string]$VcVars64,
    [string]$PythonExecutable,
    [ValidateSet('Auto', 'Off', 'On')][string]$DevControlMode = 'Auto',
    [switch]$BuildOnly,
    [string]$BuildProject,
    [string]$ShaderIncludeDirectory
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path $SourceRoot 'tools/game-test/resolve-python.ps1')

$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
if (-not $BuildProject) { $BuildProject = Join-Path $SourceRoot 'build/dlss5.vcxproj' }
if (-not $ShaderIncludeDirectory) { $ShaderIncludeDirectory = Join-Path $SourceRoot 'build/dlss5.include' }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $SourceRoot ('tmp/online-update-lifecycle-validation/' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$result = [ordered]@{suite='update-lifecycle'; build='failed'; run='not_run'; evidence='Win32 loader, ReShade SDK host fixture, exact WinHTTP loopback'}
$outcome = 'Failed'
try {
    Initialize-TestBuild $VcVars64
    $compiler = (Get-Command cl.exe -CommandType Application).Path
    $explicitMode = $DevControlMode -ne 'Auto'
    if ($explicitMode) {
        if ($env:CL -match 'NR_NEXUS_DEV_CONTROL\b' -or $env:_CL_ -match 'NR_NEXUS_DEV_CONTROL\b') {
            throw 'Explicit lifecycle DEV mode conflicts with CL or _CL_.'
        }
        . (Join-Path $SourceRoot 'tools/check-release-experiment-parity.ps1') -SourceRoot $SourceRoot
        $parityInputs = Get-ReleaseParityBuildContext -SourceRoot $SourceRoot -HostRoot $HostRoot
    }
    $projectInputs = Get-TestProjectInputs $BuildProject $SourceRoot $HostRoot $ShaderIncludeDirectory
    $serverScript = Join-Path $PSScriptRoot 'update_loopback.py'
    $testInputs = @($PSCommandPath, $serverScript, (Join-Path $PSScriptRoot 'test-support.ps1'))
    $fixtureSource = Join-Path $PSScriptRoot 'update_addon_fixture.cpp'
    $harnessSource = Join-Path $PSScriptRoot 'update_addon_lifecycle_tests.cpp'
    $addonSource = Join-Path $SourceRoot 'addon.cpp'
    $fixtureInputs = $testInputs + @($BuildProject, $fixtureSource, $addonSource,
        (Join-Path $SourceRoot 'update.hpp'), (Join-Path $SourceRoot 'dlssnr.hpp'), (Join-Path $SourceRoot 'vulkan_path.hpp'))
    $fixtureInputs += @($projectInputs.Libraries | Where-Object { [IO.Path]::IsPathFullyQualified($_) })
    $harnessInputs = $testInputs + @($harnessSource, (Join-Path $PSScriptRoot 'vulkan_test_api_stubs.hpp'))
    $before = @{}
    foreach ($path in @($fixtureInputs + $harnessInputs | Sort-Object -Unique)) {
        $before[$path] = (Get-FileHash -LiteralPath $path).Hash
    }
    $common = @('/nologo', '/std:c++20', '/EHsc', '/W3', '/MT', '/O2', '/Gy', '/Gw',
        '/bigobj', '/utf-8', '/DNOMINMAX', '/DNDEBUG', '/DRESHADE_ADDON=2')
    $fixture = Join-Path $OutputDirectory 'update_addon_fixture.dll'
    $harness = Join-Path $OutputDirectory 'update_addon_lifecycle_tests.exe'
    $targets = @(
        @{name='harness'; path=$harness; source=$harnessSource; inputs=$harnessInputs; prefix='harness-'
          compile=@("/I$HostRoot/external/reshade", "/I$HostRoot/external/reshade/include")
          link=@(); object="$OutputDirectory/update_addon_lifecycle_tests.obj"}
        @{name='fixture'; path=$fixture; source=$fixtureSource; inputs=$fixtureInputs; prefix=''
          compile=@('/LD') + @($projectInputs.Includes | ForEach-Object { "/I$_" })
          link=@('/DELAYLOAD:winhttp.dll') + $projectInputs.Libraries; object="$OutputDirectory/update_addon_fixture.obj"}
    )
    if ($DevControlMode -eq 'On') { $targets[1].compile += '/DNR_NEXUS_DEV_CONTROL' }
    if ($DevControlMode -eq 'Off') { $targets[1].compile += '/UNR_NEXUS_DEV_CONTROL' }
    foreach ($target in $targets) {
        $prefix = $target.prefix
        $dependencies = "$OutputDirectory/${prefix}dependencies.json"
        $argumentsPath = "$OutputDirectory/${prefix}arguments.json"
        $inputsPath = "$OutputDirectory/${prefix}inputs.json"
        $arguments = $common + $target.compile + @($target.source, "/Fo$($target.object)", "/Fe$($target.path)",
            '/sourceDependencies', $dependencies, '/link', '/INCREMENTAL:NO', '/OPT:REF', '/OPT:ICF') + $target.link
        $arguments | ConvertTo-Json | Set-Content -LiteralPath $argumentsPath -Encoding utf8
        Register-TestCompilerOutput $OutputDirectory -ObjectPath $target.object -ExecutablePath $target.path
        $log = Join-Path $OutputDirectory "$($target.name)-compile.log"
        & $compiler @arguments *> $log
        if ($LASTEXITCODE -ne 0) {
            Get-Content -LiteralPath $log -Tail 55
            throw "Update lifecycle $($target.name) compilation failed."
        }
        $closure = (Get-Content -LiteralPath $dependencies -Raw | ConvertFrom-Json).Data
        if ($target.name -eq 'fixture' -and @($closure.Includes) -notcontains $addonSource) {
            throw 'Fixture compiler dependencies did not identify production addon.cpp.'
        }
        @(($target.inputs + @($closure.Source) + @($closure.Includes)) | Sort-Object -Unique | ForEach-Object {
            [ordered]@{path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash}
        }) | ConvertTo-Json | Set-Content -LiteralPath $inputsPath -Encoding utf8
        $target.identity = [ordered]@{
            executable=[IO.Path]::GetFileName($target.path)
            sha256=(Get-FileHash -LiteralPath $target.path).Hash
            inputsSha256=(Get-FileHash -LiteralPath $inputsPath).Hash
            argumentsSha256=(Get-FileHash -LiteralPath $argumentsPath).Hash
            dependenciesSha256=(Get-FileHash -LiteralPath $dependencies).Hash
        }
    }
    $result.build = 'passed'
    $fixtureIdentity = $targets[1].identity
    if ($explicitMode) {
        $result['buildIdentity'] = [ordered]@{
            mode=$DevControlMode; source=$parityInputs; compiler=$compiler
            compilerSha256=(Get-FileHash -LiteralPath $compiler).Hash
            compilerEnvironment=@{CL=$env:CL; _CL_=$env:_CL_}
            argumentsSha256=$fixtureIdentity.argumentsSha256; inputsSha256=$fixtureIdentity.inputsSha256
            dependenciesSha256=$fixtureIdentity.dependenciesSha256; executable=$fixtureIdentity.executable
            executableSha256=$fixtureIdentity.sha256; softwareOnly=$true; cases=@('host-lifecycle')
            harness=$targets[0].identity
        }
    }
    if (-not $BuildOnly) {
        $result.run = 'failed'
        if (-not $PythonExecutable) {
            $PythonExecutable = Resolve-NrNexusPython -RepositoryRoot $SourceRoot -RequiredImports @()
        } elseif (-not (Test-NrNexusPython $PythonExecutable @())) {
            throw "Python interpreter is not runnable: $PythonExecutable"
        }
        $ready = Join-Path $OutputDirectory 'loopback-ready.txt'
        $requestsPath = Join-Path $OutputDirectory 'loopback-requests.jsonl'
        $receipt = Join-Path $OutputDirectory 'host-lifecycle.stdout.log'
        $cases = @('gate-headers', 'gate-body', 'gate-cold')
        Register-TestDisposable $OutputDirectory $ready 'loopback-ready'
        Register-TestDisposable $OutputDirectory ([IO.Path]::ChangeExtension($ready, '.part')) 'loopback-ready'
        foreach ($case in $cases) {
            Register-TestDisposable $OutputDirectory (Join-Path $OutputDirectory "$case.entered") 'loopback-gate'
            Register-TestDisposable $OutputDirectory (Join-Path $OutputDirectory "$case.release") 'loopback-gate'
        }
        $server = $null
        $previousCapture = $env:RENODX_NR_TEST_UI_CAPTURE
        $previousTestReport = $env:RENODX_NR_TEST_REPORT
        try {
            $env:RENODX_NR_TEST_UI_CAPTURE = $null
            $env:RENODX_NR_TEST_REPORT = $null
            $server = Start-Process -FilePath $PythonExecutable -WindowStyle Hidden -PassThru -ArgumentList @(
                '-u', '-B', ('"' + $serverScript + '"'), '--ready', ('"' + $ready + '"'),
                '--log', ('"' + $requestsPath + '"'), '--gate-directory', ('"' + $OutputDirectory + '"')
            ) -RedirectStandardOutput (Join-Path $OutputDirectory 'loopback-server.stdout.log') `
                -RedirectStandardError (Join-Path $OutputDirectory 'loopback-server.stderr.log')
            $deadline = [DateTime]::UtcNow.AddSeconds(10)
            while (-not (Test-Path -LiteralPath $ready)) {
                if ($server.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Lifecycle loopback did not start.' }
                Start-Sleep -Milliseconds 50
            }
            $port = (Get-Content -LiteralPath $ready -Raw).Trim()
            if ($port -notmatch '^[0-9]{1,5}$' -or [int]$port -lt 1 -or [int]$port -gt 65535) {
                throw 'Invalid lifecycle loopback port.'
            }
            foreach ($case in $cases) {
                $scratch = (New-Item -ItemType Directory -Path (Join-Path $OutputDirectory $case)).FullName
                try {
                    Invoke-TestProcess $harness $scratch $case @(('"' + $fixture + '"'), $port, $case,
                        ('"' + $OutputDirectory + '"')) -TimeoutMilliseconds 35000 -DeferFailureCleanup
                } finally {
                    $stdout = Join-Path $scratch "$case.stdout.log"
                    if (Test-Path -LiteralPath $stdout) {
                        Get-Content -LiteralPath $stdout | Add-Content -LiteralPath $receipt
                    }
                    Get-ChildItem -LiteralPath $scratch -Recurse -File |
                        Where-Object Name -notin @("$case.stdout.log", "$case.stderr.log") |
                        ForEach-Object { Register-TestDisposable $OutputDirectory $_.FullName 'synthetic-lifecycle-files' }
                }
            }
        } finally {
            $env:RENODX_NR_TEST_UI_CAPTURE = $previousCapture
            $env:RENODX_NR_TEST_REPORT = $previousTestReport
            if ($server) {
                if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
                $server.Dispose()
            }
        }
        $requests = @(Get-Content -LiteralPath $requestsPath | ForEach-Object { $_ | ConvertFrom-Json })
        foreach ($case in $cases) {
            $expectedChecks = if ($case -eq 'gate-cold') { 2 } else { 1 }
            $checks = @($requests | Where-Object path -eq "/check/$case")
            if ($checks.Count -ne $expectedChecks -or @($checks | Where-Object { $null -ne $_.etag }).Count -ne 0 -or
                @($requests | Where-Object path -eq "/download/$case").Count -ne 1) {
                throw "Lifecycle check/download request sequence differed: $case"
            }
        }
        if ($requests.Count -ne 7) { throw 'Unexpected lifecycle loopback requests.' }
    }
    foreach ($path in $before.Keys) {
        if ((Get-FileHash -LiteralPath $path).Hash -cne $before[$path]) { throw "Lifecycle input changed: $path" }
    }
    foreach ($target in $targets) {
        if ((Get-FileHash -LiteralPath $target.path).Hash -cne $target.identity.sha256) {
            throw "Lifecycle binary changed: $($target.path)"
        }
    }
    if (-not $BuildOnly) {
        $passed = 'NR_RELEASE_PARITY online-update host_unload_nonblocking_rebuild_state=passed'
        Add-Content -LiteralPath $receipt -Value $passed
        Write-Host $passed
        $result.run = 'passed'
    }
    $outcome = 'Passed'
    Write-Host "Update add-on lifecycle: $($result.run). Evidence: $OutputDirectory"
} finally {
    $result | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
