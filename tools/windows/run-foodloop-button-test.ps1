param(
  [string]$Port = 'COM7',
  [int]$TimeoutSeconds = 180,
  [string]$LogPath = (Join-Path $env:TEMP 'foodloop-button-test.log')
)

$utf8 = New-Object System.Text.UTF8Encoding($false)
$serial = [System.IO.Ports.SerialPort]::new(
  $Port, 115200, [System.IO.Ports.Parity]::None, 8,
  [System.IO.Ports.StopBits]::One
)
$serial.NewLine = "`n"
$serial.Encoding = $utf8
$serial.ReadTimeout = 200

try {
  $serial.Open()
  Start-Sleep -Milliseconds 500
  $serial.Write("`nfoodloop`n")
  $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)

  while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $output = $serial.ReadExisting()
    if (-not $output) {
      continue
    }

    Add-Content -LiteralPath $logPath -Value $output -Encoding utf8
    if ($output -match 'saved [0-9]+ bytes' -or
        $output -match 'scan failed') {
      break
    }
  }
} finally {
  if ($serial.IsOpen) {
    $serial.Close()
  }
}
