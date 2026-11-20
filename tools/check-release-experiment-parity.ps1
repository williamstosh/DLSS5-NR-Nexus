[CmdletBinding()]
param(
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [switch]$AllowPending
)

function Get-ReleaseParityGitRevision([string]$Directory, [string]$Ref) {
    $revision = & git --no-optional-locks -C $Directory rev-parse $Ref
    if ($LASTEXITCODE -ne 0 -or $revision -cnotmatch '^[0-9a-f]{40}$') {
        throw "Cannot identify $Ref in $Directory."
    }
    return $revision
}

function Get-ReleaseParityBuildContext([string]$SourceRoot, [string]$HostRoot) {
    $repo = Split-Path $PSScriptRoot -Parent
    $lockPath = Join-Path $repo 'host/lock.json'
    $lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json -AsHashtable
    $hostCommit = Get-ReleaseParityGitRevision $HostRoot HEAD
    if ($hostCommit -cne $lock.host.commit) { throw 'Test host differs from host/lock.json.' }
    foreach ($relative in $lock.compatibility.files.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $HostRoot $relative)).Hash -cne $lock.compatibility.files[$relative]) {
            throw "Test host compatibility input differs from host/lock.json: $relative"
        }
    }
    $hostChanges = @(& git --no-optional-locks -C $HostRoot diff --name-only --ignore-submodules=all HEAD -- . ':(exclude)external' ':(exclude)src/addons/dlss5')
    if ($LASTEXITCODE -ne 0 -or (Compare-Object @($lock.compatibility.files.Keys) $hostChanges)) {
        throw 'Test host tracked changes differ from the compatibility patch.'
    }
    $dependencies = [ordered]@{}
    foreach ($dependency in $lock.dependencies) {
        $directory = Join-Path $HostRoot $dependency.path
        $commit = Get-ReleaseParityGitRevision $directory HEAD
        & git --no-optional-locks -C $directory diff --quiet --ignore-submodules=all HEAD
        if ($LASTEXITCODE -ne 0 -or $commit -cne $dependency.commit) {
            throw "Test dependency differs from host/lock.json: $($dependency.path)"
        }
        $dependencies[$dependency.path] = $commit
    }
    $patterns = @('*.hpp','*.h','*.cpp','tools/*.ps1','tools/release-experiment-parity.json','host/*.json','host/*.patch')
    $changedFiles = @(& git --no-optional-locks -C $SourceRoot -c core.quotePath=false diff --name-only HEAD -- @patterns)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate changed test inputs.' }
    $changedFiles += @(& git --no-optional-locks -C $SourceRoot -c core.quotePath=false ls-files --others --exclude-standard -- @patterns)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate new test inputs.' }
    $workingInputs = [ordered]@{}
    foreach ($relative in @($changedFiles | Sort-Object -Unique)) {
        $path = Join-Path $SourceRoot $relative
        $workingInputs[$relative] = if (Test-Path -LiteralPath $path -PathType Leaf) { (Get-FileHash -LiteralPath $path).Hash } else { 'removed' }
    }
    return [ordered]@{
        commit=(Get-ReleaseParityGitRevision $SourceRoot HEAD)
        tree=(Get-ReleaseParityGitRevision $SourceRoot 'HEAD^{tree}')
        hostCommit=$hostCommit; dependencies=$dependencies; workingInputs=$workingInputs
        hostLockSha256=(Get-FileHash -LiteralPath $lockPath).Hash
        hostPatchSha256=(Get-FileHash -LiteralPath (Join-Path $repo "host/$($lock.compatibility.patch)")).Hash
        toolsLockSha256=(Get-FileHash -LiteralPath (Join-Path $repo 'host/tools.lock.json')).Hash
    }
}

function Assert-ReleaseParityBuildPair([string]$Suite, [string]$OffDirectory, [string]$OnDirectory, $BuildContext) {
    $pair = [ordered]@{}
    foreach ($mode in 'Off','On') {
        $directory = if ($mode -eq 'Off') { $OffDirectory } else { $OnDirectory }
        $result = Get-Content -LiteralPath (Join-Path $directory 'result.json') -Raw | ConvertFrom-Json -AsHashtable
        $identity = $result.buildIdentity
        if ($result.suite -cne $Suite -or $result.build -cne 'passed' -or $result.run -cne 'passed' -or
            $identity.mode -cne $mode -or $identity.softwareOnly -ne $true) {
            throw "$Suite requires a completed -SoftwareOnly run with -DevControlMode $mode."
        }
        foreach ($file in @{ 'arguments.json'=$identity.argumentsSha256; 'inputs.json'=$identity.inputsSha256;
                'dependencies.json'=$identity.dependenciesSha256;
                $identity.executable=$identity.executableSha256 }.GetEnumerator()) {
            if ((Get-FileHash -LiteralPath (Join-Path $directory $file.Key)).Hash -cne $file.Value) {
                throw "$Suite $mode evidence changed: $($file.Key)"
            }
        }
        $argsPath = Join-Path $directory 'arguments.json'
        $arguments = @(Get-Content -LiteralPath $argsPath -Raw | ConvertFrom-Json)
        $macro = @($arguments | Where-Object { $_ -match 'NR_NEXUS_DEV_CONTROL\b' })
        $expectedMacro = if ($mode -eq 'On') { '/DNR_NEXUS_DEV_CONTROL' } else { '/UNR_NEXUS_DEV_CONTROL' }
        $expectedExecutableArgument = '/Fe' + (Join-Path ([IO.Path]::GetFullPath($directory)) $identity.executable)
        if ($macro.Count -ne 1 -or $macro[0] -cne $expectedMacro -or $arguments -cnotcontains $expectedExecutableArgument -or
            $identity.compilerEnvironment.CL -match 'NR_NEXUS_DEV_CONTROL\b' -or
            $identity.compilerEnvironment._CL_ -match 'NR_NEXUS_DEV_CONTROL\b') {
            throw "$Suite $mode compiler command has an incorrect macro or EXE identity."
        }
        $dependenciesPath = Join-Path $directory 'dependencies.json'
        $inputs = @(Get-Content -LiteralPath (Join-Path $directory 'inputs.json') -Raw | ConvertFrom-Json)
        if ($inputs.Count -eq 0 -or -not (Test-Path -LiteralPath $dependenciesPath -PathType Leaf)) {
            throw "$Suite $mode has no actual compiler input closure."
        }
        $currentInputs = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($input in $inputs) {
            if ((Get-FileHash -LiteralPath $input.path).Hash -cne $input.sha256) { throw "Test input changed: $($input.path)" }
            $currentInputs.Add($input.path, $input.sha256)
        }
        $closure = (Get-Content -LiteralPath $dependenciesPath -Raw | ConvertFrom-Json).Data
        foreach ($file in @($closure.Source) + @($closure.Includes)) {
            if (-not $currentInputs.ContainsKey($file)) { throw "Compiler input has no hash: $file" }
        }
        if ((Get-FileHash -LiteralPath $identity.compiler).Hash -cne $identity.compilerSha256 -or
            ($identity.source | ConvertTo-Json -Depth 5 -Compress) -cne ($BuildContext | ConvertTo-Json -Depth 5 -Compress)) {
            throw "$Suite $mode does not identify the core commit and locked dependencies."
        }
        $commonArguments = @($arguments | Where-Object {
            $_ -ne $expectedMacro -and $_ -cnotmatch '^/F[eo]' -and
                [string]$_ -cne ($directory + '/dependencies.json')
        })
        $pair[$mode] = [ordered]@{
            identity=$identity; arguments=$commonArguments; inputs=$currentInputs
            source=$closure.Source
        }
    }
    if ([IO.Path]::GetFullPath($OffDirectory) -ieq [IO.Path]::GetFullPath($OnDirectory) -or
        ($Suite -ne 'adaptive-resolution' -and $pair.Off.identity.executableSha256 -ceq $pair.On.identity.executableSha256) -or
        $pair.Off.identity.compilerSha256 -cne $pair.On.identity.compilerSha256 -or $pair.Off.source -cne $pair.On.source -or
        $pair.Off.identity.compilerEnvironment.CL -cne $pair.On.identity.compilerEnvironment.CL -or
        $pair.Off.identity.compilerEnvironment._CL_ -cne $pair.On.identity.compilerEnvironment._CL_ -or
        ($pair.Off.arguments -join "`n") -cne ($pair.On.arguments -join "`n")) {
        throw "$Suite builds require distinct EXEs and matching compiler, source and non-DEV arguments."
    }
    # Conditional includes may add diagnostics in one build. Every actual
    # compiler input still has to match the snapshot used by that build.
    foreach ($file in $pair.Off.inputs.Keys) {
        if ($pair.On.inputs.ContainsKey($file) -and $pair.Off.inputs[$file] -cne $pair.On.inputs[$file]) {
            throw "$Suite has different input bytes across modes: $file"
        }
    }
    $pair.Off.identity['directory'] = [IO.Path]::GetFullPath($OffDirectory)
    $pair.On.identity['directory'] = [IO.Path]::GetFullPath($OnDirectory)
    return [ordered]@{ Off=$pair.Off.identity; On=$pair.On.identity }
}

function Compare-ReleaseParityCase([string]$Row, $Case, [string]$OffLog, [string]$OnLog) {
    $pair = [ordered]@{}
    $escapedRow = [regex]::Escape($Row)
    foreach ($mode in 'Off','On') {
        $path = if ($mode -eq 'Off') { $OffLog } else { $OnLog }
        $dev = [int]($mode -eq 'On')
        $lines = Get-Content -LiteralPath $path
        $pattern = $Case.receipt.Replace('{dev}', [string]$dev)
        $receipts = @($lines | Where-Object { $_ -cmatch $pattern })
        if ($receipts.Count -ne 1) {
            throw "Missing or ambiguous $mode receipt for $($Case.name): $path"
        }
        $traces = [ordered]@{}
        # Some existing fixtures already publish stable, assertion-backed
        # business records. Match those records in place when registered.
        if ($Case['existingTrace']) {
            foreach ($scenario in $Case.scenarios) {
                $scenarioPattern = $Case.expect[$scenario].Replace('{dev}', [string]$dev)
                $matchesForScenario = @($lines | Where-Object { $_ -cmatch $scenarioPattern })
                if ($matchesForScenario.Count -ne 1) {
                    throw "Missing or ambiguous $mode outcome for $Row/${scenario}: $path"
                }
                $stable = [regex]::Match($matchesForScenario[0], $scenarioPattern).Groups['trace']
                $traces[$scenario] = if ($stable.Success) { $stable.Value } else { $matchesForScenario[0] }
            }
            foreach ($line in $lines | Where-Object { $_ -cmatch "^NR_RELEASE_PARITY $escapedRow " }) {
                $registered = @($Case.scenarios | Where-Object {
                    $line -cmatch $Case.expect[$_].Replace('{dev}', [string]$dev)
                })
                if ($registered.Count -ne 1) {
                    throw "Unregistered or ambiguous $mode parity trace for ${Row}: $line"
                }
            }
        }
        $prefixed = if (-not $Case['existingTrace']) { $lines | Where-Object { $_ -cmatch "^NR_RELEASE_PARITY $escapedRow " } }
        foreach ($line in $prefixed) {
            if ($line -cnotmatch "^NR_RELEASE_PARITY $escapedRow ([a-z][a-z0-9-]*) (\S.*)$") {
                throw "Malformed $mode parity trace for $Row/$($Case.name): $line"
            }
            $scenario = $Matches[1]
            $facts = $Matches[2]
            if ($Case.scenarios -cnotcontains $scenario) { throw "Unregistered $mode parity scenario for ${Row}: $scenario" }
            if ($traces.Contains($scenario)) { throw "Duplicate $mode parity scenario for ${Row}: $scenario" }
            if ($Case.Contains('expect') -and $Case.expect.Contains($scenario) -and $facts -cnotmatch $Case.expect[$scenario]) {
                throw "Unexpected $mode business outcome for $Row/${scenario}: $facts"
            }
            $traces[$scenario] = $facts
        }
        foreach ($scenario in $Case.scenarios) {
            if (-not $traces.Contains($scenario)) { throw "Missing $mode parity scenario for ${Row}: $scenario" }
        }
        $pair[$mode] = [ordered]@{ receipt=$receipts[0]; trace=$traces }
    }
    if (($pair.Off.trace | ConvertTo-Json -Compress) -cne ($pair.On.trace | ConvertTo-Json -Compress)) {
        throw "Production traces differ between ordinary and DEV: $Row/$($Case.name)."
    }
    return [ordered]@{ case=$Case.name; ordinaryReceipt=$pair.Off.receipt; devReceipt=$pair.On.receipt
        trace=$pair.Off.trace }
}

function Assert-ReleaseParitySourceSection([string]$SourceRoot, [string]$Name, $Section) {
    $source = Get-Content -LiteralPath (Join-Path $SourceRoot $Section.file) -Raw
    $start = $source.IndexOf($Section.start, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing $Name source entry: $($Section.start)" }
    $end = $source.IndexOf($Section.end, $start + $Section.start.Length, [StringComparison]::Ordinal)
    if ($end -lt 0) { throw "Missing $Name source boundary: $($Section.end)" }
    $body = $source.Substring($start, $end - $start)
    foreach ($anchor in $Section.anchors) {
        if (-not $body.Contains($anchor, [StringComparison]::Ordinal)) { throw "Missing $Name effect: $anchor" }
    }
    if ($Section.Contains('forbidden') -and $body -cmatch $Section.forbidden) {
        throw "$Name accesses production state: $($Matches[0])"
    }
}

function Get-ReleaseParitySettingCatalog([string]$Config, [string]$Presets, [string]$PresetRuntime, [string[]]$IniSources) {
    $aliases = @{}
    foreach ($alias in [regex]::Matches("$Config`n$Presets`n$PresetRuntime", '\bchar\s+(\w+)\[\]\s*=\s*"([A-Za-z_]\w*)"')) {
        $aliases[$alias.Groups[1].Value] = $alias.Groups[2].Value
    }
    $configSymbols = @{}
    foreach ($setting in [regex]::Matches($Config, '\bSetting\s+(\w+)\s*[{(]\s*(?:"([A-Za-z_]\w*)"|(?:presets::)?(\w+))\s*,')) {
        $key = $setting.Groups[2].Value
        if (-not $key) { $key = $aliases[$setting.Groups[3].Value] }
        if ($key) { $configSymbols[$setting.Groups[1].Value] = $key }
    }
    $settings = @($configSymbols.Values)
    $settings += @(foreach ($setting in [regex]::Matches("$Config`n$PresetRuntime",
            '\{\s*(?:"([A-Za-z_]\w*)"|(?:presets::)?(\w+))\s*,\s*&[^;{}]+?,\s*SettingEffect::\w+\s*\}')) {
        if ($setting.Groups[1].Success) { $setting.Groups[1].Value }
        elseif ($aliases.ContainsKey($setting.Groups[2].Value)) { $aliases[$setting.Groups[2].Value] }
    })
    $presetKeys = @(foreach ($field in [regex]::Matches($Presets, '\.key\s*=\s*(?:"([A-Za-z_]\w*)"(?:\s*#PASS\s*"([A-Za-z_]\w*)")?|(\w+))\s*,')) {
        if ($field.Groups[2].Success) {
            foreach ($pass in [regex]::Matches($Presets, 'RENODX_DLSS5_PRESET_PASS_RULES\((\d+)\)')) {
                $field.Groups[1].Value + $pass.Groups[1].Value + $field.Groups[2].Value
            }
        } elseif ($field.Groups[1].Success) { $field.Groups[1].Value }
        elseif ($aliases.ContainsKey($field.Groups[3].Value)) { $aliases[$field.Groups[3].Value] }
        else { throw "Unresolved preset key: $($field.Groups[3].Value)" }
    })
    $iniKeys = @(foreach ($source in $IniSources) {
        foreach ($reader in [regex]::Matches($source, '(?:get|set)_config_value\s*\(\s*[^,;]+,\s*[^,;]+,\s*(?:"([A-Za-z_]\w*)"|(?:presets::)?(\w+))\s*,')) {
            if ($reader.Groups[1].Success) { $reader.Groups[1].Value }
            elseif ($aliases.ContainsKey($reader.Groups[2].Value)) { $aliases[$reader.Groups[2].Value] }
        }
        foreach ($reader in [regex]::Matches($source, 'ReadPresentRestoreRaw\(\s*(?:"([A-Za-z_]\w*)"|(\w+)\.key)\s*\)')) {
            if ($reader.Groups[1].Success) { $reader.Groups[1].Value }
            elseif ($configSymbols.ContainsKey($reader.Groups[2].Value)) { $configSymbols[$reader.Groups[2].Value] }
        }
    })
    $iniKeys += @([regex]::Matches($Config, '\{\s*"([A-Za-z_]\w*)"\s*,\s*IniKind::\w+\s*\}') | ForEach-Object { $_.Groups[1].Value })
    return [ordered]@{ settings=@($settings | Sort-Object -CaseSensitive -Unique)
        presets=@($presetKeys | Sort-Object -CaseSensitive -Unique)
        ini=@($iniKeys | Sort-Object -CaseSensitive -Unique) }
}

function Get-ReleaseParityCatalogHash([string[]]$Keys) {
    [Array]::Sort($Keys, [StringComparer]::Ordinal)
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($Keys -join "`n"))))
}

function Get-ReleaseParityRegistration([string]$SourceRoot, [switch]$AllowPending) {
    $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'release-experiment-parity.json') -Raw | ConvertFrom-Json -AsHashtable
    $requiredRows = @('bridge-adaptive','native-prefg-present-x2','dx12-upscaled-adaptive-x2',
        'adaptive-resource-strategies','native-application-frame-count')
    $rows = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $settings = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $commands = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    $sourceSites = @{}
    $pending = @()
    foreach ($row in $manifest.rows) {
        if (-not $rows.Add($row.id) -or $row.state -cnotin @('pending','ready') -or
            -not ($row.producer -and $row.consumer -and $row.success -and $row.refusal -and $row.fallback -and
                  @($row.sources).Count -gt 0 -and @($row.cases).Count -gt 0)) {
            throw "Invalid release parity registration: $($row.id)"
        }
        if ($row.state -eq 'pending') { $pending += $row.id }
        foreach ($key in $row.intent) { [void]$settings.Add($key) }
        foreach ($command in $row.commands) {
            if (-not $commands.Add($command)) { throw "DEV command has duplicate owners: $command" }
        }
        foreach ($site in $row.sources) {
            $file = $site[0]
            if (-not $sourceSites.ContainsKey($file)) { $sourceSites[$file] = @() }
            $sourceSites[$file] += @{ row=$row.id; anchor=$site[1]; ready=$row.state -eq 'ready'; found=$false }
        }
        $caseNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($case in $row.cases) {
            if ($case.suite -cnotin @('fg-native','bridge-motion','adaptive-resolution') -or
                -not $caseNames.Add("$($case.suite)/$($case.name)") -or $case.log -cnotmatch '^[a-z0-9-]+$' -or
                -not $case.receipt.StartsWith('^') -or @($case.scenarios).Count -eq 0 -or
                @($case.scenarios | Sort-Object -Unique).Count -ne $case.scenarios.Count -or
                @($case.scenarios | Where-Object { $_ -cnotmatch '^[a-z][a-z0-9-]*$' }).Count -gt 0) {
                throw "Invalid test case registration for $($row.id): $($case.name)"
            }
            if ($row.state -eq 'ready' -and (-not $case.Contains('expect') -or
                @($case.scenarios | Where-Object { -not $case.expect.Contains($_) -or
                    -not $case.expect[$_].StartsWith('^') -or -not $case.expect[$_].EndsWith('$') }).Count)) {
                throw "Missing explicit business expectations for $($row.id)/$($case.name)."
            }
        }
    }
    foreach ($row in $requiredRows) {
        if (-not $rows.Contains($row)) { throw "Missing release parity registration: $row" }
    }
    foreach ($category in 'diagnosticCommands','testCommands','sharedOperationAdapters') {
        foreach ($command in $manifest[$category].Keys) {
            if (-not $manifest[$category][$command] -or -not $commands.Add($command)) {
                throw "Invalid DEV command classification: $command"
            }
            if ($category -eq 'sharedOperationAdapters') {
                Assert-ReleaseParitySourceSection $SourceRoot $command $manifest[$category][$command]
            }
        }
    }
    foreach ($guard in $manifest.diagnosticWrites.Keys) {
        Assert-ReleaseParitySourceSection $SourceRoot $guard $manifest.diagnosticWrites[$guard]
    }

    # Track the complete independent key catalogs, including the older keys
    # outside this migration. Existing keys keep their catalog fingerprint.
    # New gameplay keys join a parity row and new UI/diagnostic keys get an
    # explicit classification without modifying the grandfathered fingerprint.
    $config = Get-Content -LiteralPath (Join-Path $SourceRoot 'dlssnr.hpp') -Raw
    $presets = Get-Content -LiteralPath (Join-Path $SourceRoot 'preset_fields.hpp') -Raw
    $presetRuntime = Get-Content -LiteralPath (Join-Path $SourceRoot 'preset_runtime.hpp') -Raw
    $iniSources = @(Get-ChildItem -LiteralPath $SourceRoot -File | Where-Object Extension -in @('.hpp','.cpp') |
        Select-String -Pattern '\b(get_config_value|set_config_value|ReadPresentRestoreRaw)\(' -List |
        ForEach-Object { Get-Content -LiteralPath $_.Path -Raw })
    $catalog = Get-ReleaseParitySettingCatalog $config $presets $presetRuntime $iniSources
    foreach ($directory in $catalog.Keys) {
        $keys = $catalog[$directory]
        foreach ($key in $settings) {
            if ($keys -cnotcontains $key) { throw "Missing $directory registration for formal intent: $key" }
        }
        foreach ($key in $manifest.nonGameplaySettings.Keys) {
            $classification = $manifest.nonGameplaySettings[$key]
            if ($settings.Contains($key) -or -not $classification.purpose -or
                (($keys -ccontains $key) -ne ($classification.catalogs -ccontains $directory))) {
                throw "Invalid non-gameplay setting classification: $key ($directory)"
            }
        }
        $legacy = @($keys | Where-Object { -not $settings.Contains($_) -and -not $manifest.nonGameplaySettings.Contains($_) })
        $approved = $manifest.legacySettings[$directory]
        if ($legacy.Count -ne $approved.count -or (Get-ReleaseParityCatalogHash $legacy) -cne $approved.sha256) {
            throw "Unclassified changes in the $directory setting catalog ($($legacy.Count) entries, expected $($approved.count)). Classify the changed keys in release-experiment-parity.json."
        }
    }
    $dispatch = Get-Content -LiteralPath (Join-Path $SourceRoot 'dev_control.hpp') -Raw
    $actualCommands = @([regex]::Matches($dispatch, '\*operation\s*(?:==|!=)\s*"([a-z0-9_]+)"') |
        ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
    foreach ($command in $actualCommands) {
        if (-not $commands.Contains($command)) { throw "Unclassified DEV command: $command" }
    }
    foreach ($command in $commands) {
        if ($actualCommands -cnotcontains $command) { throw "Release parity lists an absent DEV command: $command" }
    }

    $macroSites = @{}
    $adapterFiles = @('addon.cpp','d3d11_bridge.hpp','dlssnr.hpp','fg_native.hpp','fg_native_options.hpp','fg_xefg.hpp','present_path.hpp')
    $registrations = '\b((?:\w+::)*(?:[rR]egister(?:_event|\w+)|unregister_event|(?:Configure|Install|Uninstall|Attach|Detach)\w+))(?=\s*(?:<[^;]+?>)?\s*\()'
    $adapterProducers = '\b((?:\w+::)*(?:Forget|Observe|Publish|Track)\w+)(?=\s*(?:<[^;]+?>)?\s*\()'
    $productionFiles = @(Get-ChildItem -LiteralPath $SourceRoot -File | Where-Object Extension -in @('.cpp','.hpp') | ForEach-Object Name)
    foreach ($file in @($productionFiles + @($sourceSites.Keys) | Sort-Object -Unique)) {
        $sourceText = Get-Content -LiteralPath (Join-Path $SourceRoot $file) -Raw
        if (-not $sourceSites.ContainsKey($file) -and -not $sourceText.Contains('NR_NEXUS_DEV_CONTROL')) { continue }
        $producer = if ($file -in $adapterFiles) { "$registrations|$adapterProducers" } else { $registrations }
        # A directive spanning physical lines still controls its entire branch.
        $logicalLines = ($sourceText -replace '\\\r?\n', ' ') -split '\r?\n'
        $guards = [Collections.Generic.List[bool]]::new()
        foreach ($line in $logicalLines) {
            if ($line -match '^\s*#\s*(if|ifdef|ifndef|elif|endif)\b(.*)') {
                $directive = $Matches[1]
                $usesDev = $Matches[2] -match '\bNR_NEXUS_DEV_CONTROL\b'
                switch ($directive) {
                    'endif' { $guards.RemoveAt($guards.Count - 1) }
                    'elif' { $guards[$guards.Count - 1] = $guards[$guards.Count - 1] -or $usesDev }
                    default { $guards.Add($usesDev) }
                }
                continue
            }
            if ($line -match '^\s*(//|\*)') { continue }
            $guarded = $guards.Contains($true)
            foreach ($site in $sourceSites[$file]) {
                if ($line.Contains($site.anchor, [StringComparison]::Ordinal)) {
                    $site.found = $true
                    if ($site.ready -and $guarded) { throw "$($site.row) production source is DEV-dependent: $file $($site.anchor)" }
                }
            }
            if ($guarded) {
                foreach ($match in [regex]::Matches($line, $producer)) {
                    $name = $match.Value
                    if ($name -cmatch '^reshade::(?:un)?register_event$') {
                        $name += $line.Substring($match.Index + $match.Length).Split(';')[0].Trim()
                    }
                    $macroSites["$file ${name}"] = $true
                }
            }
        }
    }
    foreach ($file in $sourceSites.Keys) {
        foreach ($site in $sourceSites[$file]) {
            if ($site.ready -and -not $site.found) { throw "$($site.row) production source is absent: $file $($site.anchor)" }
        }
    }
    $unclassified = @($macroSites.Keys | Sort-Object | Where-Object { -not $manifest.macroSites.Contains($_) })
    if ($unclassified.Count) { throw "Unclassified DEV-dependent producers:`n$($unclassified -join "`n")" }
    foreach ($site in $manifest.macroSites.Keys) {
        $classification = $manifest.macroSites[$site]
        if (-not $macroSites.ContainsKey($site) -or ($classification -cnotin @('diagnostic','test') -and $classification -cnotin $pending)) {
            throw "Obsolete or incomplete DEV producer classification: $site ($classification)"
        }
    }
    if ($pending.Count -gt 0 -and -not $AllowPending) {
        throw "Release parity migration pending: $($pending -join ', ')."
    }
    $manifest['pending'] = $pending
    return $manifest
}

if ($MyInvocation.InvocationName -eq '.') { return }
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$registration = Get-ReleaseParityRegistration -SourceRoot $SourceRoot -AllowPending:$AllowPending
Write-Host "PASS release parity registration evidence=STATIC rows=$($registration.rows.Count)"
if ($registration.pending.Count) { Write-Host "Pending migration: $($registration.pending -join ', ')." }
