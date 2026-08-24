$ErrorActionPreference = 'Stop'

$compiler = Get-Command 'g++.exe' -ErrorAction SilentlyContinue
if (-not $compiler) {
    $fallbackCompiler = 'C:\ProgramData\mingw64\mingw64\bin\g++.exe'
    if (Test-Path -LiteralPath $fallbackCompiler) {
        $compilerPath = $fallbackCompiler
    } else {
        throw 'g++.exe was not found. Install x64 MinGW-w64 or add it to PATH.'
    }
} else {
    $compilerPath = $compiler.Source
}

$toolDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$source = Join-Path $toolDirectory 'flash_ota.cpp'
$output = Join-Path $toolDirectory 'flash_ota.exe'

# Static linking: the tool must run anywhere ("zero dependencies") and the
# dynamically linked MinGW build segfaulted at startup on machines whose
# PATH resolves a different runtime than the compiler used.
& $compilerPath -std=c++17 -O2 -Wall -Wextra -Werror -static -static-libgcc -static-libstdc++ `
    $source -o $output -lsetupapi -lhid
if ($LASTEXITCODE -ne 0) {
    throw "flash_ota.exe build failed with exit code $LASTEXITCODE"
}

$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash
$file = Get-Item -LiteralPath $output
Write-Host "Built $($file.FullName)"
Write-Host "Size: $($file.Length) bytes"
Write-Host "SHA-256: $hash"
