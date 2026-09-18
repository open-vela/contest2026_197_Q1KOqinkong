param(
  [Parameter(Mandatory = $true)]
  [string]$Port,

  [int]$Baud = 115200
)

$contestRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$root = (Resolve-Path (Join-Path $contestRoot "..")).Path
$python = Join-Path $root ".venv\Scripts\python.exe"

if (-not (Test-Path $python)) {
  throw "Missing workspace Python environment: $python"
}

& $python -m serial.tools.miniterm $Port $Baud --eol LF
