# Build script for the OTA flasher executables (protocol v2).
# The GUI build includes a real frozen Qt smoke test before either output is
# copied over the currently installed executable.
$ErrorActionPreference = "Stop"
$py = "$env:USERPROFILE\.pico-sdk\python\3.13.7\python.exe"
$tools = $PSScriptRoot
$buildId = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$stage = Join-Path $tools "dist\qt-clean-$buildId"
$work = Join-Path $tools "_pyi_build\qt-clean-$buildId"
$pyRoot = Split-Path -Parent $py
$pyScripts = Join-Path $pyRoot "Scripts"
$qtBin = Join-Path $pyRoot "Lib\site-packages\PyQt6\Qt6\bin"
$windowsRoot = $env:WINDIR
$oldPath = $env:PATH
$oldQtPlatform = $env:QT_QPA_PLATFORM
$oldQtPluginPath = $env:QT_PLUGIN_PATH
$oldQmlImportPath = $env:QML2_IMPORT_PATH

if (-not (Test-Path -LiteralPath $py -PathType Leaf)) {
    throw "Bundled Python not found: $py"
}

Push-Location $tools
try {
    # PyInstaller resolves native dependencies through PATH.  The inherited
    # developer/Codex PATH can expose Poppler's ICU78 DLL, whose exports are
    # incompatible with Qt6Core's unversioned ICU imports.  Keep only the
    # Python, Qt and Windows system locations for this reproducible build.
    $env:PATH = "$pyRoot;$pyScripts;$qtBin;$windowsRoot\System32;$windowsRoot"
    $env:QT_QPA_PLATFORM = "offscreen"
    $env:QT_PLUGIN_PATH = $null
    $env:QML2_IMPORT_PATH = $null
    New-Item -ItemType Directory -Path (Split-Path -Parent $stage) -Force | Out-Null
    New-Item -ItemType Directory -Path (Split-Path -Parent $work) -Force | Out-Null
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    New-Item -ItemType Directory -Path $stage | Out-Null

    & $py -m PyInstaller --clean --noconfirm --onefile --name flash_ota_cmd `
        flash_ota_cmd.py --distpath $stage --workpath $work --specpath $work
    if ($LASTEXITCODE -ne 0) { throw "CLI PyInstaller build failed ($LASTEXITCODE)" }

    & $py -m PyInstaller --clean --noconfirm --onefile --windowed --name FLASH_OTA `
        flash_ota_gui.py --distpath $stage --workpath $work --specpath $work
    if ($LASTEXITCODE -ne 0) { throw "GUI PyInstaller build failed ($LASTEXITCODE)" }

    $gui = Join-Path $stage "FLASH_OTA.exe"
    if (-not (Test-Path -LiteralPath $gui -PathType Leaf)) {
        throw "GUI build did not produce $gui"
    }
    $smoke = Start-Process -FilePath $gui -ArgumentList "--self-test" `
        -WindowStyle Hidden -PassThru
    if (-not $smoke.WaitForExit(30000)) {
        $smoke.Kill()
        throw "Frozen GUI Qt self-test timed out after 30 seconds"
    }
    if ($smoke.ExitCode -ne 0) {
        throw "Frozen GUI Qt self-test failed with exit code $($smoke.ExitCode)"
    }
    $env:QT_QPA_PLATFORM = "windows"
    $smokeWindows = Start-Process -FilePath $gui -ArgumentList "--self-test" `
        -WindowStyle Hidden -PassThru
    if (-not $smokeWindows.WaitForExit(30000)) {
        $smokeWindows.Kill()
        throw "Frozen GUI Qt Windows-platform self-test timed out after 30 seconds"
    }
    if ($smokeWindows.ExitCode -ne 0) {
        throw "Frozen GUI Qt Windows-platform self-test failed with exit code $($smokeWindows.ExitCode)"
    }

    Copy-Item -LiteralPath (Join-Path $stage "flash_ota_cmd.exe") `
        -Destination (Join-Path $tools "flash_ota_cmd.exe") -Force
    Copy-Item -LiteralPath $gui -Destination (Join-Path $tools "FLASH_OTA.exe") -Force
    Write-Host "Frozen GUI Qt self-test passed; executables installed in $tools"
}
finally {
    $env:PATH = $oldPath
    $env:QT_QPA_PLATFORM = $oldQtPlatform
    $env:QT_PLUGIN_PATH = $oldQtPluginPath
    $env:QML2_IMPORT_PATH = $oldQmlImportPath
    Pop-Location
}
