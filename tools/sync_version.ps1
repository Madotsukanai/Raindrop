param(
    [string]$Stamp = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = (Get-Content -Path (Join-Path $root 'VERSION') -Raw).Trim()
$sources = @('src\Smd\Smd.c', 'src\RefRain.c')

foreach ($rel in $sources) {
    $path = Join-Path $root $rel
    if (-not (Test-Path $path)) { continue }
    $text = [System.IO.File]::ReadAllText($path)
    $updated = [regex]::Replace($text, '(?m)^#define VERSION .*?(\r?)$', '#define VERSION "' + $version + '"$1')
    if ($updated -ne $text) {
        [System.IO.File]::WriteAllText($path, $updated)
        Write-Host "Version: $rel -> $version"
    }
}

if ($Stamp -ne "") {
    [System.IO.File]::WriteAllText((Join-Path $root $Stamp), $version)
}
