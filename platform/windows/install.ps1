# install.ps1 — instala MatrixFS Ultra en Windows y registra el servicio de
# automontaje.
#
# Uso (PowerShell como Administrador):
#   .\install.ps1
#   .\install.ps1 -InstallDir D:\MatrixFS -AddToPath
#   .\install.ps1 -NoService          # sólo los binarios
#   .\install.ps1 -Start              # arranca el servicio al terminar
#
# Requisitos: haber compilado antes con .\build.ps1 (los binarios se toman de
# dist\<Config>\). No se toca NUNCA el contenido de los volúmenes del usuario.

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'MatrixFS'),
    [switch]$AddToPath,
    [switch]$NoService,
    [switch]$Start
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$distDir = Join-Path $here "dist\$Config"
$service = 'MatrixFS-Automount'

function Fail($msg) { Write-Error $msg; exit 1 }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "Ejecute este script desde una consola elevada (Administrador)."
}

Write-Host "== MatrixFS Ultra — instalación ($InstallDir) ==" -ForegroundColor Cyan

# --- 1. Binarios -------------------------------------------------------------
if (-not (Test-Path $distDir)) {
    Fail "No se encuentran los binarios en '$distDir'. Ejecute antes .\build.ps1."
}
New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
foreach ($a in @('matrixfs_winfsp.exe', 'matrixfs-ctl.exe', 'matrixfs_automount.exe')) {
    $src = Join-Path $distDir $a
    if (-not (Test-Path $src)) { Fail "Falta el artefacto '$a' en '$distDir'." }
    Copy-Item -Force $src $InstallDir
    Write-Host "  instalado $a"
}

# --- 2. PATH -----------------------------------------------------------------
if ($AddToPath) {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    if ($machinePath -notlike "*$InstallDir*") {
        [Environment]::SetEnvironmentVariable('Path', "$machinePath;$InstallDir", 'Machine')
        Write-Host "  '$InstallDir' añadido al PATH del sistema"
    } else {
        Write-Host "  '$InstallDir' ya estaba en el PATH del sistema"
    }
}

if ($NoService) {
    Write-Host "Servicio no solicitado (-NoService)." -ForegroundColor Yellow
    exit 0
}

# --- 3. Servicio de automontaje ---------------------------------------------
$exe = Join-Path $InstallDir 'matrixfs_automount.exe'
$existing = Get-Service -Name $service -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "  deteniendo el servicio existente..."
    Stop-Service -Name $service -Force -ErrorAction SilentlyContinue
    sc.exe delete $service | Out-Null
    Start-Sleep -Seconds 1
}
Write-Host "  registrando el servicio $service..."
sc.exe create $service binPath= "`"$exe`"" start= auto DisplayName= "MatrixFS Automount" | Out-Null
sc.exe description $service "Detecta volúmenes MatrixFS Ultra y los monta en la primera letra libre (D:, E:, ...)." | Out-Null
# Reinicio automático ante fallo (5 s / 10 s / 30 s)
sc.exe failure $service reset= 86400 actions= restart/5000/restart/10000/restart/30000 | Out-Null

if ($Start) {
    Write-Host "  arrancando el servicio..."
    Start-Service -Name $service
    Get-Service -Name $service | Format-List Name, Status, StartType
} else {
    Write-Host "  servicio registrado (arranque automático en el próximo inicio)."
}

Write-Host ""
Write-Host "Instalación completada." -ForegroundColor Green
Write-Host "  Binarios : $InstallDir"
Write-Host "  Servicio : $service"
Write-Host "  Diagnóstico: & '$InstallDir\matrixfs-ctl.exe'  (sin argumentos: ayuda)"
