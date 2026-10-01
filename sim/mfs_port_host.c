/* mfs_port_host.c — puerto de host para tests/CI (§20.2/§20.3, §27)
 *
 * Implementa las primitivas obligatorias del contrato de puerto para
 * ejecutar MatrixFS sobre vFlash en un PC. En target el integrador aporta su
 * propio puerto (secciones críticas reales, contador de ciclos, WFI).
 */

/* Compilar con `-std=c11` (ISO estricto) hace que glibc oculte las
 * declaraciones POSIX (clock_gettime, CLOCK_MONOTONIC, nanosleep, pread/pwrite,
 * pthread_*). Se solicitan aquí, antes de cualquier inclusión. */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "matrixfs/mfs_port.h"

#if defined(_WIN32)
#include <windows.h>
static LARGE_INTEGER g_qpc_freq;
static int g_qpc_init;

uint32_t mfs_port_cycles(void) {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (uint32_t)c.QuadPart;
}

uint32_t mfs_port_time_us(void) {
  if (!g_qpc_init) {
    QueryPerformanceFrequency(&g_qpc_freq);
    g_qpc_init = 1;
  }
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return (uint32_t)((c.QuadPart * 1000000LL) / g_qpc_freq.QuadPart);
}
#else
#include <time.h>
uint32_t mfs_port_cycles(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

uint32_t mfs_port_time_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)((uint64_t)ts.tv_sec * 1000000ull +
                    (uint64_t)ts.tv_nsec / 1000ull);
}
#endif

void mfs_port_crit_enter(void) {}
void mfs_port_crit_exit(void) {}
void mfs_port_wfi(void) {}
