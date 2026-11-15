[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/ci-tests/publishing-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../tools/check-publishing.ps1') -OutputDirectory $OutputDirectory
. (Join-Path $PSScriptRoot 'test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$cache = Join-Path $OutputDirectory 'CMakeCache.txt'
$command = Join-Path $OutputDirectory 'dlss5.dir/Release/dlss5.tlog/CL.command.1.tlog'
$binary = Join-Path $OutputDirectory 'synthetic.addon64'
[void][IO.Directory]::CreateDirectory((Split-Path $command -Parent))
$checks = [Collections.Generic.List[string]]::new()
$validCommand = "^C:\fixture\addon.cpp`r`n/c /D NDEBUG /Foobj fixture.cpp`r`n"
function Reset-Fixture {
    [IO.File]::WriteAllText($cache, "NR_NEXUS_DEV_CONTROL:BOOL=OFF`n")
    [IO.File]::WriteAllText($command, $validCommand, [Text.Encoding]::Unicode)
    [IO.File]::WriteAllBytes($binary, [byte[]]@(0,1,2,3))
}
function Assert-Rejected([string]$Name, [string]$Pattern) {
    try { Assert-PublishingIsolation -BuildDirectory $OutputDirectory -BinaryPath $binary | Out-Null }
    catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "Wrong rejection ${Name}: $($_.Exception.Message)" }
        $checks.Add($Name)
        return
    }
    throw "Publishing isolation accepted incorrectly: $Name"
}
$outcome='Failed'
try {
    Reset-Fixture
    foreach ($file in @($cache,$command,$binary)) { Register-TestDisposable $OutputDirectory $file 'synthetic-input' }
    $result = Assert-PublishingIsolation -BuildDirectory $OutputDirectory -BinaryPath $binary
    if ($result.DllSha256 -cne (Get-FileHash -LiteralPath $binary).Hash.ToLowerInvariant()) { throw 'DLL hash changed.' }
    $checks.Add('clean Publishing isolation and hash')
    foreach ($value in @('ON',('OFF' + "`nNR_NEXUS_DEV_CONTROL:BOOL=ON"),'')) {
        Reset-Fixture
        [IO.File]::WriteAllText($cache, "NR_NEXUS_DEV_CONTROL:BOOL=$value`n")
        Assert-Rejected "invalid cache $($checks.Count)" 'requires NR_NEXUS_DEV_CONTROL'
    }
    foreach ($define in @('/DNR_NEXUS_DEV_CONTROL','/D NR_NEXUS_DEV_CONTROL=1','/D "NR_NEXUS_DEV_CONTROL=1"')) {
        Reset-Fixture
        [IO.File]::WriteAllText($command, $validCommand + $define, [Text.Encoding]::Unicode)
        Assert-Rejected "compiler definition $define" 'compiler command contains'
    }
    Reset-Fixture
    [IO.File]::WriteAllText($command, '/c unrelated.cpp', [Text.Encoding]::Unicode)
    Assert-Rejected 'missing actual addon compiler command' 'actual Release addon.cpp'
    foreach ($marker in @('inline_restore_roi','inline_restore_roi_v1','mafia-same-frame-roi','unsupported_inline_restore_contract')) {
        foreach ($encoding in @([Text.Encoding]::ASCII,[Text.Encoding]::Unicode)) {
            Reset-Fixture
            [IO.File]::WriteAllBytes($binary, ([byte[]]@(7) + $encoding.GetBytes($marker) + [byte[]]@(9)))
            Assert-Rejected "$marker $($encoding.WebName)" 'development ROI marker'
        }
    }
    $outcome='Passed'
} finally {
    @{status=$outcome;checks=@($checks)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
Write-Host "CI Publishing isolation tests passed: $($checks.Count)"
