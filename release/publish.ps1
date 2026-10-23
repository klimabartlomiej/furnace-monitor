# Publishes a firmware for the ESP's self-update (RemoteUpdate.h):
#   1. FW_VERSION in PiecTemp.ino must be higher than the published one - bump it before running,
#   2. rebuilds the web app, compiles, checks the size against the 1.69 MB app slot,
#   3. writes release/firmware-v<N>.bin (a new name every time, so no CDN cache serves an old file)
#      and release/manifest.json (version, size, sha256, url, the release notes from CHANGES in web/index.html),
#   4. commits and pushes - the ESP sees it at its next hourly check (or "@u" / the web app) and the web app offers
#      it with its notes; it is installed when someone taps "Zainstaluj".
# -Forced: a release everyone must have (a security or a data-losing fix). The manifest's "min" becomes this
# version: every ESP below it installs the newest release by itself and the web app gives no "later". "min" stays
# in later normal releases, so an ESP that skipped the forced one still has to update.
# Usage:  powershell -ExecutionPolicy Bypass -File release\publish.ps1 [-Forced] [-NoPush]

param([switch]$Forced, [switch]$NoPush)

$ErrorActionPreference = 'Stop'
$sketch  = Split-Path -Parent $PSScriptRoot
$rawBase = 'https://raw.githubusercontent.com/klimabartlomiej/furnace-monitor/main/release'
$mirrorBase = 'https://cdn.jsdelivr.net/gh/klimabartlomiej/furnace-monitor@main/release'   # another CDN, same file
$cli     = "$env:LOCALAPPDATA\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe"
$build   = Join-Path $env:TEMP 'furnace-monitor-build'
$appSlot = 0x1B0000

$version = [int]([regex]::Match((Get-Content "$sketch\PiecTemp.ino" -Raw), 'FW_VERSION\s*=\s*(\d+)').Groups[1].Value)
$manifestPath = "$PSScriptRoot\manifest.json"
$min = 0
if (Test-Path $manifestPath) {
  $previous = Get-Content $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
  if ($version -le $previous.version) { throw "FW_VERSION $version is not higher than the published v$($previous.version) - bump it first" }
  if ($previous.min) { $min = [int]$previous.min }
}
if ($Forced) { $min = $version }

# Release notes for the web app's "new version" dialog: the CHANGES entries of web/index.html.
$html = Get-Content "$sketch\web\index.html" -Raw -Encoding UTF8
$block = [regex]::Match($html, '(?s)const CHANGES = \{(.*?)\n\};').Groups[1].Value
$notes = [ordered]@{}
foreach ($m in [regex]::Matches($block, "(?m)^\s*(\d+): '((?:[^'\\]|\\.)*)',?\s*$")) {
  $notes[$m.Groups[1].Value] = $m.Groups[2].Value -replace "\\'", "'"
}
if (-not $notes.Contains("$version")) { throw "web/index.html has no CHANGES note for v$version - add one first" }

& powershell -NoProfile -ExecutionPolicy Bypass -File "$sketch\web\build.ps1"
& $cli compile --config-file "$env:USERPROFILE\.arduinoIDE\arduino-cli.yaml" `
  --fqbn 'esp32:esp32:esp32:PartitionScheme=min_spiffs' --build-path $build $sketch | Select-Object -Last 2
if ($LASTEXITCODE) { throw 'compile failed' }

$bin = "$build\PiecTemp.ino.bin"
$size = (Get-Item $bin).Length
if ($size -gt $appSlot) { throw "firmware is $size bytes, the app slot holds $appSlot" }
# The partition table can only change over USB - warn while there is still room to slim down.
if ($size -gt $appSlot * 0.97) { Write-Warning ("firmware uses {0:P1} of the app slot - {1:N0} bytes left" -f ($size / $appSlot), ($appSlot - $size)) }

Get-ChildItem $PSScriptRoot -Filter 'firmware-v*.bin' | Remove-Item
$name = "firmware-v$version.bin"
Copy-Item $bin "$PSScriptRoot\$name"
$sha = (Get-FileHash "$PSScriptRoot\$name" -Algorithm SHA256).Hash.ToLower()
# The notes go last: the ESP's tiny reader takes the first "version", "size", "sha256" and "url" it finds.
$manifest = [ordered]@{ version = $version; size = $size; sha256 = $sha; url = "$rawBase/$name"; mirror = "$mirrorBase/$name"; min = $min; notes = $notes } | ConvertTo-Json -Compress
[IO.File]::WriteAllText($manifestPath, $manifest)
"published v$version ($size bytes)$(if ($Forced) { ' - FORCED: every older ESP installs it by itself' })"

if (-not $NoPush) {
  git -C $sketch add -A
  git -C $sketch commit -m "release: firmware v$version"
  git -C $sketch push
}
