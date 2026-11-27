$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$entry = Join-Path $repo 'tools/run-fg-acceptance.ps1'
. (Join-Path $repo 'tools/project-paths.ps1')

# Extract the production reader and transform without executing the game entry.
$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($entry, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count) { throw $parseErrors[0] }
foreach ($name in @('Read-FgAcceptanceUpdatePreferences','Set-FgAcceptanceUpdateSection')) {
    $definition = $ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true)
    if ($null -eq $definition) { throw "Missing production function: $name" }
    . ([scriptblock]::Create($definition.Extent.Text))
}

function Assert-Rejected([scriptblock]$Action, [string]$Expected) {
    try { & $Action | Out-Null } catch {
        if ($_.Exception.Message.Contains($Expected, [StringComparison]::OrdinalIgnoreCase)) { return }
        throw "Expected '$Expected', received '$($_.Exception.Message)'"
    }
    throw "Expected rejection: $Expected"
}

$relativePath = 'tmp/update-startup-' + [guid]::NewGuid().ToString('N') + '.json'
$path = Resolve-NexusProjectPath -Root $repo -Path $relativePath
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
try {
    $directory = 'C:\下载 文件,目录\$1,,补丁'
    $request = [ordered]@{AutoCheck=$false; DownloadDirectory=$directory; NextAutoCheckUtc=[long]::MaxValue}
    $validJson = $request | ConvertTo-Json -Compress
    Set-Content -LiteralPath $path -Value $validJson -Encoding utf8BOM
    $profile = Read-FgAcceptanceUpdatePreferences $relativePath $repo
    if ($profile.source_sha256 -cne (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -or
        $profile.requested.AutoCheck -isnot [bool] -or $profile.requested.AutoCheck -or
        $profile.requested.DownloadDirectory -cne $directory -or
        $profile.requested.NextAutoCheckUtc -isnot [long] -or $profile.requested.NextAutoCheckUtc -ne [long]::MaxValue) {
        throw 'Startup receipt values or source hash differ from the JSON input.'
    }

    $before = "[RenoDX.DLSS5]`r`nNeuralUplift=0`r`nDownloadDirectory=keep-this`r`n"
    $section = " [ RenoDX.DLSS5.Update ] ; saved section`r`n"
    $other = "# comment`r`nKeepMe=unchanged`r`n`r`n [INPUT]`r`nAutoCheck=unchanged"
    $original = $before + $section + "AutoCheck=1`r`nDownloadDirectory = C:\Old`r`nAutoCheck=0`r`nNextAutoCheckUtc`r`n" + $other
    $values = "AutoCheck=0`r`nDownloadDirectory=" + 'C:\下载 文件,,目录\$1,,,,补丁' + "`r`nNextAutoCheckUtc=9223372036854775807`r`n"
    $expected = $before + $section + $values + $other
    if ((Set-FgAcceptanceUpdateSection $original $profile.requested) -cne $expected -or
        (Set-FgAcceptanceUpdateSection $expected $profile.requested) -cne $expected) {
        throw 'Updating ReShade INI changed unrelated text or failed to write a single escaped value per key.'
    }
    Assert-Rejected {
        Set-FgAcceptanceUpdateSection ($original + "`r`n[RenoDX.DLSS5.Update]") $profile.requested
    } 'Duplicate [RenoDX.DLSS5.Update]'

    $emptyJson = '{"AutoCheck":true,"DownloadDirectory":"","NextAutoCheckUtc":0}'
    Set-Content -LiteralPath $path -Value $emptyJson -Encoding utf8
    $empty = (Read-FgAcceptanceUpdatePreferences $path $repo).requested
    $noSection = "[RenoDX.DLSS5]`nNeuralUplift=0"
    $newSection = "[RenoDX.DLSS5.Update]`nAutoCheck=1`nDownloadDirectory=`nNextAutoCheckUtc=0`n"
    if ((Set-FgAcceptanceUpdateSection $noSection $empty) -cne ($noSection + "`n" + $newSection) -or
        (Set-FgAcceptanceUpdateSection ($noSection + "`n[RenoDX.DLSS5.Update]") $empty) -cne ($noSection + "`n" + $newSection)) {
        throw 'Failed to write default preferences with LF and an unterminated INI line.'
    }

    $invalid = @(
        @{ Json='[]'; Expected='must be a JSON object' }
        @{ Json='{"AutoCheck":true,"DownloadDirectory":""}'; Expected='preferences require' }
        @{ Json=$emptyJson.Replace('"AutoCheck"', '"autocheck"'); Expected='unknown or duplicate' }
        @{ Json=$emptyJson.Insert(1, '"AutoCheck":false,'); Expected='unknown or duplicate' }
        @{ Json=$emptyJson.Insert(1, '"URL":"https://example.invalid",'); Expected='unknown or duplicate' }
        @{ Json=$emptyJson.Replace('true', '"false"'); Expected='must be a JSON boolean' }
        @{ Json=$emptyJson.Replace('""', 'null'); Expected='must be a JSON string' }
        @{ Json=$emptyJson.Replace('""', '"relative/path"'); Expected='fully qualified path' }
        @{ Json=$emptyJson.Replace('""', '"C:drive-relative"'); Expected='fully qualified path' }
        @{ Json=$emptyJson.Replace('""', '"C:\\a\n[INPUT]"'); Expected='fully qualified path' }
        @{ Json=$emptyJson.Replace('""', '"C:\\a\u0000b"'); Expected='fully qualified path' }
        @{ Json=$emptyJson.Replace('""', '"C:\\a\uD800"'); Expected='surrogate' }
    )
    foreach ($utc in @('-1', '"0"', '0.0', '1e0', '9223372036854775808')) {
        $invalid += @{ Json=$emptyJson.Replace('0}', "$utc}"); Expected='nonnegative JSON int64' }
    }
    foreach ($case in $invalid) {
        Set-Content -LiteralPath $path -Value $case.Json -Encoding utf8
        Assert-Rejected { Read-FgAcceptanceUpdatePreferences $path $repo } $case.Expected
    }
    [IO.File]::WriteAllBytes($path, [byte[]](0xC3, 0x28))
    Assert-Rejected { Read-FgAcceptanceUpdatePreferences $path $repo } 'GetString'
    Assert-Rejected { Read-FgAcceptanceUpdatePreferences "tmp/no`tfile.json" $repo } 'must name a JSON file'
    Assert-Rejected { Read-FgAcceptanceUpdatePreferences 'host/lock.json' $repo } 'must name a JSON file'
    Assert-Rejected { Read-FgAcceptanceUpdatePreferences '../outside.json' $repo } 'Path must stay below this checkout'

    # Dummy target and installation arguments keep every direct wrapper call offline.
    $arguments = @{
        CandidateManifest='unused-synthetic-candidate'; GameBinaryDirectory='unused-synthetic-game'
        TargetPath='unused-synthetic-target'; Python='unused-synthetic-python'; Scenario='menu'
        UpdatePreferencesPath=$path
    }
    Assert-Rejected { & $entry @arguments } 'requires -RestoreInstallation'
    Assert-Rejected { & $entry @arguments -RestoreInstallation -InstallOnly } 'requires -RestoreInstallation'
    Assert-Rejected { & $entry @arguments -RestoreInstallation } 'GetString'
} finally {
    [IO.File]::Delete($path)
}
Write-Output 'Online update startup preferences tests passed.'
