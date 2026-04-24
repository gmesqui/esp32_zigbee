# Uso (desde la raiz del repo): .\idf-with-com7.ps1 build | flash | monitor | flash monitor | ...
# Tras flashear: .\flash-monitor.ps1  (flash + monitor en la misma sesion)
# Fija el puerto serie para esptool / idf.py (ESPPORT).
$env:ESPPORT = "COM7"
if (Test-Path "C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1") {
    . "C:\Espressif\tools\Microsoft.v5.5.3.PowerShell_profile.ps1"
}
Set-Location $PSScriptRoot
idf.py @args
