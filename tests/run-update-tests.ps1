[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$OutputDirectory,
    [string]$VcVars64,
    [string]$PythonExecutable,
    [ValidateSet('Auto', 'Off', 'On')][string]$DevControlMode = 'Auto',
    [switch]$BuildOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot 'test-support.ps1')
. (Join-Path $SourceRoot 'tools/game-test/resolve-python.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $SourceRoot ('tmp/tests/update-' + [guid]::NewGuid().ToString('N'))
}
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$result = [ordered]@{ suite='update'; build='failed'; run='not_run'; evidence='CPU, WinHTTP exact loopback' }
$outcome = 'Failed'
try {
    Initialize-TestBuild $VcVars64
    $compiler = (Get-Command cl.exe -CommandType Application).Path
    $explicitMode = $DevControlMode -ne 'Auto'
    if ($explicitMode) {
        if ($env:CL -match 'NR_NEXUS_DEV_CONTROL\b' -or $env:_CL_ -match 'NR_NEXUS_DEV_CONTROL\b') {
            throw 'Explicit update DEV mode conflicts with CL or _CL_.'
        }
        . (Join-Path $SourceRoot 'tools/check-release-experiment-parity.ps1') -SourceRoot $SourceRoot
        $parityInputs = Get-ReleaseParityBuildContext -SourceRoot $SourceRoot -HostRoot $HostRoot
    }
    $source = Join-Path $PSScriptRoot 'update_tests.cpp'
    $serverScript = Join-Path $PSScriptRoot 'update_loopback.py'
    $compileInputs = @($source, $serverScript, $PSCommandPath,
        (Join-Path $SourceRoot 'update.hpp'), (Join-Path $SourceRoot 'http_winhttp.hpp'))
    $inputHashes = @{}
    foreach ($file in $compileInputs) { $inputHashes[$file] = (Get-FileHash -LiteralPath $file).Hash }
    $argumentsBase = @('/nologo','/std:c++20','/EHsc','/W4','/WX','/MT','/O2','/utf-8','/DNOMINMAX',
        "/I$SourceRoot", "/I$HostRoot/external/json/include", $source)
    if ($DevControlMode -eq 'On') { $argumentsBase += '/DNR_NEXUS_DEV_CONTROL' }
    if ($DevControlMode -eq 'Off') { $argumentsBase += '/UNR_NEXUS_DEV_CONTROL' }
    $exe = Join-Path $OutputDirectory 'update_tests.exe'
    $fixture = Join-Path $OutputDirectory 'update_worker_fixture.dll'
    $dependencies = "$OutputDirectory/dependencies.json"
    $argumentsPath = Join-Path $OutputDirectory 'arguments.json'
    $inputsPath = Join-Path $OutputDirectory 'inputs.json'
    $arguments = $argumentsBase + @("/Fo$OutputDirectory/update_tests.obj", "/Fe$exe",
        '/sourceDependencies', $dependencies, '/link','/INCREMENTAL:NO','/DELAYLOAD:winhttp.dll')
    $arguments | ConvertTo-Json | Set-Content -LiteralPath $argumentsPath -Encoding utf8
    Register-TestCompilerOutput $OutputDirectory -ObjectPath "$OutputDirectory/update_tests.obj" -ExecutablePath $exe
    Register-TestCompilerOutput $OutputDirectory -ObjectPath "$OutputDirectory/update_worker_fixture.obj" -ExecutablePath $fixture
    & $compiler @arguments *> "$OutputDirectory/compile.log"
    if ($LASTEXITCODE -ne 0) {
        Get-Content "$OutputDirectory/compile.log" -Tail 55
        throw 'Update test EXE compilation failed.'
    }
    $fixtureArguments = $argumentsBase + @('/LD', '/DNR_UPDATE_TEST_DLL',
        "/Fo$OutputDirectory/update_worker_fixture.obj", "/Fe$fixture", '/link','/INCREMENTAL:NO','/DELAYLOAD:winhttp.dll')
    & $compiler @fixtureArguments *> "$OutputDirectory/fixture-compile.log"
    if ($LASTEXITCODE -ne 0) {
        Get-Content "$OutputDirectory/fixture-compile.log" -Tail 55
        throw 'Update worker fixture DLL compilation failed.'
    }
    $closure = (Get-Content -LiteralPath $dependencies -Raw | ConvertFrom-Json).Data
    @(($compileInputs + @($closure.Source) + @($closure.Includes)) | Sort-Object -Unique | ForEach-Object {
        [ordered]@{ path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash }
    }) | ConvertTo-Json | Set-Content -LiteralPath $inputsPath -Encoding utf8
    $result.build = 'passed'
    $exeHash = (Get-FileHash -LiteralPath $exe).Hash
    $result['workerFixtureSha256'] = (Get-FileHash -LiteralPath $fixture).Hash
    if ($explicitMode) {
        $result['buildIdentity'] = [ordered]@{
            mode=$DevControlMode; source=$parityInputs; compiler=$compiler
            compilerSha256=(Get-FileHash -LiteralPath $compiler).Hash
            compilerEnvironment=@{CL=$env:CL; _CL_=$env:_CL_}
            argumentsSha256=(Get-FileHash -LiteralPath $argumentsPath).Hash
            inputsSha256=(Get-FileHash -LiteralPath $inputsPath).Hash
            dependenciesSha256=(Get-FileHash -LiteralPath $dependencies).Hash
            executableSha256=$exeHash; executable=[IO.Path]::GetFileName($exe)
            softwareOnly=$true; cases=@('unit','loopback')
        }
    }
    if (-not $BuildOnly) {
        $result.run = 'failed'
        Invoke-TestProcess $exe $OutputDirectory 'unit' @('--unit') -DeferFailureCleanup
        if (-not $PythonExecutable) {
            $PythonExecutable = Resolve-NrNexusPython -RepositoryRoot $SourceRoot -RequiredImports @()
        } elseif (-not (Test-NrNexusPython $PythonExecutable @())) {
            throw "Python interpreter is not runnable: $PythonExecutable"
        }
        $readyPath = Join-Path $OutputDirectory 'loopback-ready.txt'
        $logPath = Join-Path $OutputDirectory 'loopback-requests.jsonl'
        $serverParameters = @{
            FilePath=$PythonExecutable; WindowStyle='Hidden'; PassThru=$true
            ArgumentList=@('-u', '-B', ('"' + $serverScript + '"'), '--ready', ('"' + $readyPath + '"'), '--log', ('"' + $logPath + '"'))
            RedirectStandardOutput=(Join-Path $OutputDirectory 'loopback-server.stdout.log')
            RedirectStandardError=(Join-Path $OutputDirectory 'loopback-server.stderr.log')
        }
        $downloads = Join-Path $OutputDirectory 'download-fixtures'
        $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User
        $denyWrite = [Security.AccessControl.FileSystemAccessRule]::new($userSid,
            [Security.AccessControl.FileSystemRights]::CreateFiles, [Security.AccessControl.AccessControlType]::Deny)
        $denyDeleteChild = [Security.AccessControl.FileSystemAccessRule]::new($userSid,
            [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles, [Security.AccessControl.AccessControlType]::Deny)
        $denyDeleteFile = [Security.AccessControl.FileSystemAccessRule]::new($userSid,
            [Security.AccessControl.FileSystemRights]::Delete, [Security.AccessControl.InheritanceFlags]::ObjectInherit,
            [Security.AccessControl.PropagationFlags]::InheritOnly, [Security.AccessControl.AccessControlType]::Deny)
        $originalAcls = @{}
        $server = $null
        try {
            foreach ($directory in @{ 'denied'=@($denyWrite); 'denied-delete'=@($denyDeleteChild, $denyDeleteFile) }.GetEnumerator()) {
                $path = (New-Item -ItemType Directory -Path (Join-Path $downloads $directory.Key)).FullName
                $originalAcls[$path] = Get-Acl -LiteralPath $path
                $acl = Get-Acl -LiteralPath $path
                foreach ($rule in $directory.Value) { $acl.AddAccessRule($rule) }
                Set-Acl -LiteralPath $path -AclObject $acl
            }
            $server = Start-Process @serverParameters
            $deadline = [DateTime]::UtcNow.AddSeconds(10)
            while (-not (Test-Path -LiteralPath $readyPath)) {
                if ($server.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Update loopback did not start.' }
                Start-Sleep -Milliseconds 50
            }
            $portText = (Get-Content -LiteralPath $readyPath -Raw).Trim()
            if ($portText -notmatch '^[0-9]{1,5}$' -or [int]$portText -lt 1 -or [int]$portText -gt 65535) {
                throw 'Invalid loopback port.'
            }
            Invoke-TestProcess $exe $OutputDirectory 'loopback' @('--loopback', $portText) -DeferFailureCleanup
        } finally {
            if ($server) {
                if (-not $server.HasExited) { $server.Kill(); $server.WaitForExit() }
                $server.Dispose()
            }
            foreach ($acl in $originalAcls.GetEnumerator()) { Set-Acl -LiteralPath $acl.Key -AclObject $acl.Value }
            if (Test-Path -LiteralPath $downloads) {
                Get-ChildItem -LiteralPath $downloads -Recurse -File | ForEach-Object {
                    Register-TestDisposable $OutputDirectory -Path $_.FullName -Kind 'synthetic-update-files'
                }
            }
        }
        $requests = @(Get-Content -LiteralPath $logPath | ForEach-Object { $_ | ConvertFrom-Json })
        $reuseChecks = @($requests | Where-Object path -eq '/check/reuse')
        if ($reuseChecks.Count -ne 3 -or $null -ne $reuseChecks[0].etag -or $reuseChecks[1].etag -cne '"release"' -or
            $reuseChecks[0].agent -cnotmatch '^NR Nexus/' -or $reuseChecks[0].accept -cne 'application/vnd.github+json' -or
            $reuseChecks[0].api_version -cne '2022-11-28') {
            throw 'Release API request contract did not match.'
        }
        $weak = @($requests | Where-Object path -eq '/check/etag-weak')
        $wildcard = @($requests | Where-Object path -eq '/check/etag-wildcard')
        if ($weak.Count -ne 2 -or $null -ne $weak[0].etag -or $weak[1].etag -cne 'W/"weak"' -or
            $wildcard.Count -ne 3 -or $null -ne $wildcard[0].etag -or $wildcard[1].etag -cne '"before-wildcard"' -or
            $null -ne $wildcard[2].etag) {
            throw 'Weak and wildcard ETag request sequences did not match.'
        }
        foreach ($path in '/check/etag-list','/check/etag-duplicate') {
            $calls = @($requests | Where-Object path -eq $path)
            if ($calls.Count -ne 2 -or @($calls | Where-Object { $null -ne $_.etag }).Count) {
                throw "Invalid ETag forwarded to server: $path"
            }
        }
        foreach ($entry in @{
            '/download/reuse'=1; '/download/redirected'=1; '/download/race'=2;
            '/download/redirect-loop'=6; '/download/recover'=2; '/check/retry429'=1;
            '/check/reset403'=1; '/check/pinned'=1
        }.GetEnumerator()) {
            if (@($requests | Where-Object path -eq $entry.Key).Count -ne $entry.Value) {
                throw "Unexpected loopback request count: $($entry.Key)"
            }
        }
        if (@($requests | Where-Object { $_.path -match 'should-not-reach|/download/existing-conflict' -or $_.cookie }).Count) {
            throw 'Unexpected redirect, download or cookie observed on the loopback server.'
        }
        Write-Host 'NR_RELEASE_PARITY online-update winhttp_request_contract=passed'
        $result.run = 'passed'
    }
    foreach ($file in $compileInputs) {
        if ((Get-FileHash -LiteralPath $file).Hash -cne $inputHashes[$file]) { throw "Update input changed: $file" }
    }
    if ((Get-FileHash -LiteralPath $exe).Hash -cne $exeHash) { throw 'Update test EXE changed.' }
    $outcome = 'Passed'
    Write-Host "Update CPU/loopback: $($result.run). Evidence: $OutputDirectory"
} finally {
    $result | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
