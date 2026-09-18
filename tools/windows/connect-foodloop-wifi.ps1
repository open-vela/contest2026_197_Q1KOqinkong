param(
  [string]$Port = 'COM7',
  [string]$Ssid = '',
  [string]$Bssid = '',
  [string]$Frequency = ''
)

$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::InputEncoding = $utf8
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8

$serial = [System.IO.Ports.SerialPort]::new(
  $Port, 115200, [System.IO.Ports.Parity]::None, 8,
  [System.IO.Ports.StopBits]::One
)
$serial.NewLine = "`n"
$serial.Encoding = $utf8
$serial.DtrEnable = $false
$serial.RtsEnable = $false

function Read-Until {
  param(
    [string]$Prompt,
    [int]$TimeoutMs = 10000,
    [bool]$Display = $true
  )

  $buffer = [System.Text.StringBuilder]::new()
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $text = $serial.ReadExisting()
    if ($text) {
      [void]$buffer.Append($text)
      if ($Display) { Write-Host $text -NoNewline }
      if ($buffer.ToString().Contains($Prompt)) { return $true }
    }
  }
  return $false
}

function Read-ShellPrompt {
  param([int]$TimeoutMs = 10000)

  $buffer = [System.Text.StringBuilder]::new()
  $deadline = [DateTime]::UtcNow.AddMilliseconds($TimeoutMs)
  while ([DateTime]::UtcNow -lt $deadline) {
    Start-Sleep -Milliseconds 100
    $text = $serial.ReadExisting()
    if ($text) {
      [void]$buffer.Append($text)
      Write-Host $text -NoNewline
      $all = $buffer.ToString()
      if ($all.Contains('nsh> ')) { return 'nsh' }
      if ($all.Contains('vela> ')) { return 'vela' }
    }
  }

  return ''
}

function Send-Command {
  param(
    [string]$Command,
    [int]$TimeoutMs = 10000,
    [bool]$Sensitive = $false
  )

  if (-not $Sensitive) { Write-Host ">> $Command" }
  $serial.Write("$Command`n")
  if (-not (Read-Until 'nsh> ' $TimeoutMs (-not $Sensitive))) {
    throw "Timed out waiting for nsh> after command."
  }
}

try {
  $serial.Open()
  $shell = Read-ShellPrompt 35000
  if (-not $shell) {
    # Some USB-serial opens reset the ESP32-S3. Give the booted shell a
    # newline before declaring the serial session unavailable.
    $serial.Write("`n")
    $shell = Read-ShellPrompt 5000
    if (-not $shell) {
      throw "The board did not reach nsh>. Reset it once and rerun this script."
    }
  }

  if ($shell -eq 'vela') {
    $serial.Write("quit`n")
    if (-not (Read-Until 'nsh> ' 10000)) {
      throw 'The board did not leave the Vela agent shell.'
    }
  }

  Send-Command 'wapi scan wlan0' 15000
  $ssid = if ($Ssid) { $Ssid } else { Read-Host 'Wi-Fi SSID (2.4 GHz)' }
  if ([string]::IsNullOrWhiteSpace($ssid)) { throw 'SSID cannot be empty.' }
  if ($ssid -notmatch '^[\x20-\x7e]+$') {
    throw 'Use an ASCII-only 2.4 GHz SSID for this ESP32-S3-EYE firmware.'
  }
  $passwordSecure = Read-Host 'Wi-Fi password' -AsSecureString
  $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($passwordSecure)
  try {
    $password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer)
  } finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer)
  }
  $bssid = if ($Bssid) { $Bssid } else { Read-Host '2.4 GHz BSSID from scan (required; copy the matching 24xx MHz row)' }
  if ([string]::IsNullOrWhiteSpace($bssid)) {
    throw 'A 2.4 GHz BSSID is required to avoid selecting an incompatible access point.'
  }
  $frequency = if ($Frequency) { $Frequency } else { Read-Host '2.4 GHz frequency in MHz from the same scan row (for example 2462)' }
  if ($frequency -notmatch '^(2412|2417|2422|2427|2432|2437|2442|2447|2452|2457|2462|2467|2472)$') {
    throw 'Enter a valid 2.4 GHz Wi-Fi frequency from the scan result.'
  }

  Send-Command 'ifup wlan0'
  Send-Command 'wapi disconnect wlan0'
  Send-Command 'wapi mode wlan0 WAPI_MODE_MANAGED'
  Send-Command "wapi psk wlan0 $password 3 2" 10000 $true
  Send-Command "wapi freq wlan0 $frequency 1"
  Send-Command "wapi essid wlan0 $ssid WAPI_ESSID_DELAY_ON"
  Send-Command "wapi ap wlan0 $bssid" 30000
  Start-Sleep -Seconds 2
  Send-Command 'renew wlan0' 20000
  Start-Sleep -Seconds 3
  Send-Command 'wapi save_config wlan0'
  Send-Command 'ifconfig'
  Write-Host "Wi-Fi commands completed. Confirm that wlan0 shows RUNNING and a router-issued IPv4 address."
} finally {
  $password = $null
  if ($serial.IsOpen) { $serial.Close() }
}
