/* matrixfs_automount.c — servicio de automontaje de MatrixFS (Windows).
 *
 * Detecta los medios que contienen un volumen MatrixFS y los monta como
 * unidades con letra libre, arrancando un proceso `matrixfs_winfsp.exe` por
 * volumen. El resultado es que, al conectar el medio, la unidad aparece en el
 * Explorador de Archivos de Windows 10/11 sin intervención del usuario.
 *
 * Diseño
 *   · Se instala como servicio de Windows (SERVICE_WIN32_OWN_PROCESS) con el
 *     nombre "MatrixFS-Automount", de modo que arranca en el inicio del sistema
 *     con privilegios suficientes para abrir volúmenes crudos.
 *   · Cada N segundos enumera los volúmenes con
 * FindFirstVolumeW/FindNextVolumeW y sondea cada uno con la capa VFS portable
 * (mfs_vfs_probe), la misma que usan los front-ends, por lo que la detección es
 * idéntica en Windows y en Linux. · Sólo se leen los dos superblocks (512 B)
 * por volumen y ciclo: coste despreciable y sin escritura. · La letra de unidad
 * se elige de la primera libre desde D: y se libera al desaparecer el volumen
 * (o al terminar el proceso hijo).
 *
 * Compilación: véase CMakeLists.txt (objetivo matrixfs_automount).
 * Instalación: install.ps1 registra y arranca el servicio.
 */

#if !defined(_WIN32)
#error "matrixfs_automount.c sólo compila en Windows."
#endif

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "mfs_vfs.h"

#define MFS_SVC_NAME L"MatrixFS-Automount"
#define MFS_SVC_DISPLAY L"MatrixFS — Automontaje de volúmenes"
#define MFS_SCAN_PERIOD_MS 3000u
#define MFS_MAX_MOUNTS 16u

typedef struct {
  WCHAR device[MAX_PATH]; /* \\?\Volume{...}\  */
  WCHAR letter[4];        /* "X:"              */
  HANDLE proc;            /* proceso matrixfs_winfsp */
} mount_slot;

static SERVICE_STATUS_HANDLE g_ssh;
static SERVICE_STATUS g_status;
static HANDLE g_stop_event;
static WCHAR g_exe_dir[MAX_PATH]; /* directorio donde vive este ejecutable */
static mount_slot g_mounts[MFS_MAX_MOUNTS];

/* ============================ Utilidades ================================ */

static void svc_report(DWORD state, DWORD exit_code, DWORD checkpoint) {
  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwCurrentState = state;
  g_status.dwWin32ExitCode = exit_code;
  g_status.dwWaitHint =
      (state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING) ? 5000
                                                                        : 0;
  g_status.dwControlsAccepted =
      (state == SERVICE_RUNNING) ? SERVICE_ACCEPT_STOP : 0;
  g_status.dwCheckPoint = checkpoint;
  if (g_ssh)
    SetServiceStatus(g_ssh, &g_status);
}

static void log_msg(const wchar_t *fmt, ...) {
  /* La salida se registra en el Visor de eventos a través del registro de
   * depuración de WinFsp cuando está activo; aquí se escribe además en el log
   * de depuración del sistema para diagnóstico con DebugView. */
  wchar_t buf[512];
  va_list ap;
  va_start(ap, fmt);
  _vsnwprintf_s(buf, sizeof(buf) / sizeof(WCHAR), _TRUNCATE, fmt, ap);
  va_end(ap);
  OutputDebugStringW(buf);
  OutputDebugStringW(L"\n");
}

/* Primera letra de unidad libre desde D: (A y B se reservan por convención). */
static BOOL pick_letter(WCHAR out[4]) {
  DWORD mask = GetLogicalDrives();
  for (int c = 'D'; c <= 'Z'; c++) {
    if ((mask & (1u << (c - 'A'))) == 0u) {
      out[0] = (WCHAR)c;
      out[1] = L':';
      out[2] = L'\0';
      return TRUE;
    }
  }
  return FALSE;
}

static BOOL slot_is_mounted(const WCHAR *device) {
  for (UINT i = 0; i < MFS_MAX_MOUNTS; i++)
    if (g_mounts[i].proc && _wcsicmp(g_mounts[i].device, device) == 0)
      return TRUE;
  return FALSE;
}

static void reap_dead_mounts(void) {
  for (UINT i = 0; i < MFS_MAX_MOUNTS; i++) {
    if (!g_mounts[i].proc)
      continue;
    DWORD code = 0;
    if (!GetExitCodeProcess(g_mounts[i].proc, &code) || code != STILL_ACTIVE) {
      log_msg(L"matrixfs: volumen %s (%s) desmontado", g_mounts[i].device,
              g_mounts[i].letter);
      CloseHandle(g_mounts[i].proc);
      g_mounts[i].proc = NULL;
      g_mounts[i].device[0] = L'\0';
      g_mounts[i].letter[0] = L'\0';
    }
  }
}

/* ============================== Montaje ================================= */

static void mount_volume(const WCHAR *device) {
  if (slot_is_mounted(device))
    return;

  /* Sólo interesan volúmenes con un volumen MatrixFS válido. */
  char dev_a[MAX_PATH];
  if (WideCharToMultiByte(CP_ACP, 0, device, -1, dev_a, sizeof(dev_a), NULL,
                          NULL) <= 0)
    return;
  mfs_mount_opts mo;
  mfs_mount_opts_default(&mo);
  char label[32];
  uint8_t mode = 0xFFu;
  if (mfs_vfs_probe(dev_a, &mo.blk, label, sizeof(label), &mode) != MFS_OK)
    return;

  WCHAR letter[4];
  if (!pick_letter(letter)) {
    log_msg(L"matrixfs: sin letras de unidad libres; %s no se monta", device);
    return;
  }

  int idx = -1;
  for (UINT i = 0; i < MFS_MAX_MOUNTS; i++) {
    if (!g_mounts[i].proc) {
      idx = (int)i;
      break;
    }
  }
  if (idx < 0) {
    log_msg(L"matrixfs: tabla de montajes llena; %s no se monta", device);
    return;
  }

  /* Línea de comandos: "<dir>\matrixfs_winfsp.exe <volumen> <letra>:" */
  WCHAR cmd[2 * MAX_PATH + 32];
  _snwprintf_s(cmd, sizeof(cmd) / sizeof(WCHAR), _TRUNCATE,
               L"\"%s\\matrixfs_winfsp.exe\" \"%s\" %s", g_exe_dir, device,
               letter);

  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  memset(&pi, 0, sizeof(pi));

  if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL,
                      NULL, &si, &pi)) {
    log_msg(L"matrixfs: CreateProcessW falló (%lu) para %s",
            (unsigned long)GetLastError(), device);
    return;
  }
  CloseHandle(pi.hThread);

  wcscpy_s(g_mounts[idx].device, MAX_PATH, device);
  wcscpy_s(g_mounts[idx].letter, 4, letter);
  g_mounts[idx].proc = pi.hProcess;
  log_msg(L"matrixfs: volumen '%S' montado en %s (etiqueta '%S', modo %u)",
          dev_a, letter, label, (unsigned)mode);
}

static void scan_and_mount(void) {
  WCHAR volname[MAX_PATH];
  HANDLE find = FindFirstVolumeW(volname, MAX_PATH);
  if (find == INVALID_HANDLE_VALUE)
    return;
  do {
    /* FindFirstVolumeW devuelve rutas con '\\?\' que Windows acepta al abrir
     * el volumen (el driver de bloque las reconoce como dispositivo). */
    mount_volume(volname);
  } while (FindNextVolumeW(find, volname, MAX_PATH));
  FindVolumeClose(find);
}

static void unmount_all(void) {
  for (UINT i = 0; i < MFS_MAX_MOUNTS; i++) {
    if (!g_mounts[i].proc)
      continue;
    log_msg(L"matrixfs: deteniendo montaje de %s", g_mounts[i].device);
    TerminateProcess(g_mounts[i].proc, 0);
    WaitForSingleObject(g_mounts[i].proc, 5000);
    CloseHandle(g_mounts[i].proc);
    g_mounts[i].proc = NULL;
  }
}

/* =========================== Bucle de servicio ========================== */

static DWORD WINAPI service_worker(LPVOID arg) {
  (void)arg;
  while (WaitForSingleObject(g_stop_event, MFS_SCAN_PERIOD_MS) ==
         WAIT_TIMEOUT) {
    reap_dead_mounts();
    scan_and_mount();
  }
  unmount_all();
  return 0;
}

static DWORD WINAPI service_ctrl(DWORD ctrl, DWORD event_type, LPVOID data,
                                 LPVOID ctx) {
  (void)event_type;
  (void)data;
  (void)ctx;
  if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
    svc_report(SERVICE_STOP_PENDING, NO_ERROR, 0);
    SetEvent(g_stop_event);
  }
  return NO_ERROR;
}

static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
  (void)argc;
  (void)argv;
  g_ssh = RegisterServiceCtrlHandlerExW(MFS_SVC_NAME, service_ctrl, NULL);
  if (!g_ssh)
    return;

  g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
  g_status.dwControlsAccepted = 0;
  g_status.dwWin32ExitCode = NO_ERROR;
  svc_report(SERVICE_START_PENDING, NO_ERROR, 1);

  /* Directorio del ejecutable: allí se espera matrixfs_winfsp.exe. */
  WCHAR self[MAX_PATH];
  DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    svc_report(SERVICE_STOPPED, ERROR_FILE_NOT_FOUND, 0);
    return;
  }
  wcscpy_s(g_exe_dir, MAX_PATH, self);
  WCHAR *slash = wcsrchr(g_exe_dir, L'\\');
  if (slash)
    *slash = L'\0';

  g_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!g_stop_event) {
    svc_report(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }

  svc_report(SERVICE_RUNNING, NO_ERROR, 0);

  HANDLE th = CreateThread(NULL, 0, service_worker, NULL, 0, NULL);
  if (!th) {
    svc_report(SERVICE_STOPPED, GetLastError(), 0);
    return;
  }

  WaitForSingleObject(th, INFINITE);
  CloseHandle(th);
  CloseHandle(g_stop_event);
  svc_report(SERVICE_STOPPED, NO_ERROR, 0);
}

/* ================================ main ================================== */

int wmain(int argc, wchar_t **argv) {
  /* Modo diagnóstico: `matrixfs_automount --scan` monta una vez y sale. */
  if (argc >= 2 && wcscmp(argv[1], L"--scan") == 0) {
    WCHAR self[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
      wcscpy_s(g_exe_dir, MAX_PATH, self);
      WCHAR *slash = wcsrchr(g_exe_dir, L'\\');
      if (slash)
        *slash = L'\0';
    }
    scan_and_mount();
    wprintf(L"matrixfs: sondeo completado.\n");
    return 0;
  }

  SERVICE_TABLE_ENTRYW table[] = {
      {(LPWSTR)MFS_SVC_NAME, service_main},
      {NULL, NULL},
  };
  if (!StartServiceCtrlDispatcherW(table)) {
    fwprintf(stderr,
             L"matrixfs_automount: no se pudo conectar con el gestor de "
             L"servicios (%lu).\nInstale el servicio con install.ps1 o use "
             L"'--scan' para una pasada manual.\n",
             (unsigned long)GetLastError());
    return 1;
  }
  return 0;
}
