[CmdletBinding()]
param(
    [ValidateSet('all','report','branding','ui','dev-control','presets','unicode-input','native','lastgasp','sdr-policy','hdr-policy','scope','config','constants','control-mask','control-mask-execution','vulkan-control-mask','fg-xefg','fg-native','lifecycle','bridge-motion','stage-plan','edit-reuse-policy','present-frame-policy','capability-policy','adaptive-resolution','copy-slots')]
    [string[]]$Suite = @('all'),
    [string]$SourceRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$HostRoot = (Join-Path (Split-Path $PSScriptRoot -Parent) '.local/renodx'),
    [string]$ShaderIncludeDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.include'),
    [string]$VulkanIncludeDirectory = $(if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK 'Include' }),
    [string]$BuildProject = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build/dlss5.vcxproj'),
    [string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/tests/' + [guid]::NewGuid().ToString('N'))),
    [string]$VcVars64,
    [string]$DetoursLibrary,
    [string]$PythonExecutable,
    [ValidateSet('Auto','Quick','Full','None')][string]$UiPreview = 'Auto',
    [ValidateSet('Auto','Off','On')][string]$DevControlMode = 'Auto',
    [ValidateSet('inline-restore-roi','control-mask','control-mask-safety','control-mask-safety-hardware','control-mask-capture','control-mask-native-ngx','control-mask-native-ngx-toggle','control-mask-native-ngx-toggle-rgb','control-mask-native-ngx-cost','control-mask-native-ngx-motion','control-mask-native-ngx-resize','nr-edit-reuse-history','nr-edit-reuse-cache','nr-edit-reuse-cost','nr-edit-reuse-jitter','control-mask-native-ngx-fg','gpu-pass-diagnostics','native-required-menu','native-owner-idle','native-options-resume','native-options-delayed','native-guide-contract','present-guide-models','adaptive-fg-service','adaptive-inline','adaptive-resource-strategy','adaptive-status','output-framerate-registration','output-framerate-sdk','vulkan-recorded-use-contract','native-present-decision','trace-metadata','stage-chain','stage-resolution','stage-resolution-hardware','same-stage-resolution','same-stage-resolution-hardware','render-stage-resolution','render-stage-resolution-hardware','present-stage-resolution','present-stage-resolution-hardware','native-prefg-resolution','native-prefg-resolution-hardware','native-frame-identity','same-stage-model-pair','same-stage-mixed-pair','source-lifecycle','feature-switch','feature-switch-hardware','f5-mixed','present-chain','present-chain-hardware','present-chain-scrgb','present-chain-hdr10','native-full','native-full-hardware','nr-edit-reuse-contracts','nr-edit-reuse-contracts-public','nr-edit-reuse-render-contracts','nr-edit-reuse-present-contracts','nr-edit-reuse-present-diagnostic','nr-edit-reuse-present-native','nr-edit-reuse-xefg-contracts','nr-edit-reuse-native-contracts','native-frame-source','nr-fg-output-marker','fg-consumer-capture','xefg-present-probe')]
    [string[]]$NativeCase = @(),
    [switch]$VulkanNativeNgx,
    [string]$NativeNgxProjectIdPath,
    [string]$NativeNgxSdkLibraryDirectory,
    [string]$NativeNgxRuntimePath = $(if ($env:NR_NEXUS_NGX_RUNTIME_DIR) { Join-Path $env:NR_NEXUS_NGX_RUNTIME_DIR 'nvngx_dlssnr.dll' }),
    [string]$NativeNgxSrRuntimePath,
    [ValidateSet('dlaa','sr','stage-resolution','same-stage-resolution','render-stage-resolution','present-stage-resolution','ordinary-watchdog','edit-reuse-production','adaptive','adaptive-hardware','adaptive-native-ngx','adaptive-native-snippet')][string[]]$BridgeCase = @(),
    [switch]$BuildOnly,
    [ValidateSet('Standard','Full','Acceptance')][string]$RetentionMode = $(if ($env:NR_NEXUS_TEST_RETENTION) { $env:NR_NEXUS_TEST_RETENTION } else { 'Standard' }),
    [switch]$SoftwareOnly
)
$previousRetention = $env:NR_NEXUS_TEST_RETENTION
$testWorkspaceStarted = $false
try {
$RetentionMode = @('Standard','Full','Acceptance') | Where-Object { $_ -ieq $RetentionMode } | Select-Object -First 1
$env:NR_NEXUS_TEST_RETENTION = $RetentionMode
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repositoryRoot = Split-Path $PSScriptRoot -Parent
$testRoot = Join-Path $repositoryRoot 'tests'
. (Join-Path $testRoot 'test-support.ps1')
$SourceRoot = [IO.Path]::GetFullPath($SourceRoot)
if ($SourceRoot.TrimEnd('\','/') -ne $repositoryRoot.TrimEnd('\','/') -and $UiPreview -in @('Quick','Full')) {
    throw 'Explicit UI preview requires this checkout as SourceRoot; run the preview entry in the target checkout.'
}
$HostRoot = [IO.Path]::GetFullPath($HostRoot)
$ShaderIncludeDirectory = [IO.Path]::GetFullPath($ShaderIncludeDirectory)
$sources = [ordered]@{
    report='report_flow_tests'; ui='ui_layout_tests'
    presets='preset_store_tests'
    'unicode-input'='unicode_input_tests'
    branding='branding_tests'
    native='native_contract_tests'; 'sdr-policy'='present_sdr_restore_policy_tests'
    lastgasp='lastgasp_tests'
    'hdr-policy'='present_hdr_pedestal_policy_tests'; scope='present_restore_scope_tests'
    config='present_restore_configuration_tests'; constants='present_hdr_runtime_constants_tests'
    'control-mask'='control_mask_contract_tests'
    'control-mask-execution'='control_mask_execution_tests'
    'vulkan-control-mask'='vulkan_control_mask_tests'
    'fg-xefg'='fg_xefg_tests'; 'fg-native'='fg_native_tests'
    lifecycle='fg_lifecycle_tests'; 'bridge-motion'='d3d11_bridge_motion_tests'
    'stage-plan'='stage_plan_tests'
    'edit-reuse-policy'='edit_reuse_policy_tests'
    'present-frame-policy'='present_frame_policy_tests'
    'capability-policy'='capability_policy_tests'
    'adaptive-resolution'='adaptive_resolution_tests'
    'copy-slots'='copy_slots_tests'
}
$executionPolicySuites = @('edit-reuse-policy','present-frame-policy','capability-policy')
$dependencyCheckedSuites = $executionPolicySuites + @('adaptive-resolution')
$macroSuites = @('fg-native','bridge-motion','adaptive-resolution','vulkan-control-mask')
if ($Suite -contains 'all') { $Suite = @($sources.Keys) }
if ($DevControlMode -ne 'Auto') {
    if (-not @($Suite | Where-Object { $_ -in $macroSuites }).Count) {
        throw '-DevControlMode Off/On requires fg-native, bridge-motion, adaptive-resolution or vulkan-control-mask.'
    }
    if ($env:CL -match 'NR_NEXUS_DEV_CONTROL\b' -or $env:_CL_ -match 'NR_NEXUS_DEV_CONTROL\b') {
        throw 'Explicit DEV control mode cannot be combined with NR_NEXUS_DEV_CONTROL in CL or _CL_.'
    }
    . (Join-Path $PSScriptRoot 'check-release-experiment-parity.ps1') -SourceRoot $SourceRoot
    $parityInputs = Get-ReleaseParityBuildContext -SourceRoot $SourceRoot -HostRoot $HostRoot
}
if ($BuildOnly -and @($Suite | Where-Object { $_ -in @('config','constants','lifecycle','ui') }).Count) {
    throw '-BuildOnly requires explicitly selecting native/policy/scope/fg-xefg/fg-native/bridge-motion suites.'
}
if ($NativeCase.Count -and $Suite -notcontains 'fg-native') { throw '-NativeCase requires -Suite fg-native (or all).' }
$renderEditCache = $env:NR_NEXUS_TEST_NR_CACHE_ROUTE -ceq 'render' -and $NativeCase -contains 'nr-edit-reuse-cache'
$presentEditNative = $NativeCase -contains 'nr-edit-reuse-present-native'
if ($presentEditNative -and $SoftwareOnly) { throw 'Native NGX Present x2 requires a hardware D3D12 device.' }
if (@($NativeCase | Where-Object {
            $_ -in @('nr-edit-reuse-history','nr-edit-reuse-jitter') -or ($_ -eq 'nr-edit-reuse-cache' -and -not $renderEditCache)
        }).Count -and -not $BuildOnly -and -not $PythonExecutable) {
    throw 'NR edit experiments require -PythonExecutable with NumPy and Pillow for paired analysis before raw cleanup.'
}
if ($VulkanNativeNgx -and $Suite -notcontains 'vulkan-control-mask') { throw '-VulkanNativeNgx requires -Suite vulkan-control-mask (or all).' }
if ($VulkanNativeNgx -and $SoftwareOnly) { throw '-VulkanNativeNgx requires a native Vulkan GPU.' }
$bridgeNativeNgx = @($BridgeCase | Where-Object { $_ -in @('adaptive-native-ngx','adaptive-native-snippet') }).Count -gt 0
if ($SoftwareOnly -and @($BridgeCase | Where-Object { $_ -in @('adaptive-hardware','adaptive-native-ngx','adaptive-native-snippet') }).Count) {
    throw 'Explicit Bridge hardware cases conflict with -SoftwareOnly; no hardware or WARP fallback is permitted.'
}
$nativeNgxCases = @('control-mask-native-ngx','control-mask-native-ngx-toggle','control-mask-native-ngx-toggle-rgb',
    'control-mask-native-ngx-cost','control-mask-native-ngx-motion','control-mask-native-ngx-resize','control-mask-native-ngx-fg',
    'nr-edit-reuse-history','nr-edit-reuse-cache','nr-edit-reuse-cost','nr-edit-reuse-jitter','nr-edit-reuse-present-native')
$usesNativeNgx = $bridgeNativeNgx -or $VulkanNativeNgx -or (@($NativeCase | Where-Object { $_ -in $nativeNgxCases }).Count -gt 0)
if (@($Suite | Where-Object { $_ -in @('fg-native','vulkan-control-mask','bridge-motion') }).Count -gt 0 -and ($usesNativeNgx -or $NativeNgxSdkLibraryDirectory)) {
    if (-not $NativeNgxSdkLibraryDirectory -or
        -not (Test-Path -LiteralPath (Join-Path $NativeNgxSdkLibraryDirectory 'nvsdk_ngx_s.lib') -PathType Leaf)) {
        throw 'Native NGX support requires -NativeNgxSdkLibraryDirectory containing nvsdk_ngx_s.lib; omit it for WARP+MOCK cases.'
    }
    $NativeNgxSdkLibraryDirectory = (Resolve-Path -LiteralPath $NativeNgxSdkLibraryDirectory).Path
}
if ($usesNativeNgx) {
    if (-not $NativeNgxProjectIdPath -or -not (Test-Path -LiteralPath $NativeNgxProjectIdPath -PathType Leaf)) {
        throw 'Selected native NGX cases require -NativeNgxProjectIdPath to an existing ProjectID file.'
    }
    if (-not $NativeNgxRuntimePath -or -not (Test-Path -LiteralPath $NativeNgxRuntimePath -PathType Leaf)) {
        throw 'Selected native NGX cases require -NativeNgxRuntimePath or NR_NEXUS_NGX_RUNTIME_DIR.'
    }
    $NativeNgxProjectIdPath = (Resolve-Path -LiteralPath $NativeNgxProjectIdPath).Path
    $NativeNgxRuntimePath = (Resolve-Path -LiteralPath $NativeNgxRuntimePath).Path
    if ($NativeNgxSrRuntimePath) {
        $NativeNgxSrRuntimePath = (Resolve-Path -LiteralPath $NativeNgxSrRuntimePath).Path
    }
    if ($bridgeNativeNgx -and (-not $NativeNgxSrRuntimePath -or -not (Test-Path -LiteralPath $NativeNgxSrRuntimePath -PathType Leaf))) {
        throw 'Bridge native NGX requires the existing SR Runtime via -NativeNgxSrRuntimePath.'
    }
    if ($renderEditCache -and (-not $NativeNgxSrRuntimePath -or -not (Test-Path -LiteralPath $NativeNgxSrRuntimePath -PathType Leaf))) {
        throw 'Native NGX Render x2 requires the existing SR Runtime via -NativeNgxSrRuntimePath.'
    }
    if ($presentEditNative -and (-not $NativeNgxSrRuntimePath -or -not (Test-Path -LiteralPath $NativeNgxSrRuntimePath -PathType Leaf))) {
        throw 'Native NGX Present x2 requires the existing SR Runtime via -NativeNgxSrRuntimePath.'
    }
}
if ($BridgeCase -and $Suite -notcontains 'bridge-motion') { throw '-BridgeCase requires -Suite bridge-motion (or all).' }
if ($Suite -contains 'vulkan-control-mask') {
    if (-not $PSBoundParameters.ContainsKey('VulkanIncludeDirectory') -and -not $env:VULKAN_SDK) {
        $VulkanIncludeDirectory = & (Join-Path $PSScriptRoot 'setup-vulkan-headers.ps1')
    }
    if (-not $VulkanIncludeDirectory -or -not (Test-Path -LiteralPath (Join-Path $VulkanIncludeDirectory 'vulkan/vulkan.h'))) {
        throw '-Suite vulkan-control-mask requires -VulkanIncludeDirectory or VULKAN_SDK.'
    }
    $VulkanIncludeDirectory = [IO.Path]::GetFullPath($VulkanIncludeDirectory)
}
$testWorkspaceStarted = $true
$OutputDirectory = Initialize-TestOutput $OutputDirectory
if (-not $DetoursLibrary) { $DetoursLibrary = Join-Path $HostRoot 'external/Detours/lib.X64/detours.lib' }
$parameters = @{ SourceRoot=$SourceRoot; HostRoot=$HostRoot; ShaderIncludeDirectory=$ShaderIncludeDirectory
    BuildProject=$BuildProject; VcVars64=$VcVars64 }
Initialize-TestBuild $VcVars64
$compiler = (Get-Command cl.exe -CommandType Application).Path
$common = @('/nologo','/std:c++20','/EHsc','/W4','/O2','/MT','/utf-8','/DNOMINMAX','/DWIN32_LEAN_AND_MEAN')
$includes = @($SourceRoot, "$HostRoot/src", "$HostRoot/src/addons/dlss5",
    "$HostRoot/external/Detours/src", "$HostRoot/external/reshade", "$HostRoot/external/Streamline/include")
$libraries = @($DetoursLibrary,'uuid.lib','advapi32.lib','d3d11.lib','d3d12.lib','dxgi.lib','dxguid.lib')
foreach ($item in $Suite) {
    $directory = Join-Path $OutputDirectory $item
    if ($item -eq 'unicode-input') {
        if ($BuildOnly) { throw '-BuildOnly is not supported for unicode-input.' }
        & (Join-Path $testRoot 'run-unicode-input-tests.ps1') -SourceRoot $SourceRoot -OutputDirectory $directory -VcVars64 $VcVars64
        continue
    }
    if ($item -eq 'presets') {
        & (Join-Path $testRoot 'run-preset-store-tests.ps1') -SourceRoot $SourceRoot -HostRoot $HostRoot -OutputDirectory $directory -VcVars64 $VcVars64
        & (Join-Path $testRoot 'run-profile-store-tests.ps1') -SourceRoot $SourceRoot -HostRoot $HostRoot -OutputDirectory (Join-Path $OutputDirectory 'profiles') -VcVars64 $VcVars64
        & (Join-Path $testRoot 'run-preset-dialog-tests.ps1') -OutputDirectory (Join-Path $OutputDirectory 'preset-dialog') -VcVars64 $VcVars64
        continue
    }
    if ($item -in @('report','config','constants','lifecycle','ui','dev-control')) {
        if ($BuildOnly) { throw '-BuildOnly is supported for native/policy/scope/fg-xefg/fg-native/bridge-motion; select these explicitly.' }
        $entry = switch ($item) {
            report { 'run-report-tests.ps1' }
            config { 'run-present-restore-configuration-tests.ps1' }
            constants { 'run-present-hdr-runtime-constants-tests.ps1' }
            lifecycle { 'run-fg-lifecycle-tests.ps1' }
            ui { 'run-ui-layout-tests.ps1' }
            'dev-control' { 'run-ui-layout-tests.ps1' }
        }
        if ($item -eq 'report') {
            & (Join-Path $testRoot $entry) @parameters -OutputDirectory $directory -PythonExecutable $PythonExecutable
        } elseif ($item -eq 'dev-control') {
            & (Join-Path $testRoot $entry) @parameters -OutputDirectory $directory -DevControlOnly
            & (Join-Path $testRoot 'run-dev-control-pipe-tests.ps1') -SourceRoot $SourceRoot -HostRoot $HostRoot -OutputDirectory (Join-Path $directory 'pipe') -VcVars64 $VcVars64
        } else {
            & (Join-Path $testRoot $entry) @parameters -OutputDirectory $directory
        }
        continue
    }
    $directory = Initialize-TestOutput $directory
    if ($item -eq 'branding') {
        & (Join-Path $testRoot 'run-branding-check-tests.ps1') -OutputDirectory $directory
    }
    $name = $sources[$item]
    $exe = Join-Path $directory "$name.exe"
    $arguments = $common + @($includes | ForEach-Object { "/I$_" })
    $explicitMode = $DevControlMode -ne 'Auto' -and $item -in $macroSuites
    if ($item -in $dependencyCheckedSuites) {
        $msvcInclude = Join-Path $env:VCToolsInstallDir 'include'
        $ucrtInclude = Join-Path $env:WindowsSdkDir "Include/$($env:WindowsSDKVersion.TrimEnd('\'))/ucrt"
        $arguments = $common + @('/X', '/Y-', "/I$SourceRoot", "/I$msvcInclude", "/I$ucrtInclude")
        if (-not $explicitMode) { $arguments += '/UNR_NEXUS_DEV_CONTROL' }
    }
    if ($item -in @('control-mask-execution','vulkan-control-mask')) {
        $arguments += "/I$HostRoot/external/reshade/include"
    }
    $link = $libraries
    if ($item -in $dependencyCheckedSuites -or $item -in @('branding','sdr-policy','hdr-policy','scope','stage-plan','control-mask')) { $link = @() }
    if ($item -in @('fg-native','bridge-motion','vulkan-control-mask')) {
        if (-not (Test-Path -LiteralPath "$ShaderIncludeDirectory/embed/shaders.h")) { throw 'Build new shaders first.' }
        $arguments += @('/bigobj','/Gy','/Gw','/DNDEBUG','/DRESHADE_ADDON=1',
            "/I$ShaderIncludeDirectory", "/I$HostRoot/external/DLSS/include",
            "/I$HostRoot/external/json/include", "/I$HostRoot/external/frozen/include", "/I$HostRoot/external/gtl/include")
        $automaticNativeDev = $item -eq 'fg-native' -and @($NativeCase | Where-Object {
            $_ -in @('inline-restore-roi','nr-edit-reuse-contracts','nr-edit-reuse-native-contracts','native-frame-source',
                'nr-fg-output-marker','fg-consumer-capture','xefg-present-probe')
        }).Count -gt 0
        $automaticBridgeDev = $item -eq 'bridge-motion' -and @($BridgeCase | Where-Object {
            $_ -in @('adaptive','adaptive-hardware','adaptive-native-ngx','adaptive-native-snippet')
        }).Count -gt 0
        if (-not $explicitMode -and ($automaticNativeDev -or $automaticBridgeDev)) { $arguments += '/DNR_NEXUS_DEV_CONTROL' }
        if ($item -eq 'fg-native' -and $NativeCase -contains 'xefg-present-probe') {
            $arguments += '/DNR_NEXUS_TEST_XEFG_PRESENT_PROBE'
        }
        if ($item -eq 'vulkan-control-mask') { $arguments += "/I$VulkanIncludeDirectory" }
        $link += @('user32.lib','gdi32.lib','ole32.lib','psapi.lib')
        if (($item -in @('fg-native','vulkan-control-mask') -or ($item -eq 'bridge-motion' -and $bridgeNativeNgx)) -and $NativeNgxSdkLibraryDirectory) {
            $arguments += '/DNR_NEXUS_TEST_NATIVE_NGX'
            $link += (Join-Path $NativeNgxSdkLibraryDirectory 'nvsdk_ngx_s.lib')
        }
        if ($item -eq 'bridge-motion') { $link += 'd3dcompiler.lib' }
    }
    if ($explicitMode -and $DevControlMode -eq 'On') { $arguments += '/DNR_NEXUS_DEV_CONTROL' }
    if ($explicitMode -and $DevControlMode -eq 'Off') { $arguments += '/UNR_NEXUS_DEV_CONTROL' }
    if ($item -eq 'branding') { $arguments += '/DRENODX_DLSS5_DEBUG' }
    $arguments += @((Join-Path $testRoot "$name.cpp"), "/Fo$directory/$name.obj", "/Fe$exe",
        '/sourceDependencies', "$directory/dependencies.json", '/link', '/OPT:REF', '/OPT:ICF') + $link
    $arguments | ConvertTo-Json | Set-Content -LiteralPath "$directory/arguments.json" -Encoding utf8
    Register-TestCompilerOutput $directory -ObjectPath @(Join-Path $directory "$name.obj") -ExecutablePath $exe
    & $compiler @arguments *> "$directory/compile.log"
    if ($LASTEXITCODE -ne 0) { throw "Compile/link failed: $item; see $directory/compile.log" }
    if ($explicitMode) { $executableHash = (Get-FileHash -LiteralPath $exe).Hash }
    $dependencyFile = Join-Path $directory 'dependencies.json'
    $inputPaths = @((Join-Path $testRoot "$name.cpp"), $PSCommandPath)
    if ($explicitMode) {
        $inputPaths += @((Join-Path $repositoryRoot 'host/lock.json'), (Join-Path $repositoryRoot 'host/tools.lock.json'),
            (Join-Path $PSScriptRoot 'check-release-experiment-parity.ps1'), (Join-Path $testRoot 'test-support.ps1'))
        if ($item -ne 'adaptive-resolution') { $inputPaths += $DetoursLibrary }
    }
    if ($item -in $dependencyCheckedSuites) {
        $policyChecker = Join-Path $PSScriptRoot 'check-execution-policy-dependencies.ps1'
        $inputPaths += $policyChecker
        & $policyChecker -DependencyFile $dependencyFile -SourceRoot $SourceRoot -Suite $item
    }
    $dependencies = (Get-Content -LiteralPath $dependencyFile -Raw | ConvertFrom-Json).Data.Includes
    $hashes = @($inputPaths + $dependencies | Sort-Object -Unique | ForEach-Object {
        @{path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash}
    })
    $hashes | ConvertTo-Json | Set-Content -LiteralPath "$directory/inputs.json" -Encoding utf8
    $skipVulkanRun = -not $BuildOnly -and $SoftwareOnly -and $item -eq 'vulkan-control-mask'
    if ($skipVulkanRun) {
        Write-Host 'vulkan-control-mask compiled; execution skipped under -SoftwareOnly because it requires a native Vulkan GPU and loader.'
    }
    if (-not $BuildOnly -and -not $skipVulkanRun) {
        if ($item -eq 'bridge-motion') {
            $bridgeCases = if ($BridgeCase.Count) { $BridgeCase } else { @('') }
            foreach ($case in $bridgeCases) {
                $bridgeArguments = @()
                $suffix = ''
                if ($case) { $bridgeArguments += "--$case"; $suffix = "-$case" }
                if ($case -in @('adaptive-native-ngx','adaptive-native-snippet')) {
                    $runtimeLink = Join-Path $directory 'nvngx_dlssnr.dll'
                    $srRuntimeLink = Join-Path $directory 'nvngx_dlss.dll'
                    if ((Test-Path -LiteralPath $runtimeLink) -or (Test-Path -LiteralPath $srRuntimeLink)) {
                        throw 'Bridge native NGX Runtime link target already exists; no existing file is replaced.'
                    }
                    $nrLinked = $false
                    $srLinked = $false
                    try {
                        New-Item -ItemType HardLink -Path $runtimeLink -Target $NativeNgxRuntimePath | Out-Null
                        $nrLinked = $true
                        New-Item -ItemType HardLink -Path $srRuntimeLink -Target $NativeNgxSrRuntimePath | Out-Null
                        $srLinked = $true
                        @($NativeNgxRuntimePath, $NativeNgxSrRuntimePath) | ForEach-Object {
                            @{ path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash }
                        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'native-ngx-runtime-inputs.json') -Encoding utf8
                        Invoke-TestProcess $exe $directory $case (@($bridgeArguments) + $NativeNgxProjectIdPath) -TimeoutMilliseconds 90000
                    } finally {
                        if ($srLinked -and (Test-Path -LiteralPath $srRuntimeLink)) { Remove-Item -LiteralPath $srRuntimeLink -Force }
                        if ($nrLinked -and (Test-Path -LiteralPath $runtimeLink)) { Remove-Item -LiteralPath $runtimeLink -Force }
                    }
                    continue
                }
                if ($case -eq 'adaptive-hardware') {
                    Invoke-TestProcess $exe $directory $case $bridgeArguments -TimeoutMilliseconds 90000
                    continue
                }
                if ($case -eq 'adaptive') {
                    Invoke-TestProcess $exe $directory "warp$suffix" (@('--warp') + $bridgeArguments)
                    continue
                }
                if ($case -eq 'edit-reuse-production') {
                    $stdout = Join-Path $directory 'warp-edit-reuse-production.stdout.log'
                    Invoke-TestProcess $exe $directory 'warp-edit-reuse-production' @('--warp','--edit-reuse-production')
                    $dev = [int]($arguments -ccontains '/DNR_NEXUS_DEV_CONTROL')
                    $receipts = @(Get-Content -LiteralPath $stdout | Where-Object {
                        $_ -ceq "PASS case=bridge-edit-reuse-production evidence=WARP+MOCK dev=$dev"
                    })
                    if ($receipts.Count -ne 1) {
                        Complete-TestOutput $directory -Outcome Failed
                        throw "Missing or ambiguous Bridge production edit reuse receipt: $stdout"
                    }
                    continue
                }
                if (-not $SoftwareOnly) { Invoke-TestProcess $exe $directory "hardware$suffix" $bridgeArguments }
                Invoke-TestProcess $exe $directory "warp$suffix" (@('--warp') + $bridgeArguments)
            }
        } elseif ($item -eq 'fg-native' -and $NativeCase.Count) {
            foreach ($case in $NativeCase) {
                if ($case -in $nativeNgxCases) {
                    $runtimeLink = Join-Path $directory 'nvngx_dlssnr.dll'
                    $srRuntimeLink = if ($NativeNgxSrRuntimePath) { Join-Path $directory 'nvngx_dlss.dll' } else { $null }
                    if (Test-Path -LiteralPath $runtimeLink) { throw "Runtime link target already exists: $runtimeLink" }
                    if ($srRuntimeLink -and (Test-Path -LiteralPath $srRuntimeLink)) { throw "Runtime link target already exists: $srRuntimeLink" }
                    New-Item -ItemType HardLink -Path $runtimeLink -Target $NativeNgxRuntimePath | Out-Null
                    try {
                        if ($srRuntimeLink) {
                            New-Item -ItemType HardLink -Path $srRuntimeLink -Target $NativeNgxSrRuntimePath | Out-Null
                        }
                        @($NativeNgxRuntimePath, $NativeNgxSrRuntimePath) | Where-Object { $_ } | ForEach-Object {
                            @{ path=$_; sha256=(Get-FileHash -LiteralPath $_).Hash }
                        } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'native-ngx-runtime-inputs.json') -Encoding utf8
                        $nativeArgument = "--$case"
                        if ($case -eq 'nr-edit-reuse-jitter') {
                            try {
                                Invoke-TestProcess $exe $directory $case @($nativeArgument, $NativeNgxProjectIdPath) -TimeoutMilliseconds 90000 -DeferFailureCleanup
                            } finally {
                                $calibrationDirectory = Join-Path $directory 'jitter-calibration'
                                if (Test-Path -LiteralPath $calibrationDirectory -PathType Container) {
                                    Get-ChildItem -LiteralPath $calibrationDirectory -File | Where-Object {
                                        $_.Extension -in @('.rgba16f','.r32f','.rg16f')
                                    } | ForEach-Object {
                                        Register-TestDisposable -Directory $directory -Path $_.FullName -Kind 'sr-jitter-raw'
                                    }
                                }
                            }
                            & $PythonExecutable (Join-Path $PSScriptRoot 'offline/sr_jitter.py') --input $directory
                            if ($LASTEXITCODE -ne 0) { throw 'SR jitter calibration failed or is inconclusive; see sr-jitter-analysis.json.' }
                        } elseif ($case -in @('nr-edit-reuse-history','nr-edit-reuse-cache','nr-edit-reuse-cost') -and
                                  -not ($renderEditCache -and $case -eq 'nr-edit-reuse-cache')) {
                            $cacheCostExperiment = $case -eq 'nr-edit-reuse-cost'
                            $cacheExperiment = $case -in @('nr-edit-reuse-cache','nr-edit-reuse-cost')
                            $conditionVariable = if ($cacheExperiment) { 'NR_NEXUS_TEST_NR_CACHE_CONDITION' } else { 'NR_NEXUS_TEST_NR_HISTORY_CONDITION' }
                            $previousCondition = [Environment]::GetEnvironmentVariable($conditionVariable)
                            $conditions = if ($cacheExperiment) { @('baseline','cache') } else { @('baseline','baseline-repeat','x2-compose','x2-reset') }
                            try {
                                foreach ($condition in $conditions) {
                                    [Environment]::SetEnvironmentVariable($conditionVariable, $condition)
                                    try {
                                        Invoke-TestProcess $exe $directory "$case-$condition" @($nativeArgument, $NativeNgxProjectIdPath) -TimeoutMilliseconds 90000 -DeferFailureCleanup
                                    } finally {
                                        $conditionDirectory = Join-Path $directory $(if ($cacheCostExperiment) { "cost-$condition" } elseif ($cacheExperiment -and $condition -eq 'baseline') { 'cache-baseline' } else { $condition })
                                        if ((Test-Path -LiteralPath $conditionDirectory -PathType Container) -and
                                            $global:NexusTestOutputs[$directory].status -eq 'Running') {
                                            Get-ChildItem -LiteralPath $conditionDirectory -File | Where-Object {
                                                $_.Extension -in @('.rgba16f','.r10g10b10a2','.r32f','.rg16f','.r8','.r32u')
                                            } | ForEach-Object {
                                                Register-TestDisposable -Directory $directory -Path $_.FullName -Kind 'nr-history-raw'
                                            }
                                        }
                                    }
                                }
                                if (-not $cacheCostExperiment) {
                                    $analysisScript = if ($cacheExperiment) { 'offline/nr_edit_cache.py' } else { 'offline/nr_edit_reuse.py' }
                                    & $PythonExecutable (Join-Path $PSScriptRoot $analysisScript) --input $directory
                                    if ($LASTEXITCODE -ne 0) {
                                        Complete-TestOutput $directory -Outcome Failed
                                        throw 'NR edit paired analysis failed; see the analysis JSON and preserved failure sample.'
                                    }
                                }
                            } finally {
                                [Environment]::SetEnvironmentVariable($conditionVariable, $previousCondition)
                            }
                        } else {
                            Invoke-TestProcess $exe $directory $case @($nativeArgument, $NativeNgxProjectIdPath) -TimeoutMilliseconds 90000
                        }
                    } catch {
                        if ($case -in @('nr-edit-reuse-history','nr-edit-reuse-cache','nr-edit-reuse-cost','nr-edit-reuse-jitter')) { Complete-TestOutput $directory -Outcome Failed }
                        throw
                    } finally {
                        if (Test-Path -LiteralPath $runtimeLink) { Remove-Item -LiteralPath $runtimeLink -Force }
                        if ($srRuntimeLink -and (Test-Path -LiteralPath $srRuntimeLink)) { Remove-Item -LiteralPath $srRuntimeLink -Force }
                    }
                } elseif ($case -eq 'fg-consumer-capture') {
                    $consumerPassed = $false
                    try {
                        Invoke-TestProcess $exe $directory $case @("--$case") -DeferFailureCleanup
                        $consumerPassed = $true
                    } finally {
                        $consumerRaw = Join-Path $directory 'fg-consumer-evidence'
                        if (Test-Path -LiteralPath $consumerRaw) {
                            @(Get-ChildItem -LiteralPath $consumerRaw -Filter '*.raw' -File | ForEach-Object {
                                Register-TestDisposable -Directory $directory -Path $_.FullName -Kind 'fg-consumer-raw'
                                @{path=[IO.Path]::GetRelativePath($directory, $_.FullName); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
                            }) | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $directory 'fg-consumer-raw-inputs.json') -Encoding utf8
                        }
                        if (-not $consumerPassed) { Complete-TestOutput $directory -Outcome Failed }
                    }
                } else {
                    $timeout = if ($case -eq 'output-framerate-sdk') { 180000 } else { 45000 }
                    Invoke-TestProcess $exe $directory $case @("--$case") -TimeoutMilliseconds $timeout
                }
            }
        } else {
            Invoke-TestProcess $exe $directory 'test'
            if ($item -in $executionPolicySuites) {
                $policyName = $item.ToLowerInvariant().Replace('-', ' ')
                $receipts = @(Get-Content -LiteralPath (Join-Path $directory 'test.stdout.log') |
                    Where-Object { $_ -cmatch ('^PASS ' + $policyName + ' evidence=CPU cases=[1-9][0-9]*$') })
                if ($receipts.Count -ne 1) { throw "Missing or ambiguous CPU receipt: $directory/test.stdout.log" }
            }
            if ($item -eq 'fg-native') {
                Invoke-TestProcess $exe $directory 'f5-mixed' @('--f5-mixed')
                Invoke-TestProcess $exe $directory 'control-mask' @('--control-mask')
                Invoke-TestProcess $exe $directory 'adaptive-fg-service' @('--adaptive-fg-service')
                Invoke-TestProcess $exe $directory 'adaptive-inline' @('--adaptive-inline')
                Invoke-TestProcess $exe $directory 'present-guide-models' @('--present-guide-models')
            }
            if ($item -eq 'vulkan-control-mask') {
                Invoke-TestProcess $exe $directory 'evaluate-recovery' @('--evaluate-recovery')
                Invoke-TestProcess $exe $directory 'evaluate-recovery-hdr' @('--evaluate-recovery-hdr')
            }
        }
        if ($item -eq 'vulkan-control-mask' -and $VulkanNativeNgx) {
            $runtimeLink = Join-Path $directory 'nvngx_dlssnr.dll'
            if (Test-Path -LiteralPath $runtimeLink) { throw "Runtime link target already exists: $runtimeLink" }
            New-Item -ItemType HardLink -Path $runtimeLink -Target $NativeNgxRuntimePath | Out-Null
            try {
                Invoke-TestProcess $exe $directory 'native-ngx' @('--native-ngx', $NativeNgxProjectIdPath) -TimeoutMilliseconds 90000
            } finally {
                if (Test-Path -LiteralPath $runtimeLink) { Remove-Item -LiteralPath $runtimeLink -Force }
            }
        }
        foreach ($inputHash in $hashes) {
            if ((Get-FileHash -LiteralPath $inputHash.path).Hash -ne $inputHash.sha256) { throw "Test input changed: $($inputHash.path)" }
        }
    }
    $result = @{suite=$item; build='passed'; run=$(if ($BuildOnly -or $skipVulkanRun) { 'not_run' } else { 'passed' })}
    if ($explicitMode) {
        if ((Get-FileHash -LiteralPath $exe).Hash -cne $executableHash) { throw "Test EXE changed: $exe" }
        $cases = switch ($item) {
            'fg-native' {
                if ($NativeCase.Count) { $NativeCase }
                else { 'test', 'f5-mixed', 'control-mask', 'adaptive-fg-service', 'adaptive-inline', 'present-guide-models' }
            }
            'bridge-motion' { $BridgeCase }
            default { 'test' }
        }
        $result['buildIdentity'] = [ordered]@{
            mode=$DevControlMode; source=$parityInputs; compiler=$compiler
            compilerSha256=(Get-FileHash -LiteralPath $compiler).Hash
            compilerEnvironment=@{CL=$env:CL; _CL_=$env:_CL_}
            argumentsSha256=(Get-FileHash -LiteralPath (Join-Path $directory 'arguments.json')).Hash
            inputsSha256=(Get-FileHash -LiteralPath (Join-Path $directory 'inputs.json')).Hash
            dependenciesSha256=(Get-FileHash -LiteralPath $dependencyFile).Hash
            executableSha256=$executableHash; executable=[IO.Path]::GetFileName($exe)
            softwareOnly=[bool]$SoftwareOnly; cases=@($cases)
        }
    }
    if ($item -in $dependencyCheckedSuites) { $result.dependency_policy = 'passed' }
    if ($skipVulkanRun) { $result.skip_reason = 'software_only_native_gpu_required' }
    $result | ConvertTo-Json -Depth 9 | Set-Content -LiteralPath "$directory/result.json" -Encoding utf8
    if (-not $BuildOnly) { Complete-TestOutput $directory }
}
if (-not $BuildOnly) { Complete-TestOutput $OutputDirectory }
Write-Host "Test outputs: $OutputDirectory"
if ($Suite -contains 'ui' -and -not $BuildOnly -and $UiPreview -ne 'None') {
    $previewMode = $UiPreview
    if ($previewMode -eq 'Auto') {
        $uiChanges = @(& git --no-optional-locks -C $SourceRoot diff --name-only HEAD -- ui)
        $uiChanges += @(& git --no-optional-locks -C $SourceRoot ls-files --others --exclude-standard -- ui)
        # These headers also contain rendering/business code. Trigger on UI
        # hunks rather than forcing captures for every edit to the monolith.
        $mixedDiff = @(& git --no-optional-locks -C $SourceRoot diff --unified=0 HEAD -- dlssnr.hpp preset_runtime.hpp profile_runtime.hpp brand.h)
        $uiChanges += @($mixedDiff | Where-Object {
            $_ -match '^@@.*\b(Draw[A-Z]\w*|OnOverlay|\w*Ui\w*)\b' -or
            $_ -match '^[+-](?![+-]).*(ImGui::|ui::|\bui_\w+|\bNR_NEXUS_NAME\b)'
        })
        $previewMode = $(if ($uiChanges.Count) { 'Quick' } else { 'None' })
        if ($SourceRoot.TrimEnd('\','/') -ne $repositoryRoot.TrimEnd('\','/')) {
            $previewMode = 'None'
            Write-Host 'Custom SourceRoot tested; invoke preview-ui.ps1 from that checkout to preview the same source.'
        }
    }
    if ($previewMode -ne 'None') {
        & (Join-Path $PSScriptRoot 'preview-ui.ps1') -Mode $previewMode -PythonExecutable $PythonExecutable `
            -HostRoot $HostRoot -ShaderIncludeDirectory $ShaderIncludeDirectory -BuildProject $BuildProject -VcVars64 $VcVars64 -RetentionMode $RetentionMode
    }
}

} catch {
    if ($OutputDirectory -and (Test-Path -LiteralPath (Join-Path $OutputDirectory 'artifact-lifecycle.json'))) {
        if ($testWorkspaceStarted) {
            foreach ($item in $Suite) {
                Complete-TestOutput (Join-Path $OutputDirectory $item) -Outcome Failed
            }
        }
        Complete-TestOutput $OutputDirectory -Outcome Failed
    }
    throw
} finally {
    $env:NR_NEXUS_TEST_RETENTION = $previousRetention
    if ($testWorkspaceStarted) {
        try { Invoke-TestWorkspaceMaintenanceIfDue }
        catch { Write-Warning "Historical test maintenance could not run: $($_.Exception.Message)" }
    }
}
