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
$steps = @('Check version and release tag','Prepare pinned shader compilers',
    'Prepare pinned RenoDX host and dependencies','Build clean Publishing Release x64',
    'Run independent core regression','Verify Publishing isolation and package','Upload installable archive and checksum')
function New-FixtureRun([long]$Id = 1, [string]$Commit = $sha) {
    @{ id=$Id;workflow_id=7;path='.github/workflows/ci.yml';event='push';head_branch='dev'
        head_repository=@{full_name='fixture/repository'};head_sha=$Commit;run_attempt=1
        head_commit=@{id=$Commit;tree_id=$tree}
        status='completed';conclusion='success';updated_at="2026-11-15T00:00:0${Id}Z" }
}
function New-FixtureJob($Run, [string[]]$Names = $steps) {
    @{run_id=$Run.id;head_sha=$Run.head_sha;status='completed';conclusion='success'
        steps=@($Names | ForEach-Object { @{name=$_;status='completed';conclusion='success'} })}
}
function Resolve-Fixture([object[]]$Runs, [hashtable]$Jobs, [long]$CurrentRunId = 0, [switch]$ApiError, [switch]$Overflow) {
    $request = {
        param($path)
        if ($ApiError) { throw 'Synthetic API failure.' }
        if ($path -ceq '/repos/fixture/repository/actions/workflows/ci.yml') { return @{id=7;path='.github/workflows/ci.yml'} }
        if ($path -match '/runs\?per_page=100&page=([0-9]+)$') {
            return @{total_count=$(if ($Overflow) {501} else {$Runs.Count});workflow_runs=$Runs}
        }
        if ($path -match '/runs/([0-9]+)/attempts/([0-9]+)/jobs\?per_page=100$') {
            $items = @($Jobs[[long]$Matches[1]])
            return @{total_count=$items.Count;jobs=$items}
        }
        throw "Unexpected mock API request: $path"
    }.GetNewClosure()
    Get-CiCoreEvidence -Repository fixture/repository -Tree $tree -CurrentRunId $CurrentRunId -Request $request
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

function Assert-GateCase {
    param(
        [string]$Name,
        [string]$RefName = '42/merge',
        [string]$EventName = 'pull_request',
        [string]$EvidenceResult = 'success',
        [string]$RunCore = 'true',
        [string]$WindowsResult = 'success',
        [string]$Rejection
    )

    try {
        Assert-CiGate -RefName $RefName -EventName $EventName -EvidenceResult $EvidenceResult `
            -RunCore $RunCore -WindowsResult $WindowsResult
    } catch {
        if (-not $Rejection -or $_.Exception.Message -notlike "$Rejection*") { throw }
        $checks.Add("gate: $Name")
        return
    }
    if ($Rejection) { throw "Gate accepted incorrectly: $Name" }
    $checks.Add("gate: $Name")
}

$outcome = 'Failed'
try {
    $first = New-FixtureRun
    $job = New-FixtureJob $first
    Assert-Accepted 'same tree direct core' @{Runs=@($first);Jobs=@{[long]1=$job}}
    $other = New-FixtureRun 2 ('c' * 40)
    Assert-Accepted 'different commit identical root tree' @{Runs=@($other);Jobs=@{[long]2=(New-FixtureJob $other)}} 2
    $different = New-FixtureRun
    $different.head_commit.tree_id='d' * 40
    Assert-Rejected 'different root tree' @{Runs=@($different);Jobs=@{[long]1=$job}} 'No successful'
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
    $missingCore = New-FixtureJob $first @($steps | Where-Object { $_ -cne 'Run independent core regression' })
    Assert-Rejected 'missing core step' @{Runs=@($first);Jobs=@{[long]1=$missingCore}} 'no successful core'
    $skippedCore = New-FixtureJob $first
    ($skippedCore.steps | Where-Object name -CEQ 'Run independent core regression').conclusion='skipped'
    Assert-Rejected 'skipped core step' @{Runs=@($first);Jobs=@{[long]1=$skippedCore}} 'no successful core'
    $wrongJob = New-FixtureJob $first
    $wrongJob.head_sha='e' * 40
    Assert-Rejected 'wrong job commit' @{Runs=@($first);Jobs=@{[long]1=$wrongJob}} 'no successful core'
    $main = New-FixtureRun 2 ('c' * 40)
    $main.head_branch='main'
    $mainJob = New-FixtureJob $main @('Resolve exact-tree core evidence')
    Assert-Accepted 'main reuse traces actual older core' @{Runs=@($main,$first);Jobs=@{[long]1=$job;[long]2=$mainJob}}
    Assert-Rejected 'main reuse is not actual core' @{Runs=@($main);Jobs=@{[long]2=$mainJob}} 'No successful'
    Assert-Accepted 'current pending main run excluded' @{Runs=@($oldRerun,$other);Jobs=@{[long]2=(New-FixtureJob $other)};CurrentRunId=1} 2
    Assert-Rejected 'API failure does not pass' @{Runs=@($first);Jobs=@{[long]1=$job};ApiError=$true} 'Synthetic API failure'
    Assert-Rejected 'bounded incomplete history does not pass' @{Runs=@($first);Jobs=@{[long]1=$job};Overflow=$true} 'bounded evidence search'

    $mainReuse = @{RefName='main';EventName='push';RunCore='false';WindowsResult='skipped'}
    $manualReuse = @{RefName='main';EventName='workflow_dispatch';RunCore='false';WindowsResult='skipped'}
    Assert-GateCase 'PR ran independent core'
    Assert-GateCase 'main fallback ran independent core' -RefName main -EventName push
    Assert-GateCase 'dev ran independent core' -RefName dev -EventName push
    Assert-GateCase 'main push reused exact-tree evidence' @mainReuse
    Assert-GateCase 'main dispatch reused exact-tree evidence' @manualReuse
    Assert-GateCase 'PR cannot reuse even with main ref' -RefName main -RunCore false -WindowsResult skipped -Rejection 'Invalid core'
    Assert-GateCase 'dev cannot reuse' -RefName dev -EventName push -RunCore false -WindowsResult skipped -Rejection 'Invalid core'
    foreach ($state in @('failure','cancelled','skipped','')) {
        Assert-GateCase "main reuse with evidence result '$state'" @mainReuse -EvidenceResult $state -Rejection 'Expected successful evidence'
        Assert-GateCase "PR with Windows result '$state'" -WindowsResult $state -Rejection 'Invalid core'
    }
    foreach ($state in @('failure','cancelled','success')) {
        Assert-GateCase "main reuse with Windows result '$state'" -RefName main -EventName push -RunCore false `
            -WindowsResult $state -Rejection 'Invalid core'
    }
    foreach ($decision in @('false','unknown','')) {
        Assert-GateCase "PR with run_core '$decision' despite green Windows" -RunCore $decision -Rejection 'Invalid core'
    }
    Assert-GateCase 'main with missing reuse output' -RefName main -EventName push -RunCore '' `
        -WindowsResult skipped -Rejection 'Invalid core'
    $outcome='Passed'
} finally {
    @{status=$outcome;checks=@($checks)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'result.json') -Encoding utf8
    Complete-TestOutput $OutputDirectory -Outcome $outcome
}
Write-Host "CI evidence tests passed: $($checks.Count)"
