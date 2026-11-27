# Resolve write paths from the checkout, not from .NET's possibly different
# process cwd. Never follow an output junction outside the selected project.
function Resolve-NexusProjectPath {
    param([string]$Root, [string]$Path)
    $rootPath = [IO.Path]::GetFullPath($Root).TrimEnd('\','/')
    $resolved = [IO.Path]::GetFullPath($Path, $rootPath).TrimEnd('\','/')
    if (-not $resolved.StartsWith($rootPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must stay below this checkout: $resolved"
    }
    # Visit ancestors first, including ancestors of the checkout. A dangling link
    # is still a reparse point even if Test-Path reports no target.
    $components = [Collections.Generic.Stack[string]]::new()
    for ($component = $resolved; $component; $component = [IO.Path]::GetDirectoryName($component)) {
        $components.Push($component)
    }
    foreach ($component in $components) {
        $item = $null
        try { $item = Get-Item -LiteralPath $component -Force -ErrorAction Stop }
        catch [System.Management.Automation.ItemNotFoundException] { continue }
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Path crosses a reparse point: $component"
        }
    }
    return $resolved
}

function Resolve-NexusBuildDirectory {
    param([string]$Root, [string]$Path, [switch]$DevControl)
    if ($DevControl -and -not $Path) {
        throw 'Development control requires an explicit separate -BuildDirectory.'
    }
    $resolved = Resolve-NexusProjectPath -Root $Root -Path $(if ($Path) { $Path } else { 'build' })
    $fixed = Join-Path $Root $(if ($DevControl) { 'build/dev-control' } else { 'build' })
    if ($resolved -ine $fixed -and
        -not $resolved.StartsWith((Join-Path $Root 'tmp') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Use the fixed build tree for this mode or an explicit exception below this project tmp/.'
    }
    return $resolved
}

function Resolve-NexusBuildTemp {
    param([string]$Root, [string]$Preferred)
    $tmp = Join-Path $Root 'tmp'
    if ($Preferred -and [IO.Path]::IsPathFullyQualified($Preferred)) {
        $candidate = [IO.Path]::GetFullPath($Preferred).TrimEnd('\','/')
        if ($candidate -ieq $tmp -or $candidate.StartsWith($tmp + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            return Resolve-NexusProjectPath -Root $Root -Path $candidate
        }
    }
    return Resolve-NexusProjectPath -Root $Root -Path 'tmp/build-process'
}
