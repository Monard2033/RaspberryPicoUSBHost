[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$sourcePath = Join-Path $projectRoot 'src\main.cpp'
$rcPath = Join-Path $projectRoot 'src\resources.rc'
$distPath = Join-Path $projectRoot 'dist'
$resObjPath = Join-Path $distPath 'resources.o'
$outputPath = Join-Path $distPath 'WirelessKeyboardTray.exe'

$compiler = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
    $fallbackCompiler = 'C:\ProgramData\mingw64\mingw64\bin\g++.exe'
    if (Test-Path -LiteralPath $fallbackCompiler) {
        $compiler = Get-Item -LiteralPath $fallbackCompiler
    } else {
        throw 'g++.exe was not found. Install an x64 MinGW-w64 toolchain or add it to PATH.'
    }
}

$windres = Get-Command 'windres.exe' -ErrorAction SilentlyContinue
if ($null -eq $windres) {
    $fallbackWindres = 'C:\ProgramData\mingw64\mingw64\bin\windres.exe'
    if (Test-Path -LiteralPath $fallbackWindres) {
        $windres = Get-Item -LiteralPath $fallbackWindres
    }
}

New-Item -ItemType Directory -Path $distPath -Force | Out-Null

if ($null -ne $windres -and (Test-Path -LiteralPath $rcPath)) {
    $windresArgs = @(
        '-i', $rcPath,
        '-o', $resObjPath,
        '--input-format=rc',
        '--output-format=coff'
    )
    & $windres.Source @windresArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Resource compilation failed with exit code $LASTEXITCODE."
    }
}

$arguments = @(
    '-std=c++17',
    '-Os',
    '-s',
    '-ffunction-sections',
    '-fdata-sections',
    '-Wall',
    '-Wextra',
    '-Wpedantic',
    '-Werror',
    '-municode',
    '-mwindows',
    '-static',
    '-static-libgcc',
    '-static-libstdc++',
    '-DWINVER=0x0A00',
    '-D_WIN32_WINNT=0x0A00',
    $sourcePath
)

if (Test-Path -LiteralPath $resObjPath) {
    $arguments += $resObjPath
}

$arguments += @(
    '-o',
    $outputPath,
    '-Wl,--gc-sections',
    '-lhid',
    '-lsetupapi',
    '-lshell32',
    '-luser32',
    '-lgdi32',
    '-ladvapi32',
    '-ldwmapi',
    '-luxtheme'
)

& $compiler.Source @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Native build failed with exit code $LASTEXITCODE."
}

$artifact = Get-Item -LiteralPath $outputPath
$hash = Get-FileHash -LiteralPath $outputPath -Algorithm SHA256
Write-Output "Built: $($artifact.FullName)"
Write-Output "Size: $($artifact.Length) bytes"
Write-Output "SHA-256: $($hash.Hash)"
