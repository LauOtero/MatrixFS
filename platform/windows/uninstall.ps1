# uninstall.ps1 — desinstala MatrixFS de Windows.
#
# Uso (PowerShell como Administrador):
#   .\uninstall.ps1
#   .\uninstall.ps1 -InstallDir D:\MatrixFS
#
# Pasos (inversos a install.ps1):
#   1. Detiene y elimina el servicio de automontaje MatrixFS-Automount.
#   2. Finaliza los montajes en curso (procesos matrixfs_winfsp.exe).
#   3. Borra los binarios del directorio de instalación.
#   4. Retira el directorio del PATH del sistema si install.ps1 lo añadió.
#
# IMPORTANTE: este script NO toca NUNCA los volúmenes del usuario. Toda la
# información (ficheros, metadatos y permisos) vive en el propio medio, por lo
# que desinstalar el software no destruye ni modifica ningún dato: los
# volúmenes pueden seguir usándose en Linux o volviendo a instalar MatrixFS.

[CmdletBinding()]
param(
    [string]$InstallDir = (Join-Path $env:ProgramFiles 'MatrixFS'),
    [switch]$KeepMounts
)

$ErrorActionPreference = 'Stop'
$service = 'MatrixFS-Automount'

function Fail($msg) { Write-Error $msg; exit 1 }

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Fail "Ejecute este script desde una consola elevada (Administrador)."
}

Write-Host "== MatrixFS — desinstalación ($InstallDir) ==" -ForegroundColor Cyan

# --- 1. Servicio de automontaje ---------------------------------------------
$existing = Get-Service -Name $service -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "  deteniendo el servicio $service..."
    Stop-Service -Name $service -Force -ErrorAction SilentlyContinue
    sc.exe delete $service | Out-Null
    Write-Host "  servicio eliminado"
} else {
    Write-Host "  el servicio $service no está instalado"
}

# --- 2. Montajes en curso ----------------------------------------------------
$procs = Get-Process -Name 'matrixfs_winfsp' -ErrorAction SilentlyContinue
if ($procs) {
    if ($KeepMounts) {
        Write-Host "  AVISO: hay $($procs.Count) volumen(es) montado(s); -KeepMounts los deja activos." -ForegroundColor Yellow
        Write-Host "         Los binarios en uso no podrán borrarse hasta que se desmonten."
    } else {
        Write-Host "  desmontando $($procs.Count) volumen(es) en curso..."
        $procs | Stop-Process -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 1
    }
} else {
    Write-Host "  no hay volúmenes montados"
}

# --- 3. Binarios -------------------------------------------------------------
if (Test-Path $InstallDir) {
    Remove-Item -Recurse -Force $InstallDir -ErrorAction SilentlyContinue
    if (Test-Path $InstallDir) {
        Write-Warning "No se pudo borrar por completo '$InstallDir' (¿ficheros en uso?)."
        Write-Host "   Cierre las sesiones que usen la unidad y vuelva a ejecutar el script."
    } else {
        Write-Host "  binarios eliminados"
    }
} else {
    Write-Host "  '$InstallDir' no existe"
}

# --- 4. PATH del sistema -----------------------------------------------------
$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
if ($machinePath -and $machinePath -like "*$InstallDir*") {
    $clean = ($machinePath -split ';' | Where-Object { $_ -and ($_ -ne $InstallDir) }) -join ';'
    [Environment]::SetEnvironmentVariable('Path', $clean, 'Machine')
    Write-Host "  '$InstallDir' retirado del PATH del sistema"
}

Write-Host ""
Write-Host "Desinstalación completada." -ForegroundColor Green
Write-Host "  Los volúmenes del usuario NO se han modificado: siguen intactos y son"
Write-Host "  legibles en cualquier otro sistema con MatrixFS (Linux o Windows)."
