$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$wrapper = Join-Path $repo 'tools/run-fg-acceptance.ps1'
$tokens = $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($wrapper, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Set-FgAcceptanceConfigFixture','Get-FgAcceptanceUpdateEvidence','Restore-FgAcceptanceConfigFixture')) {
    $function = $ast.Find({param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
    }, $true)
    if (-not $function) { throw "Missing production function $name" }
    . ([scriptblock]::Create($function.Extent.Text))
}

foreach ($session in @(@(), @('-RestoreInstallation','-InstallOnly'))) {
    $rejection = & pwsh.exe -NoProfile -File $wrapper -CandidateManifest 'unused-candidate' `
        -GameBinaryDirectory 'unused-game' -UpdatePreferencesPath 'unused-prefs.json' @session 2>&1
    if ($LASTEXITCODE -eq 0 -or ($rejection -join "`n") -notmatch 'requires -RestoreInstallation and a game run') {
        throw 'Update preferences must reject an unprotected or install-only session before installation.'
    }
}

$root = Join-Path $repo ('tmp/online-update-writable-config-test-' + [guid]::NewGuid().ToString('N'))
$game = Join-Path $root 'game'
$backup = Join-Path $root 'acceptance-synthetic/backup'
$config = Join-Path $game 'ReShade.ini'
$original = @{path=$config; backup=(Join-Path $backup 'ReShade.ini')}
$lock = $null
try {
    [void][IO.Directory]::CreateDirectory($game)
    [void][IO.Directory]::CreateDirectory($backup)
    $originalBytes = [Text.Encoding]::UTF8.GetBytes("[Original]`r`nPrivateValue=synthetic`r`n")
    [IO.File]::WriteAllBytes($config, $originalBytes)
    Copy-Item -LiteralPath $config -Destination $original.backup
    $original.sha256 = (Get-FileHash -LiteralPath $config).Hash
    $lock = [IO.File]::Open($original.backup, 'Open', 'Read', 'Read')

    $update = "[RenoDX.DLSS5.Update]`r`nAutoCheck=0`r`nDownloadDirectory=`r`nNextAutoCheckUtc=0`r`n"
    $initial = "[Original]`r`nPrivateValue=temporary`r`n" + $update + "[Other]`r`nValue=1`r`n"
    Set-FgAcceptanceConfigFixture $config ([Text.Encoding]::UTF8.GetBytes($initial))
    $savedUpdate = $update.Replace('NextAutoCheckUtc=0', 'NextAutoCheckUtc=1234')
    [IO.File]::WriteAllText($config, $initial.Replace($update, $savedUpdate))
    $evidence = Get-FgAcceptanceUpdateEvidence ([IO.File]::ReadAllBytes($config))
    if ($evidence.update_section -cne $savedUpdate -or $evidence.final_sha256 -ne (Get-FileHash -LiteralPath $config).Hash) {
        throw 'Evidence must retain the exact saved update section and hash of the same complete config.'
    }
    $backupWriteDenied = $false
    try { [IO.File]::WriteAllText($original.backup, 'must be denied') }
    catch [IO.IOException] { $backupWriteDenied = ($_.Exception.HResult -band 0xFFFF) -eq 32 }
    if (-not $backupWriteDenied) { throw 'Original config backup was writable before restoration.' }

    $mismatched = $original.Clone()
    $mismatched.sha256 = '0' * 64
    $restoreRejected = $false
    try { Restore-FgAcceptanceConfigFixture $mismatched }
    catch { $restoreRejected = $_.Exception.Message -eq 'Update preferences restoration backup hash mismatch' }
    if (-not $restoreRejected -or (Get-FileHash -LiteralPath $config).Hash -ne $evidence.final_sha256) {
        throw 'Restoration modified the temporary config with an unverified backup.'
    }

    Restore-FgAcceptanceConfigFixture $original
    if ((Get-FileHash -LiteralPath $config).Hash -ne $original.sha256 -or
        (Get-FileHash -LiteralPath $original.backup).Hash -ne $original.sha256) {
        throw 'The temporary write did not round-trip to the original config hash.'
    }
} finally {
    if ($lock) { $lock.Dispose() }
    foreach ($path in @($config,$original.backup)) { [IO.File]::Delete($path) }
    foreach ($path in @($game,$backup,(Split-Path -Parent $backup),$root)) {
        if ([IO.Directory]::Exists($path)) { [IO.Directory]::Delete($path) }
    }
}
Write-Output 'Online update writable config tests passed'
