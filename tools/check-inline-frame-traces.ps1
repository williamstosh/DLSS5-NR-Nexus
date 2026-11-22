[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$PublicLog,
    [Parameter(Mandatory)][string]$BridgeLog
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Read-InlineFrameTraces([string]$Path, [string[]]$Routes) {
    $traces = @{}
    foreach ($route in $Routes) { $traces[$route] = @() }
    $pattern = '^INLINE_POLICY_BASE route=(?<route>dx12|foreign|bridge) (?<frame>base=(?<base>[1-4]) ' +
        'plan_stage=Upscaled pass_begin=0 pass_count=1 model=2 calls=[0-9]+ reset=(?:-|[0-9]+) history=[0-9]+ ' +
        'anchor_base=[0-9]+ anchor_generation=[0-9]+ anchor_age=[0-9]+ complete=[01] rgba_crc32=[0-9]+)$'
    foreach ($line in (Get-Content -LiteralPath $Path)) {
        if ($line -notmatch '^\s*INLINE_POLICY_BASE\b') { continue }
        $row = [regex]::Match($line, $pattern)
        $route = $row.Groups['route'].Value
        if (-not $row.Success -or $route -cnotin $Routes) {
            throw "Unexpected inline trace in $Path. Found '$line'."
        }
        $base = $traces[$route].Count + 1
        if ($row.Groups['base'].Value -cne "$base") {
            throw "Expected inline trace for $route base frame $base in $Path. Found '$line'."
        }
        $traces[$route] += $row.Groups['frame'].Value
    }
    foreach ($route in $Routes) {
        if ($traces[$route].Count -ne 4) {
            throw "Expected four inline base frames for $route in $Path, found $($traces[$route].Count)."
        }
    }
    return $traces
}

$public = Read-InlineFrameTraces $PublicLog @('dx12','foreign')
$bridge = Read-InlineFrameTraces $BridgeLog @('bridge')
for ($index = 0; $index -lt $public.dx12.Count; $index++) {
    if ($public.dx12[$index] -cne $public.foreign[$index] -or $public.dx12[$index] -cne $bridge.bridge[$index]) {
        $base = $index + 1
        throw "Inline traces differ at base frame $base. DX12 [$($public.dx12[$index])], foreign [$($public.foreign[$index])], Bridge [$($bridge.bridge[$index])]."
    }
}
'PASS inline-frame-traces routes=dx12,foreign,bridge frames=4 rgba_crc32=matched'
