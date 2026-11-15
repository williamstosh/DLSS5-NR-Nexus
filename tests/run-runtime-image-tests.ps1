[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$OutputDirectory,
    [string]$VcVars64,
    [string]$RuntimePath
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $SourceRoot ('tmp/tests/runtime-image-' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$outcome = 'Failed'
$previousTemp = $env:TEMP
$previousTmp = $env:TMP
try {
    $env:TEMP = $env:TMP = Join-Path $OutputDirectory 'compiler-temp'
    [void][IO.Directory]::CreateDirectory($env:TEMP)
    Initialize-TestBuild $VcVars64
    $inputPaths = @(
        (Join-Path $SourceRoot 'runtime_image.hpp'),
        (Join-Path $PSScriptRoot 'native_runtime_fixture.cpp'),
        (Join-Path $PSScriptRoot 'runtime_image_tests.cpp'),
        $PSCommandPath,
        (Join-Path $PSScriptRoot 'test-support.ps1')
    )
    if ($RuntimePath) {
        $RuntimePath = (Resolve-Path -LiteralPath $RuntimePath).Path
        $inputPaths += $RuntimePath
    }
    $inputs = @($inputPaths | ForEach-Object {
        [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash }
    })
    $inputs | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $OutputDirectory 'inputs.json') -Encoding utf8
    Push-Location -LiteralPath $OutputDirectory
    try {
        foreach ($target in @(
            @{ source = 'native_runtime_fixture'; binary = 'nvngx_dlssnr.dll'; dll = $true },
            @{ source = 'runtime_image_tests'; binary = 'runtime_image_tests.exe'; dll = $false }
        )) {
            $arguments = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/O2', '/MT', '/utf-8',
                '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN', "/I$SourceRoot")
            if ($target.dll) { $arguments += '/LD' }
            $arguments += @((Join-Path $PSScriptRoot ($target.source + '.cpp')),
                ('/Fo' + $target.source + '.obj'), ('/Fe' + $target.binary),
                '/sourceDependencies', ($target.source + '.dependencies.json'),
                '/link', '/INCREMENTAL:NO', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib', 'uuid.lib')
            Register-TestCompilerOutput -Directory $OutputDirectory `
                -ObjectPath (Join-Path $OutputDirectory ($target.source + '.obj')) `
                -ExecutablePath (Join-Path $OutputDirectory $target.binary)
            $arguments | ConvertTo-Json | Set-Content -LiteralPath ($target.source + '.arguments.json') -Encoding utf8
            & cl.exe @arguments *> ($target.source + '.compile.log')
            if ($LASTEXITCODE -ne 0) {
                Get-Content -LiteralPath ($target.source + '.compile.log') -Tail 35
                throw "Runtime image fixture compilation failed: $($target.source)."
            }
        }
        $dependencies = @(Get-ChildItem -LiteralPath $OutputDirectory -Filter '*.dependencies.json' -File |
            ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).Data.Includes } |
            Sort-Object -Unique | ForEach-Object {
                [ordered]@{ path = $_; sha256 = (Get-FileHash -LiteralPath $_).Hash }
            })
        $dependencies | ConvertTo-Json | Set-Content -LiteralPath 'dependency-inputs.json' -Encoding utf8
        Get-ChildItem -LiteralPath $OutputDirectory -File |
            Where-Object { $_.Extension -in @('.dll', '.exe') } | ForEach-Object {
                [ordered]@{ path = $_.FullName; sha256 = (Get-FileHash -LiteralPath $_.FullName).Hash }
            } | ConvertTo-Json | Set-Content -LiteralPath 'outputs.json' -Encoding utf8
        Invoke-TestProcess -Executable (Join-Path $OutputDirectory 'runtime_image_tests.exe') `
            -Directory $OutputDirectory -Name 'runtime-image' -TimeoutMilliseconds 45000 -DeferFailureCleanup
        if ($RuntimePath) {
            Invoke-TestProcess -Executable (Join-Path $OutputDirectory 'runtime_image_tests.exe') `
                -Directory $OutputDirectory -Name 'runtime-image-load-only' -Arguments @(('"' + $RuntimePath + '"')) `
                -TimeoutMilliseconds 45000 -DeferFailureCleanup
        }
        foreach ($taskInput in @($inputs) + @($dependencies)) {
            if ((Get-FileHash -LiteralPath $taskInput.path).Hash -ne $taskInput.sha256) {
                throw "Runtime image input changed during test: $($taskInput.path)."
            }
        }
        [ordered]@{ outcome = 'Passed'; evidence = @('Windows-loader', 'WARP', 'MOCK');
            vendor_runtime_load_only = [bool]$RuntimePath; real_ngx = 'NotRun'; in_game = 'NotRun';
            utc = [DateTime]::UtcNow.ToString('o') } |
            ConvertTo-Json | Set-Content -LiteralPath 'result.json' -Encoding utf8
        $outcome = 'Passed'
    } finally {
        Pop-Location
    }
} finally {
    # The test process has exited before its synthetic DLLs are registered.
    Get-ChildItem -LiteralPath $OutputDirectory -Recurse -File |
        Where-Object { $_.Extension -in @('.dll', '.exe') } | ForEach-Object {
            Register-TestDisposable -Directory $OutputDirectory -Path $_.FullName -Kind 'synthetic-runtime-image'
        }
    Complete-TestOutput $OutputDirectory -Outcome $outcome
    $env:TEMP = $previousTemp
    $env:TMP = $previousTmp
}
