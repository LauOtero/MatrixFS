/* task.h — shim minimo de FreeRTOS (tareas) para pruebas de host.
 *
 * `xTaskGetCurrentTaskHandle` devuelve un centinela NO nulo y estable: el
 * componente lo usa como clave del buffer de error por tarea, de modo que en
 * host todas las llamadas caen en la misma ranura (comportamiento correcto para
 * un unico hilo).
 */
#ifndef MATRIXFS_TEST_SHIM_FREERTOS_TASK_H
#define MATRIXFS_TEST_SHIM_FREERTOS_TASK_H

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct shim_task_s *TaskHandle_t;

TaskHandle_t xTaskGetCurrentTaskHandle(void);
const char *pcTaskGetName(TaskHandle_t task);
void taskYIELD(void);

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_FREERTOS_TASK_H */
