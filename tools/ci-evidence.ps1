[CmdletBinding()]
param([ValidateSet('Branch','Release','Gate')][string]$Mode = 'Branch')

function Test-CiMainReuseEvent([string]$RefName, [string]$EventName) {
    return ($RefName -ceq 'main' -and $EventName -cin @('push','workflow_dispatch'))
}

function Assert-CiGate {
    param(
        [string]$RefName,
        [string]$EventName,
        [string]$EvidenceResult,
        [string]$RunCore,
        [string]$WindowsResult
    )

    if ($EvidenceResult -cne 'success') {
        throw "Expected successful evidence job; got '$EvidenceResult'."
    }
    if ($RunCore -ceq 'true' -and $WindowsResult -ceq 'success') { return }
    if ($RunCore -ceq 'false' -and $WindowsResult -ceq 'skipped' -and
        (Test-CiMainReuseEvent $RefName $EventName)) { return }

    throw "Invalid core result for $EventName on ${RefName}: run_core='$RunCore', windows='$WindowsResult'."
}

function Get-CiCoreEvidence {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$Tree,
        [long]$CurrentRunId,
        [Parameter(Mandatory)][scriptblock]$Request
    )
    $workflow = & $Request "/repos/$Repository/actions/workflows/ci.yml"
    if ($workflow.path -cne '.github/workflows/ci.yml' -or $workflow.id -le 0) {
        throw 'Cannot identify the repository CI workflow.'
    }
    # Read all conclusions, including pending runs and reruns. A bounded,
    # incomplete history cannot establish reuse and must fall back to core.
    $runs = [Collections.Generic.List[object]]::new()
    for ($page = 1; $page -le 5; $page++) {
        $response = & $Request "/repos/$Repository/actions/workflows/$($workflow.id)/runs?per_page=100&page=$page"
        foreach ($run in @($response.workflow_runs)) { $runs.Add($run) }
        if ($runs.Count -ge $response.total_count) { break }
        if ($page -eq 5 -or @($response.workflow_runs).Count -eq 0) {
            throw 'CI history exceeds the bounded evidence search.'
        }
    }
    $matching = foreach ($run in $runs) {
        if ($run.id -eq $CurrentRunId -or $run.workflow_id -ne $workflow.id -or
            ($run.path -split '@', 2)[0] -cne $workflow.path -or $run.event -cne 'push' -or
            $run.head_branch -cnotin @('dev','main') -or $run.head_repository.full_name -ine $Repository) { continue }
        if ($run.head_sha -cnotmatch '^[a-f0-9]{40}$' -or $run.head_commit.id -cne $run.head_sha -or
            $run.head_commit.tree_id -cnotmatch '^[a-f0-9]{40}$') {
            throw 'Cannot identify the CI source tree.'
        }
        if ($run.head_commit.tree_id -ceq $Tree) { $run }
    }
    # updated_at includes a later attempt of an older run. Do not hide that
    # pending/failed attempt behind an earlier successful run of this tree.
    $matching = @($matching | Sort-Object @{Expression={[DateTimeOffset]::Parse($_.updated_at)};Descending=$true}, @{Expression={$_.id};Descending=$true})
    $requiredSteps = @('Check version and release tag', 'Prepare pinned shader compilers',
        'Prepare pinned RenoDX host and dependencies', 'Build clean Publishing Release x64',
        'Run independent core regression', 'Verify Publishing isolation and package',
        'Upload installable archive and checksum')
    foreach ($run in $matching) {
        if ($run.status -cne 'completed' -or $run.conclusion -cne 'success') {
            throw "The latest matching CI run $($run.id) is $($run.status)/$($run.conclusion)."
        }
        $response = & $Request "/repos/$Repository/actions/runs/$($run.id)/attempts/$($run.run_attempt)/jobs?per_page=100"
        if ($response.total_count -gt 100 -or @($response.jobs).Count -ne $response.total_count) {
            throw 'Incomplete CI job evidence.'
        }
        $reusedMain = $false
        foreach ($job in @($response.jobs)) {
            if ($job.run_id -ne $run.id -or $job.head_sha -cne $run.head_sha -or
                $job.status -cne 'completed' -or $job.conclusion -cne 'success') { continue }
            $passed = @($job.steps | Where-Object { $_.status -ceq 'completed' -and $_.conclusion -ceq 'success' } | ForEach-Object name)
            if (@($requiredSteps | Where-Object { $passed -cnotcontains $_ }).Count -eq 0) {
                return [pscustomobject]@{ RunId=$run.id; SourceCommit=$run.head_sha; Tree=$Tree
                    Url="https://github.com/$Repository/actions/runs/$($run.id)" }
            }
            if ($run.head_branch -ceq 'main' -and $passed -ccontains 'Resolve exact-tree core evidence') { $reusedMain = $true }
        }
        # A successful main identity-only run is not core evidence. Follow it
        # back to an actual successful core job for the very same root tree.
        if (-not $reusedMain) { throw "Matching CI run $($run.id) has no successful core build/package job." }
    }
    throw 'No successful branch core evidence exists for the exact source tree.'
}

if ($MyInvocation.InvocationName -eq '.') { return }
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Mode -eq 'Gate') {
    Assert-CiGate -RefName $env:GITHUB_REF_NAME -EventName $env:GITHUB_EVENT_NAME `
        -EvidenceResult $env:CI_EVIDENCE_RESULT -RunCore $env:CI_RUN_CORE -WindowsResult $env:CI_WINDOWS_RESULT
    Write-Host 'CI required passed.'
    return
}
$repo = Split-Path $PSScriptRoot -Parent
$tree = (& git --no-optional-locks -C $repo rev-parse 'HEAD^{tree}').Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the current source tree.' }
$runCore = $true
$evidence = $null
$reason = 'This branch/event requires independent core regression.'
if ($Mode -eq 'Release' -or (Test-CiMainReuseEvent $env:GITHUB_REF_NAME $env:GITHUB_EVENT_NAME)) {
    try {
        if ($env:GITHUB_REPOSITORY -cnotmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or -not $env:GH_TOKEN) {
            throw 'GitHub repository and read token are required for evidence lookup.'
        }
        $api = if ($env:GITHUB_API_URL) { $env:GITHUB_API_URL.TrimEnd('/') } else { 'https://api.github.com' }
        $headers = @{ Authorization="Bearer $env:GH_TOKEN"; Accept='application/vnd.github+json'; 'X-GitHub-Api-Version'='2022-11-28' }
        $request = { param($path) Invoke-RestMethod -Uri ($api + $path) -Headers $headers -TimeoutSec 30 }.GetNewClosure()
        $evidence = Get-CiCoreEvidence -Repository $env:GITHUB_REPOSITORY -Tree $tree -CurrentRunId $env:GITHUB_RUN_ID -Request $request
        $runCore = $false
        $reason = "Reused actual core run $($evidence.RunId) for the identical root tree."
    } catch {
        if ($Mode -eq 'Release') { throw }
        $reason = "Core required: $($_.Exception.Message)"
        Write-Warning $reason
    }
}
Write-Host $reason
if ($env:GITHUB_OUTPUT) { "run_core=$($runCore.ToString().ToLowerInvariant())" >> $env:GITHUB_OUTPUT }
if ($env:GITHUB_STEP_SUMMARY) {
    @("Source tree: ``$tree``", '', $reason) >> $env:GITHUB_STEP_SUMMARY
    if ($evidence) { "`nSource core: [$($evidence.SourceCommit)]($($evidence.Url))" >> $env:GITHUB_STEP_SUMMARY }
}
