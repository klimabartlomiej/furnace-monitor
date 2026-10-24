# Release signing, shared by publish.ps1 and keygen.ps1 (dot-sourced). A manifest is signed over
# "furnace-fw|<version>|<size>|<sha256>|<min>" - exactly the values RemoteUpdate.h acts on - with an ECDSA P-256
# key (SHA-256, the signature as r||s in 128 hex digits). The key files are Windows ECCPRIVATEBLOBs: magic, length,
# X, Y, d.

$ReleaseKeyDir = Join-Path $env:USERPROFILE '.furnace-monitor'

function ConvertTo-Hex([byte[]]$bytes) { -join ($bytes | ForEach-Object { $_.ToString('x2') }) }

# The uncompressed public point "04 X Y" as ReleaseKeys.h holds it.
function Get-ReleasePublicKey([string]$keyFile) { '04' + (ConvertTo-Hex ([IO.File]::ReadAllBytes($keyFile)[8..71])) }

function Get-ManifestSignature([string]$keyFile, [int]$version, [long]$size, [string]$sha, [int]$min) {
  $key = [Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($keyFile), [Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
  $ecdsa = New-Object Security.Cryptography.ECDsaCng $key
  $ecdsa.HashAlgorithm = [Security.Cryptography.CngAlgorithm]::Sha256
  ConvertTo-Hex $ecdsa.SignData([Text.Encoding]::ASCII.GetBytes("furnace-fw|$version|$size|$($sha.ToLower())|$min"))
}

# The notes go last: the ESP's tiny reader takes the first "version", "size", "sha256", "min", "sig" and "url" it finds.
function New-Manifest([int]$version, [long]$size, [string]$sha, [string]$url, [string]$mirror, [int]$min, [string]$sig, $notes) {
  [ordered]@{ version = $version; size = $size; sha256 = $sha; url = $url; mirror = $mirror; min = $min; sig = $sig; notes = $notes } | ConvertTo-Json -Compress
}
