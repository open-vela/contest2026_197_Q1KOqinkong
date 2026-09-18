param(
  [Parameter(Mandatory = $true)]
  [string]$BoardIp,
  [int]$Port = 8789,
  [switch]$AllowLocalTest
)

$ErrorActionPreference = 'Stop'

if ($BoardIp -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
  throw 'BoardIp must be an IPv4 address.'
}

$statePath = Join-Path $env:LOCALAPPDATA 'openvela\mimo-bridge.json'
if (-not (Test-Path -LiteralPath $statePath)) {
  throw 'Start the local MiMo Token Plan bridge first.'
}

$contestRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$root = (Resolve-Path (Join-Path $contestRoot '..')).Path
$python = Join-Path $root '.venv\Scripts\python.exe'
$gateway = Join-Path $PSScriptRoot 'foodloop-home-gateway.py'

if (-not (Test-Path -LiteralPath $python)) {
  throw "Python runtime not found: $python"
}

Write-Host "FoodLoop gateway: http://0.0.0.0:$Port"
Write-Host "Allowed board: $BoardIp"
Write-Host 'Keep this window open while the board scans food.'

$arguments = @($gateway, '--board-ip', $BoardIp, '--port', $Port)
if ($AllowLocalTest) {
  $arguments += '--allow-local-test'
}

& $python @arguments
