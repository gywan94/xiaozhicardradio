param([string]$SdkDir='D:/Espressif/frameworks/esp-idf-v6.0-dev-xiaozhi-card')
$ErrorActionPreference='Stop'
$pythonDir='C:/Users/gywan94/AppData/Local/Programs/Python/Python312'
if (!(Test-Path "$SdkDir/export.ps1")) { throw 'Required ESP-IDF SDK is missing; see SMS_README.md.' }
$env:PATH="$pythonDir;$pythonDir/Scripts;"+$env:PATH
$env:IDF_TOOLS_PATH='D:/Espressif/.espressif'
$env:PYTHONIOENCODING='utf-8'
& "$SdkDir/export.ps1"
Set-Location (Split-Path $PSScriptRoot -Parent)
idf.py build
exit $LASTEXITCODE
