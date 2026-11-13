param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory,
    [string]$VcVars64,
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $SourceRoot ('tmp/dev-control-pipe-tests/' + [guid]::NewGuid().ToString('N') + '/run')
}
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$outcome = 'Failed'
try {
    Initialize-TestBuild $VcVars64
    $inputs = @((Join-Path $SourceRoot 'dev_control_pipe.hpp'),
        (Join-Path $PSScriptRoot 'dev_control_pipe_tests.cpp'), $PSCommandPath,
        (Join-Path $PSScriptRoot 'test-support.ps1'))
    $hashes = @($inputs | ForEach-Object {
        [ordered]@{ path=$_; sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
    })
    $hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
    Push-Location -LiteralPath $OutputDirectory
    try {
        foreach ($target in @(
            @{ name='dev_control_pipe_fixture'; dll=$true },
            @{ name='dev_control_pipe_tests'; dll=$false }
        )) {
            $binary = $target.name + $(if ($target.dll) { '.dll' } else { '.exe' })
            $arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/MT', '/utf-8',
                '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', '/DNR_NEXUS_DEV_CONTROL=1',
                "/I$SourceRoot", "/I$HostRoot/external/json/include",
                (Join-Path $PSScriptRoot 'dev_control_pipe_tests.cpp'), "/Fo$($target.name).obj", "/Fe$binary",
                '/sourceDependencies', ($target.name + '.dependencies.json'))
            if ($target.dll) { $arguments += @('/LD', '/DNR_NEXUS_DEV_CONTROL_PIPE_TEST_DLL=1') }
            $arguments += @('/link', '/INCREMENTAL:NO', 'advapi32.lib')
            $arguments | ConvertTo-Json | Set-Content -LiteralPath ($target.name + '.arguments.json') -Encoding utf8
            Register-TestCompilerOutput -Directory $OutputDirectory -ObjectPath (Join-Path $OutputDirectory ($target.name + '.obj')) -ExecutablePath (Join-Path $OutputDirectory $binary)
            if (-not $BuildOnly) { Register-TestDisposable -Directory $OutputDirectory -Path (Join-Path $OutputDirectory $binary) -Kind 'test-binary' }
            & cl.exe @arguments *> ($target.name + '.compile.log')
            if ($LASTEXITCODE -ne 0) {
                Get-Content -LiteralPath ($target.name + '.compile.log') -Tail 60
                throw "Pipe target compilation failed: $($target.name)"
            }
        }
        $dependencies = Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.dependencies.json' -File |
            ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).Data.Includes } |
            Sort-Object -Unique
        $dependencyHashes = @($dependencies | ForEach-Object {
            [ordered]@{ path=$_; sha256=(Get-FileHash -LiteralPath $_ -Algorithm SHA256).Hash }
        })
        $dependencyHashes | ConvertTo-Json | Set-Content -LiteralPath 'dependency-inputs.json' -Encoding utf8
        Get-ChildItem -LiteralPath $OutputDirectory -File | Where-Object { $_.Extension -in '.dll', '.exe' } |
            ForEach-Object { [ordered]@{ path=$_.FullName; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash } } |
            ConvertTo-Json | Set-Content -LiteralPath 'outputs.json' -Encoding utf8
        if (-not $BuildOnly) {
            Invoke-TestProcess -Executable (Join-Path $OutputDirectory 'dev_control_pipe_tests.exe') -Directory $OutputDirectory -Name 'pipe-tests' -TimeoutMilliseconds 30000
        }
        foreach ($inputHash in @($hashes) + @($dependencyHashes)) {
            if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) {
                throw "Input changed during pipe tests: $($inputHash.path)"
            }
        }
        $outcome = 'Passed'
        Write-Host "Dev control pipe CPU tests: $outcome; BuildOnly=$BuildOnly; $OutputDirectory"
    } finally {
        Pop-Location
    }
} finally {
    if ($BuildOnly -and $outcome -ne 'Passed') {
        Get-ChildItem -LiteralPath $OutputDirectory -File | Where-Object { $_.Extension -in '.dll', '.exe' } |
            ForEach-Object { Register-TestDisposable -Directory $OutputDirectory -Path $_.FullName -Kind 'failed-build-binary' }
    }
    Complete-TestOutput -Directory $OutputDirectory -Outcome $outcome
}
