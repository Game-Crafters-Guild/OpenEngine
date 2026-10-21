param(
  [Parameter(Mandatory = $true)]
  [string]$ProjectRoot,
  [Parameter(Mandatory = $true)]
  [string]$Name,
  [switch]$CleanOnly = $false
)

$ErrorActionPreference = "Stop"


$assetsRoot = Join-Path $ProjectRoot "Assets"
$sandboxRoot = Join-Path $assetsRoot "UIReplaySandbox"
$root = Join-Path $sandboxRoot $Name

if (Test-Path $root) {
  Remove-Item -Recurse -Force -Path $root -ErrorAction SilentlyContinue
}

if ($CleanOnly) {
  exit 0
}

New-Item -ItemType Directory -Force -Path $root | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $root "Src") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $root "Dst") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $root "Src\\FolderDrop") | Out-Null

# Create a few small files for selection + move operations.
"A" | Set-Content -NoNewline -Encoding UTF8 -Path (Join-Path $root "Src\\A.txt")
"B" | Set-Content -NoNewline -Encoding UTF8 -Path (Join-Path $root "Src\\B.txt")
"C" | Set-Content -NoNewline -Encoding UTF8 -Path (Join-Path $root "Src\\C.txt")

exit 0

