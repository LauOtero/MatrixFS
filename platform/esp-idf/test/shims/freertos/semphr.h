/* semphr.h — shim minimo de FreeRTOS (semaforos) para pruebas de host.
 *
 * `xSemaphoreCreateRecursiveMutex` devuelve un puntero NO nulo a una ranura de
 * una tabla estatica (el componente comprueba el resultado) y el mutex lleva un
 * contador de recursion REAL: `xSemaphoreTakeRecursive` incrementa y
 * `xSemaphoreGiveRecursive` decrementa, de modo que la prueba puede verificar
 * que dos lock seguidos y dos unlock seguidos dejan la recursion a 0.
 */
#ifndef MATRIXFS_TEST_SHIM_SEMPHR_H
#define MATRIXFS_TEST_SHIM_SEMPHR_H

#include <stdint.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shim_sem_s *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t sem);
BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks);
BaseType_t xSemaphoreGive(SemaphoreHandle_t sem);
void vSemaphoreDelete(SemaphoreHandle_t sem);

/* --- Solo del shim: contabilidad de recursion para las aserciones --- */
/* Profundidad de recursion actual del mutex MAS RECIENTEMENTE usado. */
int shim_sem_recursion_depth(void);
/* Maxima profundidad observada desde el ultimo shim_sem_reset_stats(). */
int shim_sem_recursion_max(void);
/* Mutex vivos (creados y no borrados). */
int shim_sem_live(void);
/* Tomas/entregas totales y profundidad maxima a 0. */
void shim_sem_reset_stats(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_SEMPHR_H */
