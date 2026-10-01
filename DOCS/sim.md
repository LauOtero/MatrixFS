# Simulador: vFlash, vFRAM y puerto de host

Spec: §20.2, §20.3, §25, §27.2.
Implementación: `sim/vflash.{h,c}`, `sim/vfram.{h,c}`, `sim/mfs_port_host.c`;
arnés de pruebas en `tests/mfs_harness.{h,c}`.

Todo el núcleo se valida contra **medios simulados**, no contra hardware: el
driver L2 (§20.2) es la única frontera entre el núcleo y la memoria, de modo que
sustituirlo por un simulador que respete la semántica del protocolo basta para
ejercitar el sistema completo (formateo, WAL, GC, recuperación, permisos) en
cualquier máquina de desarrollo, sin privilegios ni hardware.

| Simulador | Medio | Papel |
|---|---|---|
| `vflash` | NOR SPI | medio T1 principal: borrado por bloques, programación con regla 1→0 |
| `vfram` | FRAM/MRAM | tier T0 (HMT §11.11): byte-addressable, sin borrado |
| `mfs_port_host` | — | puerto §20.2 del anfitrión: ciclos, tiempo, secciones críticas, WFI |

## vFlash (`sim/vflash.h`)

```c
typedef struct {
  uint8_t *img;              /* imagen persistente del medio */
  uint32_t size;             /* bytes totales */
  uint32_t erase_unit;       /* bytes por bloque borrable */
  bool     crashed;          /* true ⇒ toda operación devuelve MFS_EIO */
  uint32_t n_read, n_prog, n_erase, n_violations;
  uint32_t viol_addr;        /* dirección de la 1.ª violación NOR */
  bool     fail_next_prog;   /* inyección de fallo de un solo uso (§27.2) */
  uint16_t pe_max;           /* máximo de ciclos P/E observado */
  uint16_t pe[VF_MAX_BLOCKS];
  mfs_l2_driver drv;         /* driver L2 asociado (ctx = este) */
} vflash_t;
```

| Constante | Valor |
|---|---|
| `VF_SECTOR` | `4096` — borrado mínimo NOR (§25) |
| `VF_MAX_BLOCKS` | `256` — cubre medios de hasta 1 MiB con sector de 4 KiB |

### API

| Función | Comportamiento |
|---|---|
| `vf_init(vf, size, erase_unit)` | Imagen de `size` bytes a `0xFF` (estado virgen); `erase_unit == 0` ⇒ `VF_SECTOR`; exige `size != 0`, `erase_unit` potencia de dos y `size >= erase_unit`; `false` si falla la reserva |
| `vf_free(vf)` | Libera la imagen (los contadores de telemetría se conservan) |
| `vf_geom(vf, g)` | Geometría NOR para el HAL (tabla siguiente) |
| `vf_driver(vf)` | Driver L2 con `ctx = vf` |
| `vf_crash(vf)` / `vf_recover(vf)` | Corte y re-alimentación: la imagen **no** se altera (§12.3) |
| `vf_fail_next_prog(vf, on)` | Arma el fallo inyectado del siguiente `prog` (§27.2, FIH) |
| `vf_raw(vf, addr)` | Acceso directo post-mortem (`NULL` si fuera de rango) |

### Semántica de programación (NOR)

1. Validaciones previas: `ctx`/`src` nulos o fuera de rango ⇒ `MFS_EINVAL`;
   `len == 0` ⇒ `MFS_OK`; `crashed` ⇒ `MFS_EIO`.
2. Fallo inyectado armado ⇒ se consume y devuelve `MFS_EIO` **sin tocar la
   imagen**: reproduce un corte de energía en el momento exacto de programar.
3. **Regla 1→0**: se recorre el búfer completo **antes** de escribir; si algún
   byte exige subir un bit de 0 a 1 (NOR no lo permite) ⇒ `MFS_EIO`, se
   incrementa `n_violations` y, sólo en la **primera** violación histórica, se
   guarda `viol_addr`. La operación es **atómica**: ante violación no se
   modifica ningún byte ni se cuenta la programación.
4. Programación real: `img[i] &= src[i]` (sólo pone bits a cero).
5. **Barrera WOB (§20.3)**: read-back del rango; si no coincide con lo
   solicitado ⇒ `MFS_ECORRUPT`. En NOR la máscara AND es determinista, por lo
   que esta ruta es defensiva; en la especificación el bloque pasaría a
   cuarentena (aquí sólo se reporta).
6. Éxito: `n_prog++`.

### Borrado

`MFS_EINVAL` si la dirección no está alineada a `erase_unit` o si el bloque se
sale del medio; `MFS_EIO` si el medio está sin alimentación. Se limpia el bloque
completo a `0xFF`, se incrementa `n_erase` y el contador P/E del bloque
(`pe[blk]`, saturado en `0xFFFF`) actualizando `pe_max`. **No hay read-back** en
borrado.

`vf_do_suspend`/`vf_do_resume` son *stubs* que devuelven `MFS_OK`: el simulador
no modela tiempos de suspensión, pero declarar la capacidad permite ejercitar
las rutas de FTL 2 que la consumen.

### Geometría NOR

| Campo | Valor |
|---|---|
| `type` | `MFS_MEDIA_NOR_SPI` |
| `size` / `erase_unit` | los del objeto |
| `program_granularity` | `1` (programable byte a byte) |
| `page_size` | `256` |
| `t_prog_max_us` / `t_erase_max_us` / `t_read_max_us` | `700` / `45000` / `100` (datasheet típico §25) |
| `t_suspend_max_us` | `20` |
| `flags0` | `SUSPEND_E | SUSPEND_P` |
| `flags1` | `MFS_HWV1_PPP` |
| `zones_per_block` | `1` |

`flags1 = PPP` (partial-page-program verificado) es lo que habilita **WOM-p**
(§11.4): el simulador cumple la regla 1→0, que es justo la precondición de la
reescritura program-only bit-lane.

## vFRAM — tier T0 (`sim/vfram.h`)

```c
typedef struct {
  uint8_t *img;
  uint32_t size;
  bool     crashed;
  uint32_t n_read, n_prog;
  uint32_t n_writes[256];   /* escrituras por bloque de 256 B (desgaste) */
  mfs_l2_driver drv;
} vfram_t;
```

| Función | Comportamiento |
|---|---|
| `vfram_init(fr, size)` | Imagen a `0xFF`; `false` si `size == 0` o falla la reserva |
| `vfram_free(fr)` | Libera la imagen |
| `vfram_geom(fr, g)` | `type = MFS_MEDIA_FRAM`, `program_granularity = 1`, `page_size = 1`, `flags1 = BYTE_ADDR | ASYM`, `erase_unit = 0` |
| `vfram_driver(fr)` | Driver L2 con `ctx = fr`, `suspend`/`resume` = `NULL` |
| `vfram_crash/recover`, `vfram_raw` | Igual que en vFlash |

Diferencias esenciales frente a NOR:

- **Programación directa**: `prog` reescribe (`memcpy`) sin regla 1→0 y sin
  borrado previo, con desgaste contabilizado por bloque de 256 B.
- **No hay borrado**: `vfr_do_erase` devuelve **siempre** `MFS_EINVAL`. El
  núcleo no debe invocarlo para T0; es la razón por la que el token T0 se
  escribe byte-directo (MFS-HMT-001, véase [`wal.md`](wal.md)).
- Sin read-back (el medio garantiza la escritura del byte programado).

Estas propiedades son exactamente las que hacen de T0 el lugar idóneo para el
token y el espejo de metadatos: **una escritura por transacción sin borrado**.

## Puerto de host (`sim/mfs_port_host.c`)

Implementa las cinco primitivas obligatorias del contrato §20.2:

| Primitiva | Linux | Windows |
|---|---|---|
| `mfs_port_cycles()` | `clock_gettime(CLOCK_MONOTONIC)` en ns, truncado a 32 bits | `QueryPerformanceCounter` (contador crudo) |
| `mfs_port_time_us()` | `tv_sec·10⁶ + tv_nsec/1000` | `c.QuadPart · 10⁶ / frecuencia` (frecuencia obtenida de forma perezosa) |
| `mfs_port_crit_enter()` / `crit_exit()` | vacías | vacías |
| `mfs_port_wfi()` | vacía | vacía |

Notas de fidelidad, importantes para interpretar los resultados:

- Los contadores son de **32 bits** y envuelven; las medidas del banco de
  pruebas son válidas para duraciones cortas (el banco reintenta si detecta
  envoltura).
- Las secciones críticas y `wfi` son no-ops porque el anfitrión es
  multihilo pero **el núcleo no es reentrante**: la serialización efectiva la
  hace el mutex de `mfs_vfs` (véase
  [`linux-integration.md`](linux-integration.md)).
- **No hay barrera de memoria** expuesta: la "barrera WOB" es semántica de
  protocolo (el `prog` retorna sólo tras verificar el estado), no un *fence* de
  CPU. En hardware real, el driver debe garantizar el orden.

## Arnés de pruebas (`tests/mfs_harness.h`)

| Función | Comportamiento |
|---|---|
| `env_open(e, ram_total, key)` | Medio NOR de **1 MiB** (`MFS_TEST_SIZE`) con `erase_unit = MFS_TEST_SECTOR = 4096`, geometría derivada y `mfs_config` completo (`arch_class = 2`, `forced_mode = MFS_MODE_UNSUPPORTED`, clave opcional) |
| `env_open_t0(e, ram_total, key, t0_size)` | Igual, más un tier T0 FRAM de `t0_size` bytes (`cfg.drv_t0` / `cfg.geom_t0`, `has_t0 = true`) |
| `env_format(e)` | Formatea y monta |
| `env_remount(e)` | `mf_deinit` + `mf_init`: verifica la **persistencia** en el medio |
| `env_close(e)` | Desmonta y libera los medios |
| `env_set_assets(e, flags2, flags3)` | Simula los *assets* del HAL (PUF, rail, supercap, aceleradores) marcando bits del HWV |

El medio de 1 MiB con sector de 4 KiB da 256 bloques, exactamente el rango
cubierto por `VF_MAX_BLOCKS`: la contabilidad de desgaste es completa en todas
las pruebas.

`env_open_t0` recibe el tamaño de T0 del llamante: es lo que permite probar
tanto la activación normal (64 KiB) como el rechazo por insuficiencia (4 KiB)
en `test_tier`.

## Fallo inyectado (§27.2, FIH)

`vf_fail_next_prog()` es el gancho que hace determinista el *Fault Injection
Harness*: el fallo cae en una programación **concreta** (la siguiente), no en un
momento aleatorio. Combinado con `vf_crash()` permite dos escenarios distintos:

| Escenario | Efecto |
|---|---|
| `vf_fail_next_prog` | la programación falla y el medio queda **inalterado** |
| `vf_crash` / `vf_recover` | el medio rechaza toda operación mientras está caído y **conserva** lo ya programado |

La recuperación tras el corte se valida remontando (`env_remount`) y comprobando
que el estado reconstruido (WAL replay + escaneo E2G) es consistente; la suite
`test_fih` recorre las combinaciones de corte en formateo, escritura, commit y
checkpoint.

## Errores

| Código | Causa |
|---|---|
| `MFS_EINVAL` | `ctx`/búfer nulo, rango fuera del medio, borrado desalineado, `erase` sobre FRAM |
| `MFS_EIO` | medio sin alimentación; violación 1→0 en NOR; fallo inyectado |
| `MFS_ECORRUPT` | read-back de la barrera WOB no coincide |
| `MFS_OK` | operación de longitud cero; programación/lectura/borrado correctos; `suspend`/`resume` |
