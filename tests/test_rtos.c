/* test_rtos.c — Verificación del puerto RTOS genérico (§20.2, MFS-HW-001)
 *
 * En host no hay RTOS: se comprueba la detección (bare-metal), el despacho a
 * través del adaptador activo y la sustitución por el integrador
 * (mfs_port_rtos_register). Los adaptadores nativos (FreeRTOS/Zephyr/ThreadX)
 * solo se compilan en el target del RTOS correspondiente.
 */
#include "matrixfs/mfs_port_rtos.h"
#include "mfs_test.h"

/* --- Adaptador de prueba (simula un RTOS aportado por el integrador) --- */
static int g_dummy_crit_enter = 0;
static int g_dummy_crit_exit = 0;
static int g_dummy_wfi = 0;
static int g_dummy_yield = 0;
static uint32_t g_dummy_cycles = 0;
static uint32_t g_dummy_us = 0;

static void dummy_crit_enter(void) { g_dummy_crit_enter++; }
static void dummy_crit_exit(void) { g_dummy_crit_exit++; }
static uint32_t dummy_cycles(void) { return ++g_dummy_cycles; }
static uint32_t dummy_time_us(void) { return g_dummy_us++; }
static void dummy_wfi(void) { g_dummy_wfi++; }
static void dummy_yield(void) { g_dummy_yield++; }

static const mfs_rtos_ops dummy_ops = {
    dummy_crit_enter, dummy_crit_exit, dummy_cycles,   dummy_time_us,
    dummy_wfi,        dummy_yield,     MFS_RTOS_CUSTOM, "Dummy RTOS",
    (uint8_t)(MFS_RTOS_CAP_NESTING | MFS_RTOS_CAP_YIELD)};

void test_rtos_port(void) {
  TEST_BEGIN("puerto RTOS genérico");

  /* --- Detección: en host no hay RTOS huésped --- */
  CHECK_EQ(mfs_rtos_detect(), MFS_RTOS_NONE);

  /* --- Nombres: nunca NULL --- */
  CHECK(mfs_rtos_name(MFS_RTOS_NONE) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_ZEPHYR) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_FREERTOS) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_THREADX) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_MBED) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_NUTTX) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_RIOT) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_MYNEWT) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_RTTHREAD) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_PX5) != NULL);
  CHECK(mfs_rtos_name(MFS_RTOS_CUSTOM) != NULL);
  CHECK(strcmp(mfs_rtos_name(MFS_RTOS_ZEPHYR), "Zephyr RTOS") == 0);

  /* --- init(): activa el adaptador bare-metal y es idempotente --- */
  CHECK_EQ(mfs_port_rtos_init(), MFS_OK);
  const mfs_rtos_ops *ops = mfs_port_rtos_get_ops();
  CHECK(ops != NULL);
  CHECK_EQ(ops->id, MFS_RTOS_NONE);
  CHECK((ops->caps & MFS_RTOS_CAP_NESTING) != 0);

  /* El despacho sobre bare-metal no debe fallar ni bloquear. */
  mfs_port_rtos_crit_enter();
  mfs_port_rtos_crit_exit();
  mfs_port_rtos_wfi();
  mfs_port_rtos_yield(); /* sin yield en bare ⇒ no-op seguro */
  uint32_t c0 = mfs_port_rtos_cycles();
  uint32_t c1 = mfs_port_rtos_cycles();
  CHECK(c1 == c0 + 1u); /* contador determinista en bare-metal */
  (void)mfs_port_rtos_time_us();

  /* --- register(): validación de argumentos --- */
  CHECK_EQ(mfs_port_rtos_register(NULL), MFS_EINVAL);

  static const mfs_rtos_ops incomplete = {
      NULL, NULL, NULL, NULL, NULL, NULL, MFS_RTOS_CUSTOM, "Incompleto", 0};
  CHECK_EQ(mfs_port_rtos_register(&incomplete), MFS_EINVAL);

  /* --- register(): sustitución por adaptador del integrador --- */
  CHECK_EQ(mfs_port_rtos_register(&dummy_ops), MFS_OK);
  ops = mfs_port_rtos_get_ops();
  CHECK(ops != NULL);
  CHECK_EQ(ops->id, MFS_RTOS_CUSTOM);
  CHECK((ops->caps & MFS_RTOS_CAP_NESTING) != 0);
  CHECK((ops->caps & MFS_RTOS_CAP_YIELD) != 0);
  CHECK(strcmp(ops->name, "Dummy RTOS") == 0);

  /* El despacho debe invocar exactamente las primitivas registradas. */
  int e0 = g_dummy_crit_enter, x0 = g_dummy_crit_exit;
  int w0 = g_dummy_wfi, y0 = g_dummy_yield;
  mfs_port_rtos_crit_enter();
  mfs_port_rtos_crit_exit();
  mfs_port_rtos_wfi();
  mfs_port_rtos_yield();
  CHECK_EQ(g_dummy_crit_enter, e0 + 1);
  CHECK_EQ(g_dummy_crit_exit, x0 + 1);
  CHECK_EQ(g_dummy_wfi, w0 + 1);
  CHECK_EQ(g_dummy_yield, y0 + 1);

  uint32_t d0 = g_dummy_cycles;
  CHECK_EQ(mfs_port_rtos_cycles(), d0 + 1u);
  uint32_t u0 = g_dummy_us;
  CHECK_EQ(mfs_port_rtos_time_us(), u0);
  CHECK_EQ(g_dummy_us, u0 + 1u);

  /* --- Restaurar el adaptador bare-metal para el resto de la suite --- */
  CHECK_EQ(mfs_port_rtos_init(), MFS_OK);
  CHECK_EQ(mfs_port_rtos_get_ops()->id, MFS_RTOS_NONE);

  TEST_END("puerto RTOS genérico");
}
