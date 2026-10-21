<#
.SYNOPSIS
    원격(origin)에는 S1 · Server · Tools · .github · README.md · .gitignore 만 올린다. 로컬 master 는 문서까지 전부 유지한다.

.DESCRIPTION
    로컬 master 를 임시 폴더에 복제 → git filter-repo 로 여섯 경로만 남김 → 원격 master 에 푸시.
    .github 는 CI(server-tests.yml — 서버 빌드 · 테스트 · 스트레스 100회)가 원격에서 계속 돌게 하려고 넣는다.
    Tools 는 2026-10-21 에 넣었다 (도구 통합 - 워크플로 원본 · DB 스크립트 · 검증 툴 · ServerMonitor). 옛 tools/ 히스토리는 넣지 않는다 -
    원격에서는 Tools/ 가 통합 커밋에서 처음 나타난다 (Server/Tools/ 는 Server/ 라 그 전 히스토리가 남는다).

    filter-repo 는 입력 히스토리가 같으면 결과 해시도 같다 (2026-09-28 두 번 돌려 확인).
    로컬 master 를 재작성하지 않는 한 매번 fast-forward 푸시가 된다.
    🔴 허용 경로를 바꾸면 결과 해시가 전부 바뀐다 — 그때는 -ForceWithLease 로 한 번 더 덮어써야 한다.

    🔴 로컬에서 그냥 `git push` 하면 문서·.claude 까지 올라간다. 그래서 origin 의 pushurl 을 막아 두고
       (git config remote.origin.pushurl), 이 스크립트는 URL 을 직접 지정해 푸시한다.

.PARAMETER ForceWithLease
    최초 1회(히스토리 재작성)에만 쓴다. 현재 원격 master 커밋 해시를 넘기면 원격이 그 상태일 때만 덮어쓴다.

.PARAMETER DryRun
    필터와 검사까지만 하고 푸시하지 않는다.

.EXAMPLE
    .\Tools\repo\push-remote.ps1 -DryRun
    .\Tools\repo\push-remote.ps1
    .\Tools\repo\push-remote.ps1 -ForceWithLease a1223f050748b7715408f3c493713d1458c0a755   # 최초 1회
#>
param(
    [string] $ForceWithLease,
    [switch] $DryRun
)

$ErrorActionPreference = 'Stop'

$Repo    = 'C:\Server\MMO'
$Url     = 'https://github.com/HyeonjungYun/for-be-the-king.git'
$Allowed = @('S1', 'Server', 'Tools', '.github', 'README.md', '.gitignore')

$work = Join-Path $env:TEMP ('mmo-push-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))

git clone -q --no-local --single-branch --branch master $Repo $work
if ($LASTEXITCODE -ne 0) { throw 'clone 실패' }

$filterArgs = @('filter-repo', '--quiet', '--path', 'S1/', '--path', 'Server/', '--path', 'Tools/', '--path', '.github/', '--path', 'README.md', '--path', '.gitignore')
git -C $work @filterArgs
if ($LASTEXITCODE -ne 0) { throw 'filter-repo 실패' }

$head  = git -C $work rev-parse HEAD
$count = git -C $work rev-list --count HEAD
$roots = git -C $work log --all --format= --name-only |
    ForEach-Object { ($_ -split '/')[0] } | Where-Object { $_ } | Sort-Object -Unique

Write-Host "filtered HEAD=$head commits=$count roots=$($roots -join ', ')"

$bad = $roots | Where-Object { $_ -notin $Allowed }
if ($bad) { throw "허용 밖 경로가 히스토리에 남았다: $($bad -join ', ')" }

if ($DryRun) {
    Write-Host "DryRun — 푸시하지 않음. 작업 폴더: $work"
    return
}

if ($ForceWithLease) {
    git -C $work push "--force-with-lease=refs/heads/master:$ForceWithLease" $Url HEAD:refs/heads/master
}
else {
    # fast-forward 만 한다. 거절되면 원격이 갈라진 것 — 강제로 덮지 말고 원인부터 볼 것
    git -C $work push $Url HEAD:refs/heads/master
}
if ($LASTEXITCODE -ne 0) { throw '푸시 실패' }

Write-Host "pushed $head -> $Url master"

# 복제본이 한 번에 ~350MB 라 성공하면 치운다. 실패하면 남겨 두고 원인을 본다
Remove-Item -Recurse -Force $work
