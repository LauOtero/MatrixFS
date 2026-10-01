/* mfs_plat.c — implementación de las utilidades de portabilidad. */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L /* nanosleep, pthread_*, time() */
#endif

#include "mfs_plat.h"

#if defined(_WIN32)
#include <sys/timeb.h>
#include <windows.h>

uint64_t mfs_now_epoch_sec(void) {
  FILETIME ft;
  ULARGE_INTEGER u;
  GetSystemTimeAsFileTime(&ft);
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  /* 100 ns desde 1601-01-01 → segundos desde 1970-01-01 */
  return (uint64_t)((u.QuadPart / 10000000ull) - 11644473600ull);
}

void mfs_sleep_ms(uint32_t ms) { Sleep(ms); }
#else
#include <time.h>

uint64_t mfs_now_epoch_sec(void) { return (uint64_t)time(NULL); }

void mfs_sleep_ms(uint32_t ms) {
  struct timespec ts;
  ts.tv_sec = (time_t)(ms / 1000u);
  ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
  nanosleep(&ts, NULL);
}
#endif
