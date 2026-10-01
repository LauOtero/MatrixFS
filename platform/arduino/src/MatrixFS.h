/* MatrixFS.h — envoltorio Arduino (C++) de MatrixFS Ultra «ATLAS».
 *
 * Licencia: Apache-2.0.
 *
 * Esta clase es una fachada delgada sobre la API C pública del núcleo
 * (include/matrixfs/matrixfs.h) y sobre la capa de integración embebida
 * (platform/embedded/mfs_embedded.h). NO reimplementa lógica del sistema de
 * archivos: sólo resuelve la región de flash del MCU, aporta los callbacks
 * read/prog/erase y delega en el núcleo.
 *
 * Notas de diseño:
 *   · Sin heap. La instancia del núcleo (mf_t) es un miembro ESTÁTICO de la
 *     clase para no reservarla dentro de cada objeto: su tamaño depende de los
 *     macros MFS_MAX_* del modo de compilación (del orden de decenas de KB en
 *     la configuración de 16/32 bits). Por eso el envoltorio soporta UNA sola
 *     instancia montada a la vez (begin() devuelve false con MFS_EBUSY en la
 *     segunda).
 *   · El tipo mf_t es opaco en la API pública; su definición completa vive en
 *     src/mfs_internal.h y se incluye únicamente en el .cpp.
 *   · Comentarios y mensajes en español; sin emojis.
 */
#ifndef MATRIXFS_ARDUINO_MATRIXFS_H
#define MATRIXFS_ARDUINO_MATRIXFS_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

#include "matrixfs/matrixfs.h"
#include "mfs_embedded.h"

/* Fachada Arduino sobre un volumen MatrixFS alojado en una región de flash
 * reservada. Uso típico:
 *
 *   MatrixFS mfs("matrixfs");          // ESP32: etiqueta de la partición
 *   void setup() { mfs.begin(true); ... }
 */
class MatrixFS {
public:
  /* Callback de listado: devuelve true para continuar la iteración, false para
   * detenerla. `entry` sólo es válido durante la llamada. */
  typedef bool (*ListCallback)(const mfs_dirent *entry, void *user);

  /* Construye el envoltorio.
   *   partitionLabel: etiqueta de la partición (ESP32) o nombre informativo.
   *   ramTotal:   RAM en bytes declarada al núcleo (0 = presupuesto por
   *               defecto de mfs_embedded_opts_default: 32 KB).
   *   sizeBytes:  tamaño de la región reservada en bytes. Necesario en ESP8266
   *               y RP2040 (en ESP32 se deduce de la partición).
   *   baseAddr:   dirección/offset inicial de la región reservada. Necesario en
   *               ESP8266 (offset de flash) y RP2040 (offset sobre XIP_BASE).
   */
  MatrixFS(const char *partitionLabel = "matrixfs", uint32_t ramTotal = 0,
           uint32_t sizeBytes = 0, uint32_t baseAddr = 0);
  ~MatrixFS();

  /* Monta el volumen. Si formatIfNeeded es true, formatea cuando el volumen
   * está ausente o corrupto (mfs_embedded_mount con format_if_needed). */
  bool begin(bool formatIfNeeded = false);

  /* Formatea el volumen y lo deja montado. Destruye el contenido previo. */
  bool format();

  /* Desmonta y libera el estado (mf_deinit). */
  void end();

  bool mounted() const { return _mounted; }

  /* Escribe `len` bytes en `path` (crea o trunca). Devuelve MFS_OK o error. */
  int writeFile(const char *path, const void *data, size_t len);
  /* Lee hasta `cap` bytes de `path`. Si `out` != NULL recibe los leídos. */
  int readFile(const char *path, void *buf, size_t cap, size_t *out);

  int mkdir(const char *path);
  int remove(const char *path);
  int rename(const char *from, const char *to);
  bool exists(const char *path);

  /* Lista `path`: bien por callback, bien acumulando nombres en `out`
   * (separados por '\n'). Devuelve MFS_OK o error. */
  int list(const char *path, ListCallback cb, void *user);
  int list(const char *path, String &out);

  /* Modo operativo seleccionado por el núcleo (MFS_MODE_*). */
  mfs_mode_t mode() const;

  /* Diagnóstico: último estado y su texto (mfs_ststr). */
  mfs_st lastError() const { return _lastError; }
  const char *lastErrorString() const;

  uint32_t partitionSize() const { return _flash.size; }
  uint32_t partitionBase() const { return _flash.base_addr; }
  const char *partitionLabel() const { return _label; }

private:
  bool resolveRegion();
  void configureFlash();
  int record(mfs_st st);

  const char *_label;
  uint32_t _ramTotal;
  uint32_t _sizeBytes;
  uint32_t _baseAddr;
  bool _mounted;
  mfs_st _lastError;
  mfs_embedded_flash_t _flash;
  mfs_embedded_opts _opts;

  /* Instancia única del núcleo. Tipo opaco: sólo se define en el .cpp. */
  static mf_t _fs;
};

#endif /* MATRIXFS_ARDUINO_MATRIXFS_H */
