/* FreeRTOS.h — shim minimo de FreeRTOS para pruebas de host (un solo hilo).
 *
 * No pretende ser un RTOS: reproduce las primitivas que el componente MatrixFS
 * usa, con semantica REAL donde importa (el mutex recursivo lleva contador de
 * recursion, de modo que la prueba puede afirmar que lock/unlock son
 * recursivos). Las implementaciones estan en idf_shim.c.
 */
#ifndef MATRIXFS_TEST_SHIM_FREERTOS_H
#define MATRIXFS_TEST_SHIM_FREERTOS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0

#define portMAX_DELAY 0xFFFFFFFFu

typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;

/* --- Secciones criticas ---------------------------------------------------
 * En host (un solo hilo) entran/salen sin deshabilitar nada, pero cuentan
 * profundidad para poder detectar desbalances en la prueba.
 * ----------------------------------------------------------------------- */
typedef struct {
  volatile int nesting;
  volatile uint32_t entries;
  volatile uint32_t exits;
} portMUX_TYPE;

#define portMUX_INITIALIZER_UNLOCKED {0, 0u, 0u}

void shim_port_enter_critical(portMUX_TYPE *mux);
void shim_port_exit_critical(portMUX_TYPE *mux);

#define portENTER_CRITICAL(mux) shim_port_enter_critical(mux)
#define portEXIT_CRITICAL(mux) shim_port_exit_critical(mux)

#define portENTER_CRITICAL_ISR(mux) shim_port_enter_critical(mux)
#define portEXIT_CRITICAL_ISR(mux) shim_port_exit_critical(mux)

#define vTaskDelay(t) ((void)(t))

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_TEST_SHIM_FREERTOS_H */
