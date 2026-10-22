# Publishes a firmware for the ESP's self-update (RemoteUpdate.h):
#   1. FW_VERSION in PiecTemp.ino must be higher than the published one - bump it before running,
#   2. rebuilds the web app, compiles, checks the size against the 1.69 MB app slot,
#   3. writes release/firmware-v<N>.bin (a new name every time, so no CDN cache serves an old file)
#      and release/manifest.json (version, size, sha256, url),
#   4. commits and pushes - the ESP picks it up at its next hourly check, or right away with "@u" / the web app.
# Usage:  powershell -ExecutionPolicy Bypass -File release\publish.ps1 [-NoPush]

param([switch]$NoPush)

$ErrorActionPreference = 'Stop'
$sketch  = Split-Path -Parent $PSScriptRoot
$rawBase = 'https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release'
$cli     = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$build   = Join-Path $env:TEMP 'furnace-monitor-build'
$appSlot = 0x1B0000

$version = [int]([regex]::Match((Get-Content "$sketch\PiecTemp.ino" -Raw), 'FW_VERSION\s*=\s*(\d+)').Groups[1].Value)
$manifestPath = "$PSScriptRoot\manifest.json"
if (Test-Path $manifestPath) {
  $published = (Get-Content $manifestPath -Raw | ConvertFrom-Json).version
  if ($version -le $published) { throw "FW_VERSION $version is not higher than the published v$published - bump it first" }
}

& powershell -NoProfile -ExecutionPolicy Bypass -File "$sketch\web\build.ps1"
& $cli compile --config-file "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml" `
  --fqbn 'esp32:esp32:esp32:PartitionScheme=min_spiffs' --build-path $build $sketch | Select-Object -Last 2
if ($LASTEXITCODE) { throw 'compile failed' }

$bin = "$build\PiecTemp.ino.bin"
$size = (Get-Item $bin).Length
if ($size -gt $appSlot) { throw "firmware is $size bytes, the app slot holds $appSlot" }

Get-ChildItem $PSScriptRoot -Filter 'firmware-v*.bin' | Remove-Item
$name = "firmware-v$version.bin"
Copy-Item $bin "$PSScriptRoot\$name"
$sha = (Get-FileHash "$PSScriptRoot\$name" -Algorithm SHA256).Hash.ToLower()
$manifest = [ordered]@{ version = $version; size = $size; sha256 = $sha; url = "$rawBase/$name" } | ConvertTo-Json -Compress
[IO.File]::WriteAllText($manifestPath, $manifest)
"published v$version ($size bytes)"

if (-not $NoPush) {
  git -C $sketch add -A
  git -C $sketch commit -m "release: firmware v$version"
  git -C $sketch push
}
