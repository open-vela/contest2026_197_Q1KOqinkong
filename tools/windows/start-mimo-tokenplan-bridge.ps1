param(
  [int]$Port = 8787,
  [string]$BridgeHost = ''
)

$secure = Read-Host 'New MiMo Token Plan key' -AsSecureString
$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
try {
  $mimoKey = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr)
} finally {
  [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr)
}

# Keep the local bridge token short enough for the ESP32-S3 NSH set_llm command.
$bytes = New-Object byte[] 6
$rng = New-Object Security.Cryptography.RNGCryptoServiceProvider
try {
  $rng.GetBytes($bytes)
} finally {
  $rng.Dispose()
}
$boardToken = -join ($bytes | ForEach-Object { $_.ToString('x2') })
if (-not $BridgeHost) {
  $BridgeHost = (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object {
      $_.AddressState -eq 'Preferred' -and
      $_.IPAddress -match '^192\.168\.'
    } |
    Select-Object -First 1 -ExpandProperty IPAddress)
}
if (-not $BridgeHost) { throw 'No suitable private IPv4 address found.' }

$stateDir = Join-Path $env:LOCALAPPDATA 'openvela'
New-Item -ItemType Directory -Path $stateDir -Force | Out-Null
$statePath = Join-Path $stateDir 'mimo-bridge.json'
@{
  host = $BridgeHost
  port = $Port
  board_token = $boardToken
} | ConvertTo-Json -Compress | Set-Content -LiteralPath $statePath -Encoding ASCII

$contestRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$root = (Resolve-Path (Join-Path $contestRoot '..')).Path
$python = Join-Path $root '.venv\Scripts\python.exe'
$bridge = Join-Path $PSScriptRoot 'mimo-tokenplan-bridge.py'
$env:MIMO_TOKEN_PLAN_KEY = $mimoKey
$env:OPENVELA_BRIDGE_TOKEN = $boardToken
Write-Host "Local bridge: http://${BridgeHost}:$Port"
Write-Host 'Keep this window open while the board uses MiMo.'
try {
  & $python $bridge --port $Port
} finally {
  Remove-Item Env:MIMO_TOKEN_PLAN_KEY -ErrorAction SilentlyContinue
  Remove-Item Env:OPENVELA_BRIDGE_TOKEN -ErrorAction SilentlyContinue
  $mimoKey = $null
}
