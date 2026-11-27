# Resolve a real Python interpreter for local test entry points.
function Resolve-NrNexusLauncherTemp([string]$RepositoryRoot) {
    $root = (Resolve-Path -LiteralPath $RepositoryRoot -ErrorAction Stop).ProviderPath
    . (Join-Path $root 'tools/project-paths.ps1')
    $cwd = (Get-Location).ProviderPath
    if ($cwd -ine $root) { [void](Resolve-NexusProjectPath -Root $root -Path $cwd) }
    $tmp = Join-Path $root 'tmp'
    $temp = Join-Path $tmp 'normal-process/launcher-temp'
    if ($env:TMPDIR -and [IO.Path]::IsPathFullyQualified($env:TMPDIR)) {
        $requested = [IO.Path]::GetFullPath($env:TMPDIR).TrimEnd('\', '/')
        if ($requested.StartsWith($tmp + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            $temp = $requested
        }
    }
    $temp = Resolve-NexusProjectPath -Root $root -Path $temp
    [void][IO.Directory]::CreateDirectory($temp)
    return (Resolve-Path -LiteralPath $temp).ProviderPath
}

function Test-NrNexusPython {
    param(
        [Parameter(Mandatory)][string]$Path,
        [string[]]$RequiredImports = @('PIL')
    )
    try {
        $resolved = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
        if (-not (Test-Path -LiteralPath $resolved -PathType Leaf) -or
            $resolved.StartsWith((Join-Path $env:LOCALAPPDATA 'Microsoft/WindowsApps'), [StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
        $imports = ($RequiredImports | ForEach-Object { "import $_" }) -join '; '
        $probe = if ($imports) { "$imports; " } else { '' }
        $scopeCheck = @'
import ctypes, os, sys
cwd, temp = sys.argv[1:]
normalize = lambda value: os.path.normcase(os.path.normpath(value))
native = ctypes.create_unicode_buffer(32768)
size = ctypes.windll.kernel32.GetTempPathW(len(native), native)
if (not size or size >= len(native) or normalize(os.getcwd()) != normalize(cwd)
        or normalize(native.value) != normalize(temp)
        or any(not os.environ.get(k) or normalize(os.environ[k]) != normalize(temp)
               for k in ('TEMP', 'TMP', 'TMPDIR'))):
    raise SystemExit('Actual Python cwd/Windows temp do not match project preflight.')
'@
        & $resolved -B -c ($scopeCheck + [Environment]::NewLine + $probe + 'import sys; print(sys.executable)') ((Get-Location).ProviderPath) $env:TEMP *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

function Resolve-NrNexusPython {
    param(
        [string]$RequestedPath,
        [Parameter(Mandatory)][string]$RepositoryRoot,
        [string[]]$RequiredImports = @('PIL')
    )
    $temp = Resolve-NrNexusLauncherTemp $RepositoryRoot
    $previous = @($env:TEMP, $env:TMP, $env:TMPDIR)
    try {
        $env:TEMP = $env:TMP = $env:TMPDIR = $temp
        if ($RequestedPath) {
            $candidate = [IO.Path]::GetFullPath($RequestedPath)
            if (-not (Test-NrNexusPython $candidate $RequiredImports)) {
                throw "Python interpreter is missing, not runnable, or lacks required modules: $candidate. Pass -Python with a real interpreter path."
            }
            return $candidate
        }

        $candidates = @(
            (Join-Path $RepositoryRoot '.local/game-compare-venv/Scripts/python.exe'),
            (Join-Path $RepositoryRoot '.venv/Scripts/python.exe')
        )
        if ($env:NR_NEXUS_PYTHON) { $candidates += $env:NR_NEXUS_PYTHON }
        foreach ($name in @('python.exe', 'py.exe')) {
            $command = Get-Command $name -ErrorAction SilentlyContinue
            if ($command -and $command.Source) { $candidates += $command.Source }
        }
        foreach ($candidate in ($candidates | Select-Object -Unique)) {
            if (Test-NrNexusPython $candidate $RequiredImports) {
                return (Resolve-Path -LiteralPath $candidate).Path
            }
        }
        throw "No working Python interpreter with the required modules was found. Install or restore .local/game-compare-venv, or pass -Python with a real interpreter path; WindowsApps aliases are ignored."
    } finally {
        $env:TEMP, $env:TMP, $env:TMPDIR = $previous
    }
}
