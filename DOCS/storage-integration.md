# Integración de almacenamiento flash

Guía de integración de las unidades flash soportadas sobre la capa de
abstracción de almacenamiento del proyecto.

Spec: §3.2 (medios soportados), §5.1 (detección), §8.1 (layout), §11 (FTL),
MFS-CAP-001 (perfiles por tecnología). Ver también
[`media-profiles.md`](media-profiles.md) (ficha de cada medio) y
[`known-limitations.md`](known-limitations.md) (estado honesto).

## 1. Los tres motores: quién hace el trabajo

La estrategia **la dicta el medio**, no una heurística única. Cada tecnología
declara su motor en `mfs_media_profile_t` (`src/core/mfs_profile.c`):

| Motor | Medios | Quién hace el mapeo físico, el *wear leveling*, el *bad block management* y el ECC |
|---|---|---|
| **RAW** | NOR SPI/QSPI/OSPI, NAND raw, ONFI/Toggle, FRAM, MRAM, EEPROM | **MatrixFS** (FTL del núcleo: ZLF + E2G + L2P, desgaste, ECC del host y recuperación) |
| **ZONED** | ZNS NAND / UFS 4.0 | La zona se mapea **1:1**; el reset es nativo del dispositivo |
| **MANAGED** | SD, eMMC, UFS, USB, NVMe, SSD SATA | **El propio dispositivo** (su FTL interno resuelve reubicación, desgaste, ECC y TRIM) |

> Es un reparto explícito: en RAW, MatrixFS asume el coste en RAM del FTL; en
> MANAGED, MatrixFS asigna por clúster y **no duplica** el FTL del controlador.

### Qué implica para *wear leveling*, *bad block management* y ECC

| Mecanismo | RAW | ZONED | MANAGED |
|---|---|---|---|
| Wear leveling | Implementado en el núcleo (ZLF/E2G, §11) | Del dispositivo | Del dispositivo |
| Bad block management | Tabla BMT del núcleo + `bad_block_probe` del HAL | Del dispositivo | Del dispositivo |
| ECC | ECC del host sobre el código de corrección del núcleo; en NOR/NAND con ECC *on-die* se delega | Del dispositivo | Del dispositivo (`MFS_HWV0_ECC_ON_DIE`) |

## 2. Medios cubiertos y perfil certificado

| Unidad | Tipo | Motor | Perfil | Interfaz / API nativa a enganchar |
|---|---|---|---|---|
| **NOR / SPI Flash** | `MFS_MEDIA_NOR_SPI` | RAW | 4 GiB (motor RAW) | SPI/QSPI/OSPI; `mfs_l2_8bit.c` (`mfs_l2_nor_spi`) |
| **NAND raw** | `MFS_MEDIA_NAND_RAW` | RAW | 4 GiB | NAND paralelo/SPI con ECC del host |
| **NAND ONFI/Toggle** | `MFS_MEDIA_NAND_ONFI` | RAW | 4 GiB | JEDEC JESD230 |
| **eMMC 5.1** | `MFS_MEDIA_EMMC` | MANAGED | 2 TB | `sdmmc`/`mmc` del SDK |
| **SD (SDXC/SDUC)** | `MFS_MEDIA_SD` | MANAGED | 2 TB | `sdspi`/`sdmmc` del SDK |
| **UFS 3.1/4.0** | `MFS_MEDIA_UFS` | MANAGED | 4 TiB | JEDEC JESD220 (UniPro/UFSHCI) |
| **SSD NVMe** | `MFS_MEDIA_NVME` | MANAGED | 8 TiB | NVMe 2.0 (NVM Command Set) |
| **SSD SATA** | `MFS_MEDIA_SATA` | MANAGED | 4 TiB | AHCI/ATA-8 + SCSI SBC (UNMAP/DSM) |
| **USB flash** | `MFS_MEDIA_USB` | MANAGED | 2 TB | USB MSC / UASP |
| **ZNS NAND** | `MFS_MEDIA_ZNS_NAND` | ZONED | 2 TiB | NVMe ZNS / UFS 4.0 |

`MFS_MEDIA_SATA` y `MFS_MEDIA_UFS` se añadieron al modelo en
`include/matrixfs/mfs_types.h` (con sus perfiles en `src/core/mfs_profile.c`).

## 3. El adaptador L2 para medios gestionados

Los medios gestionados no exponen bytes ni bloques de borrado: exponen
**sectores** (LBA) y, opcionalmente, TRIM/UNMAP. El adaptador
[`platform/common/mfs_l2_managed.h`](../platform/common/mfs_l2_managed.h)
traduce esa API de sector a un `mfs_l2_driver` de MatrixFS:

- Traduce byte-offset ⇄ LBA y hace **RMW alineado a sector** (el medio solo
  admite escritura de sector completo) usando un *bounce buffer* que aporta el
  integrador (sin heap).
- Emite **TRIM/UNMAP** en `erase()` cuando el dispositivo lo expone (no-op si
  no hay `trim_sectors`).
- Deriva la geometría (`MFS_ENGINE_MANAGED`, `erase_unit`, `sector_size`) y
  valida que el medio pertenezca al motor MANAGED (`mfs_profile_check`).

El integrador solo implementa **2-3 callbacks de sector** sobre la API nativa
del SDK.

### Firmas que debes rellenar

```c
typedef struct mfs_managed_dev {
  mfs_st (*read_sectors)(void *ctx, uint32_t lba, uint32_t count, void *dst);
  mfs_st (*write_sectors)(void *ctx, uint32_t lba, uint32_t count, const void *src);
  mfs_st (*trim_sectors)(void *ctx, uint32_t lba, uint32_t count); /* NULL ⇒ no */
  mfs_st (*flush)(void *ctx);                                      /* NULL ⇒ no */
  void *ctx;
  uint16_t sector_size;   /* 512 o 4096 */
  uint32_t sector_count;
  uint32_t t_read_max_us, t_write_max_us, t_trim_max_us;
  uint8_t *bounce;        /* RMW: >= sector_size (estático, lo aporta el integrador) */
  uint32_t bounce_len;
  const char *name;       /* diagnóstico */
} mfs_managed_dev_t;
```

## 4. Ejemplo completo (SD/eMMC/UFS/NVMe/SATA)

```c
#include "matrixfs/matrixfs.h"
#include "mfs_l2_managed.h"
#include "mfs_internal.h"        /* mf_t es opaco: necesario para instanciar */
#include <string.h>              /* memset */

/* --- 1. Bounce buffer estático (sin heap) --- */
static uint8_t g_bounce[4096];

/* --- 2. Callbacks sobre la API nativa del SDK --- */
static mfs_st sd_read(void *ctx, uint32_t lba, uint32_t n, void *dst) {
  (void)ctx;
  return sdmmc_read_sectors(g_sd, lba, n, dst) == 0 ? MFS_OK : MFS_EIO;
}
static mfs_st sd_write(void *ctx, uint32_t lba, uint32_t n, const void *src) {
  (void)ctx;
  return sdmmc_write_sectors(g_sd, src, lba, n) == 0 ? MFS_OK : MFS_EIO;
}
static mfs_st sd_trim(void *ctx, uint32_t lba, uint32_t n) {
  (void)ctx;
  return sdmmc_erase_sectors(g_sd, lba, n);   /* o NULL si no se expone */
}

/* --- 3. Descriptor + adaptador --- */
static mfs_managed_dev_t g_dev = {
    .read_sectors = sd_read, .write_sectors = sd_write, .trim_sectors = sd_trim,
    .flush = NULL, .ctx = NULL,
    .sector_size = 512, .sector_count = 0,          /* 0 ⇒ se ajusta al detectar */
    .t_read_max_us = 100000, .t_write_max_us = 250000, .t_trim_max_us = 500000,
    .bounce = g_bounce, .bounce_len = sizeof(g_bounce), .name = "microSD",
};
static mfs_managed_l2_t g_l2;
static mfs_config g_cfg;
static mf_t g_fs;

/* --- 4. Montaje --- */
void arranque(void) {
  g_dev.sector_count = sd_num_sectors(g_sd);
  if (mfs_managed_l2_init(&g_l2, &g_dev, MFS_MEDIA_SD) != MFS_OK)
    return;                                        /* geometría/medio inválidos */

  memset(&g_cfg, 0, sizeof(g_cfg));
  g_cfg.drv = &g_l2.drv;
  g_cfg.geom = &g_l2.geom;
  g_cfg.ram_total = 32u * 1024u;                   /* RAM física disponible */
  g_cfg.arch_class = MFS_ARCH_AUTO;                /* MFS-ARCH-010 rev.3 */
  g_cfg.forced_mode = MFS_MODE_UNSUPPORTED;        /* modo automático */
  g_cfg.suite_preferred = 0xFFu;                   /* negociar */

  if (mf_init(&g_fs, &g_cfg) != MFS_OK) {           /* primer uso: sin volumen */
    mf_format(&g_fs, NULL);
    if (mf_init(&g_fs, &g_cfg) != MFS_OK) return;
  }
}
```

Mismo patrón para **eMMC** (`MFS_MEDIA_EMMC`), **UFS** (`MFS_MEDIA_UFS`, escribe
sobre el driver UFSHCI), **NVMe** (`MFS_MEDIA_NVME`, cola de comandos NVM y
`deallocate`) y **SATA** (`MFS_MEDIA_SATA`, AHCI + UNMAP/DSM).

## 5. En qué se apoya cada unidad (por interfaz)

| Unidad | Detecta / inicializa | Transferencia | TRIM |
|---|---|---|---|
| microSD | `CMD0/CMD8/ACMD41/CMD58` | `CMD17/CMD18` (single/multi) | `CMD32/33/38` |
| eMMC | `CMD1/CMD8` + `EXT_CSD` a `AU_SIZE`/`ERASE_GRP` | `CMD17/18/24/25` | `CMD35/36/38` |
| UFS | UniPro + `UFSHCI` (DME) | `UTRD`/`UTP` | `UNMAP` |
| NVMe | `Identify` + `Set Features` | SQ/CQ NVM Read/Write | `Deallocate` (DSM) |
| SATA | `Identify Device` | AHCI (FIS) | `UNMAP`/`DATA SET MANAGEMENT` |
| NOR/NAND | JEDEC `0x9F`/SFDP/ONFI | `mfs_l2_8bit.c` | erase nativo |

## 6. Validación

- **Host (verificado):** `tests/test_managed.c` usa el simulador
  [`tests/vblk_sim.h`](../tests/vblk_sim.h)/[`.c`](../tests/vblk_sim.c) — un
  dispositivo gestionado simulado en memoria (imagen a `0xFF`, contadores de
  E/S, TRIM y fallo inyectable). Cubre: rechazo de un medio no gestionado,
  derivación de geometría, `mf_format` + TRIM, E/S **de 700 B** (no múltiplo de
  sector ⇒ RMW), remontaje/persistencia, idempotencia y fallo de medio
  tipificado (`MFS_EIO`).
- **Perfiles:** `tests/test_profile.c` valida las fichas de todos los medios
  (incluidos SATA y UFS) y las cotas de capacidad (MFS-CAP-001).
- **Hardware real (pendiente):** la validación de rendimiento y estabilidad
  sobre unidades físicas (secuencial/aleatorio > 95 % de la partición, relectura
  total, remontajes y FIH) exige el banco **F5** de
  [`media-profiles.md`](media-profiles.md) y hardware; en este entorno no hay
  2 TB de medio ni de disco.

Ejecuta la parte verificable con:

```sh
make test        # incluye test_managed_media y test_media_profiles
```

## 7. Estado honesto

| Elemento | Estado |
|---|---|
| Modelo de medio SATA/UFS + perfiles | **Implementado y probado** (host) |
| Adaptador L2 MANAGED (RMW + TRIM) | **Implementado y probado** (host, con dispositivo simulado) |
| NOR/NAND/FRAM/EEPROM/SPI drivers | Implementados en `platform/common/mfs_l2_8bit.c` (ver [`embedded-integration.md`](embedded-integration.md)) |
| FTL RAW (desgaste, BMT, ECC host) | Implementado en el núcleo (§11); ECC *on-die* delegado en RAW con `MFS_HWV0_ECC_ON_DIE` |
| Drivers NVMe/SATA/UFS/SD/eMMC de bajo nivel | **Adaptador genérico + plantilla**: MatrixFS no reimplementa el controlador; consume la API de sector del SDK del silicio (véase §3-§4). El enganche concreto depende del SoC. |
| Validación en hardware real | **No verificada aquí** (requiere el banco F5) |
