# build.ps1 — compila el soporte Windows (WinFsp) de MatrixFS «ATLAS».
#
# Uso:
#   .\build.ps1                     # Release x64 (por defecto)
#   .\build.ps1 -Config Debug
#   .\build.ps1 -Clean
#
# Requisitos:
#   · CMake >= 3.20 y Visual Studio 2019/2022 (o LLVM/clang-cl) en el PATH.
#   · WinFsp instalado (https://winfsp.dev). Si está en una ruta no estándar,
#     defina la variable de entorno WINFSP antes de ejecutar:
#         $env:WINFSP = "D:\SDK\WinFsp"
#
# Resultado: platform\windows\dist\<Config>\*.exe

[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $here 'build'
$distDir = Join-Path $here "dist\$Config"

function Fail($msg) { Write-Error $msg; exit 1 }

Write-Host "== MatrixFS — build Windows ($Config x64) ==" -ForegroundColor Cyan

# --- 1. Herramientas ---------------------------------------------------------
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Fail "No se encuentra 'cmake' en el PATH. Instale CMake (https://cmake.org)."
}

# --- 2. SDK de WinFsp -------------------------------------------------------
$winfspHints = @()
if ($env:WINFSP) { $winfspHints += $env:WINFSP }
$winfspHints += @(
    (Join-Path ${env:ProgramFiles(x86)} 'WinFsp'),
    (Join-Path $env:ProgramFiles 'WinFsp')
)
$winfsp = $winfspHints | Where-Object {
    $_ -and (Test-Path (Join-Path $_ 'inc\winfsp\winfsp.h'))
} | Select-Object -First 1

if (-not $winfsp) {
    Write-Warning ("No se ha detectado el SDK de WinFsp en las rutas habituales.`n" +
                   "  Si CMake tampoco lo encuentra, instale WinFsp o defina `$env:WINFSP.")
} else {
    Write-Host "WinFsp SDK: $winfsp"
    if (-not $env:WINFSP) { $env:WINFSP = $winfsp }
}

# --- 3. Configuración y compilación -----------------------------------------
if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Limpiando $buildDir"
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configurando CMake..."
cmake -S $here -B $buildDir -A x64
if ($LASTEXITCODE -ne 0) { Fail "La configuración de CMake falló." }

Write-Host "Compilando ($Config)..."
cmake --build $buildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) { Fail "La compilación falló." }

# --- 4. Reunir artefactos ---------------------------------------------------
New-Item -ItemType Directory -Force -Path $distDir | Out-Null
$artifacts = @('matrixfs_winfsp.exe', 'matrixfs-ctl.exe', 'matrixfs_automount.exe')
foreach ($a in $artifacts) {
    $src = Join-Path $buildDir "$Config\$a"
    if (-not (Test-Path $src)) { $src = Join-Path $buildDir $a }
    if (Test-Path $src) {
        Copy-Item -Force $src $distDir
    } else {
        Write-Warning "No se encontró el artefacto $a"
    }
}

Write-Host ""
Write-Host "Artefactos en $distDir :" -ForegroundColor Green
Get-ChildItem $distDir | ForEach-Object { Write-Host "  $($_.Name)" }
Write-Host ""
Write-Host "Siguientes pasos:"
Write-Host "  1) ./matrixfs-ctl.exe format <imagen.img> --label DATOS   # crear volumen"
Write-Host "  2) .\matrixfs_winfsp.exe <imagen.img> X:                  # montar en X:"
Write-Host "  3) .\install.ps1                                          # instalación en el sistema"
