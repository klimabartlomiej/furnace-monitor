# Listens to the Geco GH11TB controller's RS485 bus (panel terminals 21 A / 22 B) through a USB-RS485 adapter and
# writes down what it hears. It NEVER sends anything: the port is opened and only read - no request goes out, so
# nothing on the controller can change. decode.py turns the capture into the controller's state and temperatures;
# docs/geco-gh11tb-bus.md has the protocol.
#
# Capture: one line per piece read, "<ms since start> <hex bytes>"; the pieces follow the idle gaps on the bus only
# roughly (the decoder finds the frames by their own structure, not by the gaps).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File sniff.ps1 [-Port COM7] [-Baud 9600] [-Seconds 600] [-Out capture.txt]
#   powershell -ExecutionPolicy Bypass -File sniff.ps1 -Scan [-ScanSeconds 15]
#     -Scan listens at each usual speed in turn and counts the GH11 frames (checksum right) it hears - for another
#     controller or when nothing readable comes.

param(
  [string]$Port = 'COM7',
  [int]$Baud = 9600,
  [int]$Seconds = 600,
  [string]$Out,
  [switch]$Scan,
  [int]$ScanSeconds = 15
)

$ErrorActionPreference = 'Stop'

# GH11 frames (bytes inverted on the wire): 0x40 or 0x50, eight bytes, then sum(bytes 0..8) - 0x10.
function Count-Frames([byte[]]$b) {
  $n = 0
  for ($i = 0; $i + 10 -le $b.Length; $i++) {
    $first = $b[$i] -bxor 0xFF
    if ($first -ne 0x40 -and $first -ne 0x50) { continue }
    $sum = 0
    for ($k = 0; $k -lt 9; $k++) { $sum += $b[$i + $k] -bxor 0xFF }
    if ((($sum - 0x10) -band 0xFF) -eq ($b[$i + 9] -bxor 0xFF)) { $n++ }
  }
  $n
}

# Opens the port for reading only. RTS/DTR stay low: the adapter switches to sending only when data is written,
# and this script writes none.
function Open-Port([int]$baud) {
  $p = New-Object System.IO.Ports.SerialPort $Port, $baud, 'None', 8, 'One'
  $p.DtrEnable = $false
  $p.RtsEnable = $false
  $p.ReadBufferSize = 1MB
  $p.Open()
  $p
}

# Reads for $seconds; calls $onPiece(ms, bytes) for every piece. Returns all bytes.
function Listen([System.IO.Ports.SerialPort]$p, [int]$seconds, [scriptblock]$onPiece) {
  $all = New-Object System.Collections.Generic.List[byte]
  $clock = [Diagnostics.Stopwatch]::StartNew()
  while ($clock.Elapsed.TotalSeconds -lt $seconds) {
    $n = $p.BytesToRead
    if ($n -gt 0) {
      $buf = New-Object byte[] $n
      $got = $p.Read($buf, 0, $n)
      if ($got -lt $n) { $buf = $buf[0..($got - 1)] }
      $all.AddRange([byte[]]$buf)
      if ($onPiece) { & $onPiece $clock.ElapsedMilliseconds $buf }
    } else {
      Start-Sleep -Milliseconds 2
    }
  }
  [byte[]]$all.ToArray()
}

if ($Scan) {
  foreach ($baud in 9600, 19200, 38400, 57600, 115200) {
    $p = Open-Port $baud
    try { $bytes = Listen $p $ScanSeconds $null } finally { $p.Close() }
    "{0,6} baud: {1,6} bytes, {2,5} GH11 frames" -f $baud, $bytes.Length, (Count-Frames $bytes)
  }
  return
}

if (-not $Out) { $Out = Join-Path $PSScriptRoot ('capture-{0:yyyyMMdd-HHmmss}.txt' -f (Get-Date)) }
$writer = New-Object System.IO.StreamWriter($Out, $false, (New-Object System.Text.UTF8Encoding($false)))
$writer.WriteLine("# port $Port, $Baud 8N1, started $((Get-Date).ToString('yyyy-MM-dd HH:mm:ss'))")
$p = Open-Port $Baud
"Listening on $Port at $Baud 8N1 for $Seconds s (read only) -> $Out"
try {
  $bytes = Listen $p $Seconds {
    param($ms, $piece)
    $writer.WriteLine(('{0} {1}' -f $ms, (($piece | ForEach-Object { $_.ToString('x2') }) -join ' ')))
    $writer.Flush()
  }
} finally {
  $p.Close()
  $writer.Close()
}
"Heard $($bytes.Length) bytes, $(Count-Frames $bytes) GH11 frames. Decode: python -I decode.py `"$Out`""
