[CmdletBinding()]
param()

function Get-CiBranchEvidence {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$Tree,
        [Parameter(Mandatory)][ValidatePattern('^[a-f0-9]{40}$')][string]$Commit,
        [long]$CurrentRunId,
        [Parameter(Mandatory)][scriptblock]$Request
    )
    $workflow = & $Request "/repos/$Repository/actions/workflows/ci.yml"
    if ($workflow.path -cne '.github/workflows/ci.yml' -or $workflow.id -le 0) {
        throw 'Cannot identify the repository CI workflow.'
    }
    $find = {
        param([string]$TargetSha)
        foreach ($branch in @('dev','main')) {
            $query = "branch=$branch&event=push"
            if ($TargetSha) { $query += "&head_sha=$TargetSha" }
            $count = 0
            for ($page = 1; $page -le 5; $page++) {
                $response = & $Request "/repos/$Repository/actions/workflows/$($workflow.id)/runs?$query&per_page=100&page=$page"
                $runs = @($response.workflow_runs)
                foreach ($run in $runs) {
                    if ($run.id -eq $CurrentRunId -or $run.workflow_id -ne $workflow.id -or
                        ($run.path -split '@', 2)[0] -cne $workflow.path -or $run.event -cne 'push' -or
                        $run.head_branch -cne $branch -or $run.head_repository.full_name -ine $Repository -or
                        ($TargetSha -and $run.head_sha -cne $TargetSha)) { continue }
                    if ($run.head_sha -cnotmatch '^[a-f0-9]{40}$' -or $run.head_commit.id -cne $run.head_sha -or
                        $run.head_commit.tree_id -cnotmatch '^[a-f0-9]{40}$') {
                        throw 'Cannot identify the CI source tree.'
                    }
                    if ($TargetSha -and $run.head_commit.tree_id -cne $Tree) {
                        throw 'The exact CI target commit reports a different root tree.'
                    }
                    if ($run.head_commit.tree_id -ceq $Tree) { $run }
                }
                $count += $runs.Count
                if ($count -ge $response.total_count) { break }
                if ($page -eq 5 -or $runs.Count -eq 0) {
                    throw 'CI history exceeds the bounded evidence search.'
                }
            }
        }
    }
    # Same-tree reuse applies only when neither protected branch has a run
    # for the exact target SHA, not when that target's run failed.
    $matching = @(& $find $Commit)
    if ($matching.Count -eq 0) { $matching = @(& $find '') }

    # updated_at includes reruns. Do not let an older green hide a pending
    # or failed attempt for this exact tree.
    $run = $matching | Sort-Object @{Expression={[DateTimeOffset]::Parse($_.updated_at)};Descending=$true}, @{Expression={$_.id};Descending=$true} |
        Select-Object -First 1
    if (-not $run) { throw 'No successful branch source-form CI exists for the exact source tree.' }
    if ($run.status -cne 'completed' -or $run.conclusion -cne 'success') {
        throw "The latest matching CI run $($run.id) is $($run.status)/$($run.conclusion)."
    }
    $response = & $Request "/repos/$Repository/actions/runs/$($run.id)/attempts/$($run.run_attempt)/jobs?per_page=100"
    if ($response.total_count -gt 100 -or @($response.jobs).Count -ne $response.total_count) {
        throw 'Incomplete CI job evidence.'
    }
    $required = @($response.jobs | Where-Object name -CEQ 'CI required')
    if ($required.Count -ne 1 -or $required[0].run_id -ne $run.id -or $required[0].head_sha -cne $run.head_sha -or
        $required[0].status -cne 'completed' -or $required[0].conclusion -cne 'success') {
        throw "Matching CI run $($run.id) has no successful CI required job."
    }
    $form = @($required[0].steps | Where-Object name -CEQ 'Check source form')
    if ($form.Count -ne 1 -or $form[0].status -cne 'completed' -or $form[0].conclusion -cne 'success') {
        throw "Matching CI run $($run.id) has no successful source-form step."
    }
    return [pscustomobject]@{ RunId=$run.id; Attempt=$run.run_attempt; SourceCommit=$run.head_sha; Tree=$Tree
        Url="https://github.com/$Repository/actions/runs/$($run.id)" }
}

if ($MyInvocation.InvocationName -eq '.') { return }
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:GITHUB_REPOSITORY -cnotmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$' -or -not $env:GH_TOKEN) {
    throw 'GitHub repository and read token are required for evidence lookup.'
}
$repo = Split-Path $PSScriptRoot -Parent
$identity = @(& git --no-optional-locks -C $repo rev-parse HEAD 'HEAD^{tree}')
if ($LASTEXITCODE -ne 0 -or $identity.Count -ne 2) { throw 'Cannot identify the current source commit and tree.' }
$commit, $tree = $identity
$api = if ($env:GITHUB_API_URL) { $env:GITHUB_API_URL.TrimEnd('/') } else { 'https://api.github.com' }
$headers = @{ Authorization="Bearer $env:GH_TOKEN"; Accept='application/vnd.github+json'; 'X-GitHub-Api-Version'='2022-11-28' }
$request = { param($path) Invoke-RestMethod -Uri ($api + $path) -Headers $headers -TimeoutSec 30 }.GetNewClosure()
$evidence = Get-CiBranchEvidence -Repository $env:GITHUB_REPOSITORY -Tree $tree -Commit $commit -CurrentRunId $env:GITHUB_RUN_ID -Request $request
$reason = "Verified branch source-form CI run $($evidence.RunId), attempt $($evidence.Attempt), for the identical root tree."
Write-Host $reason
if ($env:GITHUB_STEP_SUMMARY) {
    @("Source tree: ``$tree``", '', $reason, "[Branch source $($evidence.SourceCommit)]($($evidence.Url))") >> $env:GITHUB_STEP_SUMMARY
}
