/* mpconfigport_fragment.h — fragmento de configuración para el usermod
 * MatrixFS de MicroPython.
 *
 * Copyright (c) 2026 MatrixFS Ultra contribuidores.
 * Licencia: Apache-2.0 (ver LICENSE en la raíz del repositorio).
 *
 * USO
 * ---
 * Copia el contenido de este fragmento (o inclúyelo) en el
 * `mpconfigport.h` del port de MicroPython que vayas a usar (unix, esp32,
 * stm32, rp2, ...). El port incluye su propio `mpconfigport.h`; este fichero
 * es sólo el fragmento relativo a MatrixFS para no tener que mezclarlo.
 *
 * El módulo se registra con MP_REGISTER_MODULE(MP_QSTR_matrixfs,
 * mp_module_matrixfs) dentro de modmatrixfs.c, por lo que NO se necesita
 * MICROPY_MODULE_BUILTIN_INIT ni ninguna entrada en MICROPY_PORT_BUILTIN_MODULES.
 */

#ifndef MICROPY_PY_MATRIXFS
#define MICROPY_PY_MATRIXFS (1)
#endif

/* Notas:
 *  - El módulo se añade a MicroPython como módulo ADICIONAL; no sustituye al
 *    VFS ni a LittleFS del port.
 *  - La macro MICROPY_PY_MATRIXFS se deja como interruptor documental: la
 *    inclusión efectiva del fichero modmatrixfs.c la decide el build del port
 *    (USER_C_MODULES). Si tu versión de MicroPython condiciona el registro del
 *    módulo a esta macro, defínela antes de compilar modmatrixfs.c.
 *  - No se requiere habilitar VFS ni LittleFS para usar este módulo.
 */
