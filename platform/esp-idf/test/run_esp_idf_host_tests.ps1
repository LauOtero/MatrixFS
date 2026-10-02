<#
.SYNOPSIS
  Compila y ejecuta la prueba del componente ESP-IDF de MatrixFS en host, contra
  los perfiles de version simulados de ESP-IDF (v5.5.x y v6.1.x).

.DESCRIPTION
  No hay ESP-IDF instalado: los perfiles de platform/esp-idf/test/shims son la
  unica forma de verificar el componente. El script:

    1. compila la suite funcional (test_esp_idf.c) contra AMBOS perfiles;
    2. compila la suite de versiones (test_esp_idf_versions.c) contra AMBOS;
    3. compila la suite funcional contra v6.1 SIN el adaptador de version, para
       demostrar el fallo real que encuentra un componente escrito para v5
       (no se considera un fallo del script: se informa);
    4. borra los ejecutables.

  El unico parametro de entrada que cambia entre perfiles es el primer -I: el
  directorio del perfil coloca su sdkconfig.h y su esp_vfs.h por delante.

.EXAMPLE
  pwsh -File platform/esp-idf/test/run_esp_idf_host_tests.ps1
#>
[CmdletBinding()]
param(
  [string]$Gcc = "C:\ProgramData\mingw64\mingw64\bin\gcc.exe",
  [switch]$KeepExe
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
Set-Location $root

if (-not (Test-Path $Gcc)) { throw "No se encuentra gcc en '$Gcc'" }

# Núcleo del proyecto: mismo glob y mismas exclusiones que
# platform/esp-idf/CMakeLists.txt (el puerto §20.2 lo aporta matrixfs_esp.c).
$core = Get-ChildItem src/core/*.c, src/crypto/*.c, src/ftl/*.c, src/tier/*.c,
                      src/sec/*.c, src/xio/*.c |
        Where-Object { $_.Name -ne 'mfs_port_arch.c' -and $_.Name -ne 'mfs_port_rtos.c' } |
        ForEach-Object { $_.FullName }

$shims    = "platform/esp-idf/test/shims"
$profiles = [ordered]@{
  "v5.5" = @{ inc = "$shims/idf_v55"; id = 1 }
  "v6.1" = @{ inc = "$shims/idf_v61"; id = 2 }
}

# Fuentes del componente (las REALES del repositorio).
$component = @(
  "platform/esp-idf/matrixfs_esp.c"
  "platform/esp-idf/matrixfs_esp_vfs.c"
  "platform/embedded/mfs_embedded.c"
)
# Shim + particion simulada + adaptador de version.
$harness = @(
  "platform/esp-idf/test/esp_partition_fake.c"
  "platform/esp-idf/test/shims/idf_shim.c"
  "platform/esp-idf/test/mfs_vfs_adapter.c"
)

$results = @()

# NOTA: el parametro NO puede llamarse $Args: es una variable automatica de
# PowerShell y el splatting quedaria vacio ("no input files" de gcc).
function Invoke-Gcc {
  param([string[]]$GccArgs, [string]$Out)
  # Se captura la salida en memoria: no dependemos de ficheros temporales (que
  # pueden no crearse si gcc falla muy pronto).
  $text = (& $Gcc @GccArgs 2>&1 | Out-String)
  $rc = $LASTEXITCODE
  return @{ rc = $rc; log = $text }
}

foreach ($name in $profiles.Keys) {
  $inc = $profiles[$name].inc
  $pid_ = $profiles[$name].id
  foreach ($suite in @("test_esp_idf.c", "test_esp_idf_versions.c")) {
    $exe = "mfs_${name}_$($suite -replace '\.c$','' ).exe"
    $gccArgs = @(
      "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-g",
      "-DMATRIXFS_TEST_IDF_PROFILE=$pid_",
      "-include", "$inc/esp_vfs.h",
      "-I$inc", "-I$shims",
      "-Iplatform/esp-idf", "-Iplatform/esp-idf/include",
      "-Iinclude", "-Isrc", "-Iplatform/embedded", "-Itests",
      "-Iplatform/esp-idf/test",
      "-o", $exe,
      "platform/esp-idf/test/$suite"
    ) + $harness + $component + $core

    $build = Invoke-Gcc -GccArgs $gccArgs -Out $exe
    $status = if ($build.rc -eq 0) { "BUILD OK" } else { "BUILD FAIL" }
    Write-Host "--- [$name] $suite : $status"
    if ($build.rc -ne 0) {
      Write-Host $build.log
      $results += [pscustomobject]@{ Perfil = $name; Suite = $suite; Build = $status; Run = "-" }
      continue
    }
    if (-not (Test-Path $exe)) { throw "gcc dijo OK pero no hay $exe" }
    $out = & ".\$exe" 2>&1
    $run = if ($LASTEXITCODE -eq 0) { "RUN OK" } else { "RUN FAIL" }
    $checks = ($out | Select-String -Pattern '^checks:').Line
    $out | Where-Object { $_ -match '^== |^checks:|\[FAIL\]|perfil:' } | ForEach-Object { Write-Host "    $_" }
    $results += [pscustomobject]@{ Perfil = $name; Suite = $suite; Build = $status; Run = "$run ($checks)" }
  }
}

# --- Control negativo: el componente REAL contra v6.1 SIN adaptador ---------
Write-Host "--- [v6.1] test_esp_idf.c SIN adaptador (control negativo esperado)"
$negArgs = @(
  "-std=c11", "-Wall", "-Wextra", "-O2",
  "-include", "$shims/esp_vfs.h",
  "-I$($profiles['v6.1'])", "-I$shims",
  "-Iplatform/esp-idf", "-Iplatform/esp-idf/include",
  "-Iinclude", "-Isrc", "-Iplatform/embedded", "-Itests",
  "-Iplatform/esp-idf/test",
  "-o", "mfs_v61_no_adapter.exe",
  "platform/esp-idf/test/test_esp_idf.c"
) + $harness + $component + $core

$neg = Invoke-Gcc -GccArgs $negArgs -Out "mfs_v61_no_adapter.exe"
if ($neg.rc -eq 0) {
  Write-Host "    INESPERADO: compilo sin adaptador (el perfil v6.1 no refleja el cambio de firma)"
  $results += [pscustomobject]@{ Perfil = "v6.1"; Suite = "sin adaptador"; Build = "INESPERADO OK"; Run = "-" }
} else {
  $first = ($neg.log -split "`r?`n" | Select-String -Pattern 'error:' | Select-Object -First 2).Line
  Write-Host "    fallo esperado (la API publica de v6.1 no declara la funcion):"
  $first | ForEach-Object { Write-Host "      $_" }
  $results += [pscustomobject]@{ Perfil = "v6.1"; Suite = "sin adaptador"; Build = "FALLO ESPERADO"; Run = "-" }
}

Write-Host ""
$results | Format-Table -AutoSize

if (-not $KeepExe) {
  Get-ChildItem -Filter "mfs_*.exe" | Remove-Item -Force
  Write-Host "ejecutables borrados"
}
