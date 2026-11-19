[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OrdinaryLog,
    [Parameter(Mandatory)][string]$XefgLog
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-PresentFrameTrace([string]$Path, [string]$Route) {
    $rows = @(Get-Content -LiteralPath $Path | Where-Object { $_ -match '^\s*PRESENT_POLICY_BASE\b' })
    if ($rows.Count -ne 4) {
        throw "Expected four Present base frames for $Route in $Path, found $($rows.Count)."
    }
    for ($index = 0; $index -lt $rows.Count; $index++) {
        $base = $index + 1
        $pattern = "^PRESENT_POLICY_BASE route=$Route (base=$base calls=[0-9]+ reset=-?[0-9]+ history=[0-9]+ " +
            'anchor_base=[0-9]+ anchor_generation=[0-9]+ anchor_age=[0-9]+ complete=[01] rgba_crc32=[0-9]+)$'
        $row = [regex]::Match($rows[$index], $pattern)
        if (-not $row.Success) {
            throw "Expected a complete Present trace for $Route base frame $base in $Path. Found '$($rows[$index])'."
        }
        $row.Groups[1].Value
    }
}

$ordinary = @(Read-PresentFrameTrace $OrdinaryLog 'ordinary')
$xefg = @(Read-PresentFrameTrace $XefgLog 'xefg')
for ($index = 0; $index -lt $ordinary.Count; $index++) {
    if ($ordinary[$index] -cne $xefg[$index]) {
        $base = $index + 1
        throw "Present traces differ at base frame $base. Ordinary [$($ordinary[$index])], XeFG [$($xefg[$index])]."
    }
}
'PASS present-frame-traces routes=ordinary,xefg frames=4 rgba_crc32=matched'
