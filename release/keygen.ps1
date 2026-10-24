# Makes the release signing keys - once, for good (RemoteUpdate.h installs only manifests signed with them):
#   %USERPROFILE%\.furnace-monitor\release-key.bin         - publish.ps1 signs with it
#   %USERPROFILE%\.furnace-monitor\release-key-backup.bin  - for when the first one is lost: move it off this PC
# Both are ECDSA P-256 private keys. Their public halves go into ReleaseKeys.h, built into the firmware, with a
# manifest signed by the first key that the ESP checks itself on (see RemoteUpdate.h). Keep copies of both files
# somewhere safe (a USB stick, a password manager): without either key, the ESPs out there take updates only over a
# USB cable or from a file in the home WiFi. Never put them in the repo.
# Refuses to run when the keys exist - new keys would lock out every ESP that has the old ones.
# Usage:  powershell -ExecutionPolicy Bypass -File release\keygen.ps1

$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\sign.ps1"
$sketch = Split-Path -Parent $PSScriptRoot
$files  = 'release-key.bin', 'release-key-backup.bin' | ForEach-Object { Join-Path $ReleaseKeyDir $_ }
foreach ($f in $files) { if (Test-Path $f) { throw "$f exists - the keys are made once; restore a lost one from its copy" } }
New-Item -ItemType Directory -Force $ReleaseKeyDir | Out-Null

foreach ($f in $files) {
  $p = New-Object Security.Cryptography.CngKeyCreationParameters
  $p.ExportPolicy = [Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
  # A real null name (plain $null becomes "" here) = an in-memory key, never stored by Windows itself.
  $key = [Security.Cryptography.CngKey]::Create([Security.Cryptography.CngAlgorithm]::ECDsaP256, [NullString]::Value, $p)
  [IO.File]::WriteAllBytes($f, $key.Export([Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
}

$zeros = '0' * 64
$test = New-Manifest 1 1 $zeros '-' '-' 0 (Get-ManifestSignature $files[0] 1 1 $zeros 0) ([ordered]@{})
$header = @"
// The public halves of the release signing keys (release/keygen.ps1): RemoteUpdate.h installs a release only when
// its manifest is signed with one of them. The first signs every release; the second is the spare, kept off the
// release PC. Changing these locks out the ESPs that have the old ones - they take the new keys only over USB.

#pragma once

static const char* const RELEASE_KEYS[] = {
  "$(Get-ReleasePublicKey $files[0])",
  "$(Get-ReleasePublicKey $files[1])",
};

// A manifest in publish.ps1's format, signed with the first key: the ESP checks its verifier on it (and on a
// forged copy) before it demands signatures.
static const char RELEASE_SELFTEST[] = R"($test)";
"@
[IO.File]::WriteAllText("$sketch\ReleaseKeys.h", $header.Replace("`r`n", "`n") + "`n")
"keys written to $ReleaseKeyDir - copy both files somewhere safe, then move release-key-backup.bin off this PC"
