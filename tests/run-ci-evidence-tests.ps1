[CmdletBinding()]
param([string]$OutputDirectory = (Join-Path (Split-Path $PSScriptRoot -Parent) ('tmp/ci-tests/evidence-' + [guid]::NewGuid().ToString('N'))))
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot '../tools/ci-evidence.ps1')
. (Join-Path $PSScriptRoot 'test-support.ps1')
$OutputDirectory = Initialize-TestOutput $OutputDirectory
$tree = 'a' * 40
$sha = 'b' * 40
$checks = [Collections.Generic.List[string]]::new()
$steps = @('Check out source','Check source form')
function New-FixtureRun([long]$Id = 1, [string]$Commit = $sha) {
    @{ id=$Id;workflow_id=7;path='.github/workflows/ci.yml';event='push';head_branch='dev'
        head_repository=@{full_name='fixture/repository'};head_sha=$Commit;run_attempt=1
        head_commit=@{id=$Commit;tree_id=$tree}
        status='completed';conclusion='success';updated_at="2026-11-15T00:00:0${Id}Z" }
}
function New-FixtureJob($Run, [string[]]$Names = $steps, [string]$Name = 'CI required') {
    @{name=$Name;run_id=$Run.id;head_sha=$Run.head_sha;status='completed';conclusion='success'
        steps=@($Names | ForEach-Object { @{name=$_;status='completed';conclusion='success'} })}
}
function Resolve-Fixture([object[]]$Runs, [hashtable]$Jobs, [long]$CurrentRunId = 0, [switch]$ApiError, [switch]$Overflow, [string]$TargetCommit = $sha) {
    $request = {
        param($path)
        if ($ApiError) { throw 'Synthetic API failure.' }
        if ($path -ceq '/repos/fixture/repository/actions/workflows/ci.yml') { return @{id=7;path='.github/workflows/ci.yml'} }
        if ($path -cmatch '/runs\?branch=(dev|main)&event=push(?:&head_sha=([a-f0-9]{40}))?&per_page=100&page=([0-9]+)$') {
            $branch = $Matches[1]
            $requestedSha = $Matches[2]
            $filtered = @($Runs | Where-Object {
                $_.head_branch -ceq $branch -and $_.event -ceq 'push' -and
                (-not $requestedSha -or $_.head_sha -ceq $requestedSha)
            })
            return @{total_count=$(if ($Overflow -and -not $requestedSha) {501} else {$filtered.Count});workflow_runs=$filtered}
        }
        if ($path -match '/runs/([0-9]+)/attempts/([0-9]+)/jobs\?per_page=100$') {
            $id = [long]$Matches[1]
            $attempt = [long]$Matches[2]
            $matching = @($Runs | Where-Object id -EQ $id)
            if ($matching.Count -ne 1 -or $matching[0].run_attempt -ne $attempt) { throw 'Unexpected run attempt.' }
            $items = @($Jobs[$id])
            return @{total_count=$items.Count;jobs=$items}
        }
        throw "Unexpected mock API request: $path"
    }.GetNewClosure()
    Get-CiBranchEvidence -Repository fixture/repository -Tree $tree -Commit $TargetCommit -CurrentRunId $CurrentRunId -Request $request
}
function Assert-Accepted([string]$Name, [hashtable]$Arguments, [long]$ExpectedRun = 1) {
    $result = Resolve-Fixture @Arguments
    if ($result.RunId -ne $ExpectedRun -or $result.Tree -cne $tree) { throw "Wrong source evidence: $Name" }
    $checks.Add($Name)
}
function Assert-Rejected([string]$Name, [hashtable]$Arguments, [string]$Pattern) {
    try { Resolve-Fixture @Arguments | Out-Null }
    catch {
        if ($_.Exception.Message -notmatch $Pattern) { throw "Wrong rejection ${Name}: $($_.Exception.Message)" }
        $checks.Add($Name)
        return
    }
    throw "Evidence accepted incorrectly: $Name"
}

$outcome = 'Failed'
try {
    $first = New-FixtureRun
    $job = New-FixtureJob $first
    Assert-Accepted 'same tree with checked source form' @{Runs=@($first);Jobs=@{[long]1=$job}}
    $other = New-FixtureRun 2 ('c' * 40)
    Assert-Accepted 'different commit identical root tree' @{Runs=@($other);Jobs=@{[long]2=(New-FixtureJob $other)}} 2
    $different = New-FixtureRun 1 ('d' * 40)
    $different.head_commit.tree_id='d' * 40
    Assert-Rejected 'different root tree' @{Runs=@($different);Jobs=@{[long]1=$job}} 'No successful'
    $impossible = New-FixtureRun
    $impossible.head_commit.tree_id='d' * 40
    Assert-Rejected 'wrong root for exact target' @{Runs=@($impossible);Jobs=@{[long]1=$job}} 'different root tree'
    $qualified = New-FixtureRun
    $qualified.path += '@refs/heads/dev'
    Assert-Accepted 'workflow path with ref suffix' @{Runs=@($qualified);Jobs=@{[long]1=$job}}
    $invalid = New-FixtureRun
    $invalid.head_commit.id='e' * 40
    Assert-Rejected 'commit payload identity mismatch' @{Runs=@($invalid);Jobs=@{[long]1=$job}} 'Cannot identify'
    foreach ($state in @('failure','cancelled','timed_out','pending')) {
        $newer = New-FixtureRun 2
        $newer.conclusion = $state
        if ($state -eq 'pending') { $newer.status='in_progress';$newer.conclusion=$null }
        Assert-Rejected "newer same-tree $state" @{Runs=@($newer,$first);Jobs=@{[long]1=$job}} 'latest matching'
    }
    $oldRerun = New-FixtureRun 1
    $oldRerun.updated_at='2026-11-15T01:00:00Z';$oldRerun.status='queued';$oldRerun.conclusion=$null
    Assert-Rejected 'later attempt of older run blocks success' @{Runs=@($other,$oldRerun);Jobs=@{[long]2=(New-FixtureJob $other)}} 'latest matching'
    foreach ($field in @('event','head_branch','head_repository','workflow_id','path')) {
        $wrong = New-FixtureRun
        $wrong[$field] = switch ($field) {
            event {'pull_request'}; head_branch {'feature'}; head_repository {@{full_name='fork/repository'}}
            workflow_id {8}; path {'.github/workflows/other.yml'}
        }
        Assert-Rejected "wrong source $field" @{Runs=@($wrong);Jobs=@{[long]1=$job}} 'No successful'
    }
    $old = New-FixtureJob $first @('Resolve exact-tree core evidence','Require successful core or exact-tree evidence')
    Assert-Rejected 'old same-name build gate is not source-form evidence' @{Runs=@($first);Jobs=@{[long]1=$old}} 'no successful source-form'
    $duplicate = New-FixtureJob $first @('Check source form','Check source form')
    Assert-Rejected 'ambiguous source-form step' @{Runs=@($first);Jobs=@{[long]1=$duplicate}} 'no successful source-form'
    $skipped = New-FixtureJob $first
    ($skipped.steps | Where-Object name -CEQ 'Check source form').conclusion='skipped'
    Assert-Rejected 'skipped source-form step' @{Runs=@($first);Jobs=@{[long]1=$skipped}} 'no successful source-form'
    $failedJob = New-FixtureJob $first
    $failedJob.conclusion='failure'
    Assert-Rejected 'failed required job despite green run' @{Runs=@($first);Jobs=@{[long]1=$failedJob}} 'no successful CI required'
    $wrongName = New-FixtureJob $first -Name 'Unrelated check'
    Assert-Rejected 'wrong check name despite correct step' @{Runs=@($first);Jobs=@{[long]1=$wrongName}} 'no successful CI required'
    $wrongCommit = New-FixtureJob $first
    $wrongCommit.head_sha='e' * 40
    Assert-Rejected 'wrong checked commit' @{Runs=@($first);Jobs=@{[long]1=$wrongCommit}} 'no successful CI required'
    $main = New-FixtureRun 2 ('c' * 40)
    $main.head_branch='main'
    $main.run_attempt=3
    $mainJob = New-FixtureJob $main
    Assert-Accepted 'main check on a different SHA with identical tree' @{Runs=@($main,$first);Jobs=@{[long]1=$job;[long]2=$mainJob};TargetCommit=('f' * 40)} 2
    $accepted = Resolve-Fixture -Runs @($main) -Jobs @{[long]2=$mainJob}
    if ($accepted.Attempt -ne 3 -or $accepted.SourceCommit -cne $main.head_sha) {
        throw 'Attempt and branch source commit must bind the accepted run.'
    }
    $checks.Add('records the accepted attempt and commit')
    Assert-Accepted 'current run excluded' @{Runs=@($oldRerun,$other);Jobs=@{[long]2=(New-FixtureJob $other)};CurrentRunId=1} 2
    Assert-Rejected 'API failure does not pass' @{Runs=@($first);Jobs=@{[long]1=$job};ApiError=$true} 'Synthetic API failure'
    $unrelated = New-FixtureRun 2 ('c' * 40)
    $unrelated.conclusion = 'failure'
    Assert-Accepted 'exact SHA wins over a later identical tree failure and a large history' @{
        Runs=@($unrelated,$first);Jobs=@{[long]1=$job};Overflow=$true}
    $pendingTarget = New-FixtureRun
    $pendingTarget.status='in_progress';$pendingTarget.conclusion=$null
    Assert-Rejected 'incomplete exact SHA cannot borrow a successful identical tree' @{
        Runs=@($pendingTarget,$main);Jobs=@{[long]2=$mainJob};Overflow=$true} 'latest matching'
    Assert-Rejected 'bounded incomplete history cannot grant tree-only reuse' @{
        Runs=@($first);Jobs=@{[long]1=$job};Overflow=$true;TargetCommit=('f' * 40)} 'bounded evidence search'

    $outcome='Passed'
} finally {
    @{status=$outcome;checks=@($checks)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
Write-Host "CI evidence tests passed: $($checks.Count)"
