/* mfs_plat.h — utilidades de portabilidad de la capa de integración.
 *
 * La capa `platform/common` es el único punto donde el proyecto toca APIs del
 * sistema operativo; los front-ends FUSE (Linux) y WinFsp (Windows) consumen
 * este mismo contrato, de modo que la lógica de E/S y de metadatos es idéntica
 * en ambos sistemas.
 */
#ifndef MFS_PLAT_H
#define MFS_PLAT_H

/* Con `-std=c11` (ISO estricto), glibc oculta las declaraciones POSIX
 * (pthread_*, clock_gettime, pread/pwrite, nanosleep). Debe solicitarse antes
 * de incluir cualquier cabecera de la libc, por lo que este bloque vive al
 * principio de los ficheros que las usan y aquí, de forma defensiva, para
 * quien incluya este encabezado en primer lugar. */
#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#define MFS_PATH_SEP '\\'
#else
#define MFS_PATH_SEP '/'
#endif

/* ==== Mutex portable ====
 * El núcleo MatrixFS mantiene estado global (L2P, pools, tablas CFX) y por
 * tanto NO es reentrante entre instancias; el adaptador serializa todas las
 * llamadas al núcleo con este mutex. */
#if defined(_WIN32)
#include <windows.h>
typedef CRITICAL_SECTION mfs_mutex_t;
static inline void mfs_mutex_init(mfs_mutex_t *m) {
  InitializeCriticalSection(m);
}
static inline void mfs_mutex_destroy(mfs_mutex_t *m) {
  DeleteCriticalSection(m);
}
static inline void mfs_mutex_lock(mfs_mutex_t *m) { EnterCriticalSection(m); }
static inline void mfs_mutex_unlock(mfs_mutex_t *m) { LeaveCriticalSection(m); }
#else
#include <pthread.h>
typedef pthread_mutex_t mfs_mutex_t;
static inline void mfs_mutex_init(mfs_mutex_t *m) {
  (void)pthread_mutex_init(m, NULL);
}
static inline void mfs_mutex_destroy(mfs_mutex_t *m) {
  (void)pthread_mutex_destroy(m);
}
static inline void mfs_mutex_lock(mfs_mutex_t *m) {
  (void)pthread_mutex_lock(m);
}
static inline void mfs_mutex_unlock(mfs_mutex_t *m) {
  (void)pthread_mutex_unlock(m);
}
#endif

/* Tiempo de pared en segundos (epoch) — usado para mtime/setattr. */
uint64_t mfs_now_epoch_sec(void);
/* Retardo cooperativo (reintentos, sondeo del servicio de automontaje). */
void mfs_sleep_ms(uint32_t ms);

#endif /* MFS_PLAT_H */
