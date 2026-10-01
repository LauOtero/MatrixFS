/* Basic.ino — ejemplo mínimo del envoltorio Arduino de MatrixFS.
 *
 * Licencia: Apache-2.0.
 *
 * Monta la región de flash reservada (partición "matrixfs" en ESP32), formatea
 * si hace falta, escribe un fichero y lo vuelve a leer, imprimiendo el
 * resultado por el puerto serie.
 *
 * En ESP8266/RP2040 hay que indicar base y tamaño de la región reservada
 * (no existe detección de partición homogénea); ajusta la línea comentada.
 */

#include <MatrixFS.h>
#include <string.h>

/* ESP32: la región se localiza por la etiqueta de la partición. */
static MatrixFS mfs("matrixfs");

/* ESP8266/RP2040: descomenta y ajusta a tu layout reservado.
 *   static MatrixFS mfs("matrixfs", 32768u, 0x00100000u, 0x00100000u);
 *   (label, ramTotal, sizeBytes, baseAddr) */

void setup() {
  Serial.begin(115200);
  delay(300);

  /* Format if needed: crea el volumen la primera vez. */
  if (!mfs.begin(true)) {
    Serial.print("MatrixFS begin() fallo: ");
    Serial.println(mfs.lastErrorString());
    return;
  }

  Serial.print("MatrixFS montado. Modo=");
  Serial.println((int)mfs.mode());
  Serial.print("Particion base=0x");
  Serial.print(mfs.partitionBase(), HEX);
  Serial.print(" size=");
  Serial.println(mfs.partitionSize());

  const char *msg = "Hola desde MatrixFS";
  int wr = mfs.writeFile("/hola.txt", msg, strlen(msg));
  if (wr != 0) {
    Serial.print("writeFile fallo: ");
    Serial.println(mfs.lastErrorString());
    return;
  }

  char buf[64];
  size_t got = 0;
  int rd = mfs.readFile("/hola.txt", buf, sizeof(buf) - 1, &got);
  if (rd != 0) {
    Serial.print("readFile fallo: ");
    Serial.println(mfs.lastErrorString());
    return;
  }
  buf[got] = '\0';
  Serial.print("Leido (");
  Serial.print((unsigned)got);
  Serial.print(" bytes): ");
  Serial.println(buf);

  String listado;
  mfs.list("/", listado);
  Serial.print("Contenido de /: ");
  Serial.println(listado);

  /* Desmontaje limpio (persiste metadatos y ceroiza secretos). */
  mfs.end();
}

void loop() { /* Nada: el ejemplo es de un solo uso. */ }
