param([string]$SteamPath)

if (-not $SteamPath) {
    $key = Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -ErrorAction SilentlyContinue
    if ($key) { $SteamPath = $key.InstallPath }
}
if (-not (Test-Path "$SteamPath\steam.exe")) { Write-Error "Steam no encontrado"; exit 1 }

Stop-Process -Name steam -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

Remove-Item "$SteamPath\version.dll"  -ErrorAction SilentlyContinue
Remove-Item "$SteamPath\crbridge.dll" -ErrorAction SilentlyContinue

# Restaurar backup si existía
if (Test-Path "$SteamPath\version.dll.bak") {
    Move-Item "$SteamPath\version.dll.bak" "$SteamPath\version.dll"
    Write-Host "Backup restaurado" -ForegroundColor Yellow
}

Write-Host "Desinstalado" -ForegroundColor Green
