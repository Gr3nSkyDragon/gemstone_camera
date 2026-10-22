# Builds (and optionally flashes) the firmware with a local ESP-IDF 5.2 install.
#   .\build.ps1                          build only
#   .\build.ps1 -Flash auto              build, flash the board it finds, open the serial monitor
#   .\build.ps1 -Flash COM8              same, on a specific port
#   .\build.ps1 -Flash COM8 -NoMonitor   build and flash only
#   .\build.ps1 -Menuconfig              settings menu (WiFi, camera clock)
#   .\build.ps1 -Idf D:\Espressif        ESP-IDF installed somewhere unusual
param([string]$Flash = "", [switch]$NoMonitor, [switch]$Menuconfig, [string]$Idf = "")

# Find the ESP-IDF install: -Idf, then IDF_TOOLS_PATH, then the installer's usual spots
$candidates = @($Idf, $env:IDF_TOOLS_PATH, "C:\Espressif", "D:\Espressif", "E:\Espressif") | Where-Object { $_ }
$root = $candidates | Where-Object { Test-Path "$_\frameworks" } | Select-Object -First 1
if (-not $root) {
    Write-Error "ESP-IDF not found. Install ESP-IDF 5.2 (see README), or pass -Idf <folder>."
    exit 1
}
$framework = Get-ChildItem "$root\frameworks" -Directory -Filter "esp-idf-v5.2*" | Sort-Object Name -Descending | Select-Object -First 1
$pyEnv = Get-ChildItem "$root\python_env" -Directory -Filter "idf5.2_py*_env" | Sort-Object Name -Descending | Select-Object -First 1
if (-not $framework -or -not $pyEnv) {
    Write-Error "Found $root but no ESP-IDF 5.2 in it. Install v5.2.x with the Espressif installer."
    exit 1
}

$env:IDF_TOOLS_PATH = $root
$env:IDF_PATH = $framework.FullName
# export.ps1 otherwise may pick up a system Python that has no IDF env
$env:IDF_PYTHON_ENV_PATH = $pyEnv.FullName
$env:PATH = "$env:IDF_PYTHON_ENV_PATH\Scripts;$env:PATH"
. "$env:IDF_PATH\export.ps1" | Out-Null

Set-Location $PSScriptRoot
if (-not (Test-Path "sdkconfig")) { idf.py set-target esp32s3 }

if ($Menuconfig) {
    idf.py menuconfig
    exit $LASTEXITCODE
}

idf.py build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if ($Flash -eq "auto") {
    # Espressif native USB (303A), WCH CH340/CH343 (1A86) or Silicon Labs CP210x (10C4)
    $port = Get-CimInstance Win32_PnPEntity |
        Where-Object { $_.DeviceID -match "VID_(303A|1A86|10C4)" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } } | Select-Object -First 1
    if (-not $port) {
        Write-Error "No ESP32 board found. Plug it in, or pass the port: -Flash COM8"
        exit 1
    }
    Write-Host "Using $port"
    $Flash = $port
}

if ($Flash -and $NoMonitor) { idf.py -p $Flash flash }
elseif ($Flash) { idf.py -p $Flash flash monitor }
