# Flash an already-built ESP32-C6 Super Mini image on COM7, then open monitor.
param(
    [switch]$ManualBoot
)

$ErrorActionPreference = "Stop"

$profile = "C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1"
if (Test-Path $profile) {
    . $profile
}

Set-Location $PSScriptRoot
$env:ESPPORT = "COM7"

$buildDir = "build-c6"
$beforeReset = "default_reset"

if ($ManualBoot) {
    Write-Host "Pon el C6 en modo bootloader: manten BOOT, pulsa RST, suelta RST y luego suelta BOOT."
    Read-Host "Pulsa Enter cuando este en bootloader"
    $beforeReset = "no_reset"
}

Push-Location $buildDir
try {
    esptool.py --chip esp32c6 -p COM7 -b 115200 --before $beforeReset --after hard_reset write_flash "@flash_args"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

idf.py -B $buildDir -DSDKCONFIG=$buildDir/sdkconfig -DIDF_TARGET=esp32c6 monitor
