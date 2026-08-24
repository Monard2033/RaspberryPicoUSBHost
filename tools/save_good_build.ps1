# Snapshot the current (or a given git ref's) firmware artifacts as a
# validated "good build".  Run this only after hardware validation.
#
#   tools\save_good_build.ps1                                   # working tree
#   tools\save_good_build.ps1 -Ref codex/rp2040-low-power-stage1 -Note "..."
#
# Creates firmware/good/<timestamp>_<sha8>/ with the cable image, the OTA
# payload, a BUILD_INFO.txt (ref, sha, note, SHA-256 of every file, revert
# instructions) and refreshes firmware/good/LATEST.txt as the pointer that
# answers "which build is the Latest Good".
param(
    [string]$Ref = "",
    [string]$Note = ""
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$goodDir = Join-Path $repo 'firmware\good'

$shaArg = if ($Ref) { $Ref } else { 'HEAD' }
$sha = (git -C $repo rev-parse --short "$shaArg").Trim()
if (-not $sha) { throw 'could not resolve git ref/HEAD' }

$stamp = Get-Date -Format 'yyyyMMdd_HHmm'
$name = "${stamp}_${sha}"
$dest = Join-Path $goodDir $name
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$artifacts = @(
    'WirelessKeyboard.uf2',
    'WirelessKeyboard_OTA.bin',
    'WirelessKeyboard_OTA.wkota'
)

foreach ($file in $artifacts) {
    if ($Ref) {
        # cmd redirection is byte-faithful; PowerShell re-encodes binaries
        # and turns native stderr into terminating errors.  A failed git
        # show still creates an empty file, so drop zero-byte results
        # (the artifact simply does not exist on that ref).
        $target = Join-Path $dest $file
        cmd /c "git -C `"$repo`" show `"$($Ref):firmware/$file`" > `"$target`" 2>nul"
        if ((Test-Path -LiteralPath $target) -and
            (Get-Item -LiteralPath $target).Length -eq 0) {
            Remove-Item -LiteralPath $target
        }
    } else {
        $src = Join-Path $repo "firmware\$file"
        if (Test-Path -LiteralPath $src) { Copy-Item -LiteralPath $src -Destination $dest -Force }
    }
}

$hashLines = foreach ($file in $artifacts) {
    $p = Join-Path $dest $file
    if (Test-Path -LiteralPath $p) {
        $h = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
        "$file  SHA-256 $h"
    }
}

$subject = if ($Ref) { (git -C $repo log -1 --format='%s' $Ref).Trim() } else { (git -C $repo log -1 --format='%s' HEAD).Trim() }

$info = @(
    "Latest Good snapshot",
    "Date     : $stamp",
    "Git ref  : $(if ($Ref) { $Ref } else { 'working tree / HEAD' })",
    "Commit   : $sha  $subject",
    "Note     : $Note",
    "",
    "Files:",
    $hashLines,
    "",
    "Revert by cable : BOOTSEL-flash WirelessKeyboard.uf2 (works from any state).",
    "Revert over air : tools\flash_ota.exe only against strict-OTA builds;",
    "                  builds without the strict DFU protocol must be cable-flashed."
)
$infoPath = Join-Path $dest 'BUILD_INFO.txt'
$info | Out-File -LiteralPath $infoPath -Encoding utf8

"LATEST_GOOD=$name" | Out-File -LiteralPath (Join-Path $goodDir 'LATEST.txt') -Encoding utf8

Write-Host "Snapshot saved: $dest"
Write-Host "LATEST.txt now points to $name"
