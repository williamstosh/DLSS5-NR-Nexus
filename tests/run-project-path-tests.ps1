[CmdletBinding()]
param([string]$Python)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $root 'tools/project-paths.ps1')
. (Join-Path $root 'tools/game-test/resolve-python.ps1')
. (Join-Path $PSScriptRoot 'test-support.ps1')
function Assert-PathRejected([scriptblock]$Action, [string]$Expected) {
    try { & $Action | Out-Null }
    catch {
        if ($_.Exception.Message -like $Expected) { return }
        throw
    }
    throw "Path check should have rejected: $Expected"
}
function Assert-Equal($Actual, $Expected) {
    if ($Actual -ine $Expected) { throw "Expected $Expected, got $Actual" }
}

$out = Resolve-NexusProjectPath -Root $root -Path ('tmp/path-test-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($out)
$previousCwd = [Environment]::CurrentDirectory
$previousTemp = @($env:TEMP, $env:TMP, $env:TMPDIR)
$link = Join-Path $out 'link'
$target = Join-Path $out 'link-target'
try {
    # Resolve relative paths from checkout even when .NET cwd differs.
    [Environment]::CurrentDirectory = $out
    Assert-Equal (Resolve-NexusBuildDirectory -Root $root) (Join-Path $root 'build')
    Assert-Equal (Resolve-NexusBuildDirectory -Root $root -Path 'build/dev-control' -DevControl) (Join-Path $root 'build/dev-control')
    Assert-Equal (Resolve-NexusBuildDirectory -Root $root -Path 'tmp/fixture-build') (Join-Path $root 'tmp/fixture-build')
    Assert-PathRejected { Resolve-NexusBuildDirectory -Root $root -Path 'build/dev-control' } '*fixed build tree*'
    Assert-PathRejected { Resolve-NexusBuildDirectory -Root $root -Path 'build' -DevControl } '*fixed build tree*'
    Assert-PathRejected { Resolve-NexusBuildDirectory -Root $root -DevControl } '*explicit separate*'
    Assert-PathRejected { & (Join-Path $root 'tools/build.ps1') -ToolRoot $out -BuildDirectory '../outside' } '*below this checkout*'
    Assert-PathRejected { Resolve-NexusBuildDirectory -Root $root -Path ($root + '-sibling/build') } '*below this checkout*'
    Assert-PathRejected { Initialize-TestOutput -Path '../outside' } '*below this checkout*'
    Assert-PathRejected { Initialize-TestOutput -Path 'build/fixture' } '*below this repository*'
    Assert-Equal (Resolve-NexusBuildTemp -Root $root -Preferred ($root + '-sibling/tmp')) (Join-Path $root 'tmp/build-process')
    Assert-Equal (Resolve-NexusBuildTemp -Root $root -Preferred $out) $out

    [void][IO.Directory]::CreateDirectory($target)
    New-Item -ItemType Junction -Path $link -Target $target | Out-Null
    Assert-PathRejected { Resolve-NexusBuildDirectory -Root $root -Path (Join-Path $link 'not-created') } '*reparse point*'
    Assert-PathRejected { Resolve-NexusBuildTemp -Root $root -Preferred $link } '*reparse point*'
    Assert-PathRejected { Initialize-TestOutput -Path (Join-Path $link 'not-created') } '*reparse point*'
    Remove-Item -LiteralPath $target
    Assert-PathRejected { Resolve-NexusBuildDirectory -Root $root -Path (Join-Path $link 'dangling') } '*reparse point*'
    if ($Python) {
        $env:TMPDIR = $out
        $env:TEMP = 'outside-sentinel'
        $env:TMP = 'other-sentinel'
        $actual = Resolve-NrNexusPython -RequestedPath $Python -RepositoryRoot $root -RequiredImports @()
        Assert-Equal $actual $Python
        Assert-Equal $env:TEMP 'outside-sentinel'
        Assert-Equal $env:TMP 'other-sentinel'
        Assert-Equal $env:TMPDIR $out
        Write-Host "PASS actual Python cwd/Windows TEMP/TMP/TMPDIR at D test scope; parent values restored."
    }
} finally {
    [Environment]::CurrentDirectory = $previousCwd
    $env:TEMP, $env:TMP, $env:TMPDIR = $previousTemp
    $item = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
    if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { Remove-Item -LiteralPath $link -Force }
    Remove-Item -LiteralPath $out
}
Write-Host 'PASS fixed ordinary and DEV builds, explicit tmp exception, external/output rejection, existing and dangling junction rejection, local build temp.'
