[CmdletBinding()]
param(
    [string]$HostRoot = (Join-Path $PSScriptRoot '../.local/renodx'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../tmp/tests/look-transport-' + [guid]::NewGuid().ToString('N'))),
    [string]$BaselineShaderPath,
    [string]$RoiFixture,
    [string]$VcVars64
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
. (Join-Path $PSScriptRoot 'test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$outcome = 'Failed'
try {
    Initialize-TestBuild $VcVars64
    $source = Join-Path $PSScriptRoot 'look_transport_tests.cpp'
    $shader = Join-Path $root 'shaders/look_upsample.cs_5_1.hlsl'
    $exe = Join-Path $OutputDirectory 'look_transport_tests.exe'
    $object = Join-Path $OutputDirectory 'look_transport_tests.obj'
    $dependencies = Join-Path $OutputDirectory 'dependencies.json'
    $inputs = @($source, $PSCommandPath, (Join-Path $PSScriptRoot 'test-support.ps1'),
        (Join-Path $PSScriptRoot 'd3d12_test_gpu.hpp'), (Join-Path $root 'look_stage.hpp'),
        (Join-Path $root 'host/lock.json'), (Join-Path $root 'host/renodx-compat.patch'),
        $shader, (Join-Path $root 'shaders/look_common.hlsli'),
        (Join-Path $root 'shaders/codec_math.hlsli'), (Join-Path $root 'shaders/vk_binding.hlsli'))
    $fixtureArgument = '-'
    if ($RoiFixture) {
        $RoiFixture = (Resolve-Path -LiteralPath $RoiFixture).Path
        $fixture = Get-Content -LiteralPath $RoiFixture -Raw | ConvertFrom-Json
        if ($fixture.schema -ne 'nr-nexus-look-transport-fixture/1') { throw 'Unsupported ROI fixture schema.' }
        $inputs += $RoiFixture
        foreach ($name in @('reference', 'proxy', 'neural', 'expectedNeural')) {
            if ($fixture.PSObject.Properties.Name -contains $name) {
                $inputs += [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetDirectoryName($RoiFixture)) $fixture.$name.path))
            }
        }
        $fixtureArgument = $RoiFixture
    }
    $baseline = $null
    if ($BaselineShaderPath) {
        $BaselineShaderPath = (Resolve-Path -LiteralPath $BaselineShaderPath).Path
        $inputs += $BaselineShaderPath
        # Only the upsample shader changes in the negative control; its shared math/bindings stay identical.
        $baselineDirectory = Join-Path $OutputDirectory 'source/baseline'
        [void][IO.Directory]::CreateDirectory($baselineDirectory)
        $baseline = Join-Path $baselineDirectory 'look_upsample.cs_5_1.hlsl'
        Copy-Item -LiteralPath $BaselineShaderPath -Destination $baseline
        foreach ($name in @('look_common.hlsli', 'codec_math.hlsli', 'vk_binding.hlsli')) {
            Copy-Item -LiteralPath (Join-Path $root "shaders/$name") -Destination (Join-Path $baselineDirectory $name)
        }
        $inputs += @(Get-ChildItem -LiteralPath $baselineDirectory -File | ForEach-Object FullName)
    }
    $before = @{}
    foreach ($path in ($inputs | Select-Object -Unique)) { $before[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    $before | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
    $arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/utf-8',
        '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DNR_NEXUS_TEST_WARP_ONLY=1',
        "/I$root", "/I$HostRoot/src", "/I$HostRoot/src/addons/dlss5", "/I$HostRoot/external/reshade/include",
        "/I$HostRoot/external/json/include", "/I$HostRoot/external/Detours/src", "/I$HostRoot/external/gtl/include",
        $source, "/Fo$object", "/Fe$exe", '/sourceDependencies', $dependencies,
        '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'd3dcompiler.lib')
    $arguments | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'arguments.json') -Encoding utf8
    Register-TestCompilerOutput $OutputDirectory -ObjectPath $object -ExecutablePath $exe
    & cl.exe @arguments *> (Join-Path $OutputDirectory 'compile.log')
    if ($LASTEXITCODE -ne 0) { throw "Look transport fixture compilation failed; see $OutputDirectory/compile.log" }
    foreach ($path in (Get-Content -LiteralPath $dependencies -Raw | ConvertFrom-Json).Data.Includes) {
        if (-not $before.ContainsKey($path)) { $before[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash }
    }
    $before | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
    $roiRoot = Join-Path $OutputDirectory 'roi'
    foreach ($mode in @('fixed', 'legacy')) {
        if ($mode -eq 'legacy' -and -not $baseline) { continue }
        $programShader = if ($mode -eq 'fixed') { $shader } else { $baseline }
        $roiDirectory = Join-Path $roiRoot $mode
        [void][IO.Directory]::CreateDirectory($roiDirectory)
        $result = Join-Path $OutputDirectory "$mode.json"
        $runArguments = @($root, $programShader, $result, $mode, $fixtureArgument, $roiDirectory) |
            ForEach-Object { '"' + $_ + '"' }
        Invoke-TestProcess -Executable $exe -Directory $OutputDirectory -Name $mode -Arguments $runArguments `
            -TimeoutMilliseconds 180000 -DeferFailureCleanup
        if ((Get-Content -LiteralPath $result -Raw | ConvertFrom-Json).status -ne 'Passed') {
            throw "Look transport $mode result did not pass."
        }
    }
    foreach ($path in $before.Keys) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -cne $before[$path]) {
            throw "Input changed during transport regression: $path"
        }
    }
    $readbacks = @{}
    foreach ($file in @(Get-ChildItem -LiteralPath $roiRoot -Filter '*.f32' -File -Recurse)) {
        $readbacks[[IO.Path]::GetRelativePath($OutputDirectory, $file.FullName)] = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash
    }
    [ordered]@{
        status = 'Passed'; evidence = 'strict WARP; mandatory Debug Layer/InfoQueue; synthetic neural inputs; NGX NotRun'
        sourceCommit = (& git --no-optional-locks -C $root rev-parse HEAD)
        currentShaderSha256 = $before[$shader]
        baselineShaderSha256 = $(if ($BaselineShaderPath) { $before[$BaselineShaderPath] } else { $null })
        roiFixture = $RoiFixture; readbacks = $readbacks
        rawReadbackRetention = 'Temporary; normal lifecycle cleanup unless NR_NEXUS_TEST_KEEP_RAW=1'
        compiler = (Get-Command cl.exe).Source
        shaderCompiler = (Get-Item (Join-Path $env:WINDIR 'System32/d3dcompiler_47.dll')).VersionInfo.FileVersion
        executableSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    $outcome = 'Passed'
    Write-Host "Look transport strict-WARP regression passed: $OutputDirectory"
} catch {
    [ordered]@{ status = 'Failed'; error = $_.Exception.Message; evidence = 'WARP only; no NGX/game' } |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    throw
} finally {
    foreach ($file in @(Get-ChildItem -LiteralPath (Join-Path $OutputDirectory 'roi') -Filter '*.f32' -File -Recurse -ErrorAction SilentlyContinue)) {
        Register-TestDisposable -Directory $OutputDirectory -Path $file.FullName -Kind 'raw-readback'
    }
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
