# Instala crbridge en una instalación de Steam.
# Uso: .\install.ps1 -SteamPath "C:\Program Files (x86)\Steam"
#      .\install.ps1                       (intenta detectar Steam automáticamente)

param(
    [string]$SteamPath
)

# Detección automática vía registry si no se pasa ruta
if (-not $SteamPath) {
    $key = Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -ErrorAction SilentlyContinue
    if (-not $key) { $key = Get-ItemProperty -Path "HKLM:\SOFTWARE\Valve\Steam" -ErrorAction SilentlyContinue }
    if ($key) { $SteamPath = $key.InstallPath }
}

if (-not $SteamPath -or -not (Test-Path "$SteamPath\steam.exe")) {
    Write-Error "Steam no encontrado. Usa -SteamPath para especificar manualmente."
    exit 1
}

Write-Host "Steam detectado en: $SteamPath" -ForegroundColor Cyan

# Backup defensivo de cualquier version.dll preexistente (otro tool podría tenerlo)
if (Test-Path "$SteamPath\version.dll") {
    $backup = "$SteamPath\version.dll.bak"
    if (-not (Test-Path $backup)) {
        Copy-Item "$SteamPath\version.dll" $backup
        Write-Host "Backup existente: $backup" -ForegroundColor Yellow
    }
}

# Cerrar Steam si está corriendo
$steamProc = Get-Process steam -ErrorAction SilentlyContinue
if ($steamProc) {
    Write-Host "Cerrando Steam..." -ForegroundColor Yellow
    Stop-Process -Name steam -Force
    Start-Sleep -Seconds 2
}

# Resolver ruta de los binarios construidos. Asume CMake build en build/
$buildDir = Join-Path $PSScriptRoot "..\build"
$versionDll  = Join-Path $buildDir "proxy\Release\version.dll"
$crbridgeDll = Join-Path $buildDir "Release\crbridge.dll"

if (-not (Test-Path $versionDll))  { Write-Error "version.dll no encontrado en $versionDll. ¿Build hecho?"; exit 1 }
if (-not (Test-Path $crbridgeDll)) { Write-Error "crbridge.dll no encontrado en $crbridgeDll. ¿Build hecho?"; exit 1 }

# Copiar
Copy-Item $versionDll  "$SteamPath\version.dll"  -Force
Copy-Item $crbridgeDll "$SteamPath\crbridge.dll" -Force

Write-Host "Instalado:" -ForegroundColor Green
Write-Host "  $SteamPath\version.dll"
Write-Host "  $SteamPath\crbridge.dll"
Write-Host "Arranca Steam normal. Log esperado en %TEMP%\crbridge.log" -ForegroundColor Cyan
