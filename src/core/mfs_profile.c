/* mfs_profile.c — perfiles por tecnología de memoria (MFS-CAP-001).
 *
 * Cada medio se explota con la granularidad y la estrategia que certifica su
 * propia especificación, en lugar de con una heurística común:
 *
 *   · NOR/NAND/FRAM/MRAM/EEPROM — el host hace el mapeo (motor RAW): ZLF +
 *     E2G + L2P, desgaste y recuperación propios, porque el medio no tiene FTL.
 *   · ZNS — mapeo 1:1 zona↔zona física con reset nativo (motor ZONED).
 *   · SD/eMMC/USB/NVMe — el dispositivo ya tiene FTL (motor MANAGED): el
 *     sistema de archivos asigna por clúster (la unidad certificada del medio)
 *     y no duplica el trabajo del controlador.
 *
 * Datos de la tabla (cita de la fuente en `spec`):
 *   SD   — SD Association, SD Physical Layer Specification: SDXC hasta 2 TB
 *          (SDUC hasta 128 TB); el direccionamiento es por bloque de 512 B y la
 *          unidad de asignación (AU_SIZE) típica de SDXC/SDUC es 4 MB, que es
 *          también la granularidad de borrado (CMD32/33/38).
 *   eMMC — JEDEC JESD84-B51 (eMMC 5.1): AU_SIZE en EXT_CSD[11] (4 MB típico),
 *          ERASE_GRP_SIZE[224]/ERASE_GRP_MULT[225] ⇒ erase group de 512 KiB en
 *          la mayoría de dispositivos; direccionamiento por bloque de 512 B.
 *   NAND — página de 4-16 KB y bloque de borrado de cientos de páginas
 *          (128 KB en 2D SLC/MLC; 1-8 MB en 3D y en SSD): se toma el mínimo
 *          conservador por perfil.
 *   NVMe — NVMe 2.0 (NVM Command Set): bloques lógicos de 512 B o 4 KB, sin
 *          operación de borrado expuesta (deallocate/DSM hace de TRIM); el
 *          tamaño de transferencia óptimo es del orden de 128 KB.
 *
 * Sin heap (MFS-RES-001): la tabla es constante y vive en .rodata.
 */
#include "mfs_internal.h"

#define GIB (1024ull * 1024ull * 1024ull)
#define TIB (1024ull * GIB)
/* Cota de capacidad de los medios gestionados de consumo: 2 TiB, que cubre los
 * 2 TB exigidos (2 TB decimales = 1,82 TiB) y coincide con la frontera de la
 * familia SDXC (2 TB) del estándar SD. */
#define CAP_2T (2ull * TIB)

static const mfs_media_profile_t PROFILES[MFS_MEDIA_COUNT] = {
    /* 0 — genérico/no reconocido: sin geometría certificada ⇒ sólo sirve como
     * respaldo de consulta; `mfs_profile_check` lo rechaza. */
    {"NONE", "—", MFS_MEDIA_NONE, MFS_ENGINE_RAW, 0ull, 0u, 0u, 0u, 0u, 0u, 0u,
     0u, 0u, 0u, 0u},

    /* 1 — NOR SPI/QSPI/OSPI: programable byte a byte, con suspend y lectura en
     * sitio. La cota de 4 GiB es la del motor RAW de 32 bits (los die
     * comerciales no pasan de 4 Gb = 512 MiB). */
    {"NOR_SPI", "JEDEC JESD216 (SFDP) · §3.2", MFS_MEDIA_NOR_SPI,
     MFS_ENGINE_RAW, 4ull * GIB, 65536u, 4096u, 256u, 1u, 1u, 4096u,
     (uint16_t)(MFS_PROF_PPP | MFS_PROF_SUSPEND | MFS_PROF_XIP), 700u, 45000u,
     100u},

    /* 2 — NAND raw (SPI/paralelo): ECC del host, bloque de borrado de 128 KiB
     * (mínimo 2D; 1-8 MB en 3D) y suspensión de erase. */
    {"NAND_RAW", "datasheet NAND 2D/3D · §3.2", MFS_MEDIA_NAND_RAW,
     MFS_ENGINE_RAW, 4ull * GIB, 1u << 20, 131072u, 4096u, 4096u, 4096u, 4096u,
     (uint16_t)(MFS_PROF_SUSPEND | MFS_PROF_MULTIPLANE), 700u, 3500u, 100u},

    /* 3 — NAND ONFI/Toggle: bloque mayor (1 MiB típico 3D), página 8 KB,
     * multi-plano y read-retry. */
    {"NAND_ONFI", "JEDEC JESD230 (ONFI)", MFS_MEDIA_NAND_ONFI, MFS_ENGINE_RAW,
     4ull * GIB, 2u << 20, 1u << 20, 8192u, 8192u, 4096u, 4096u,
     (uint16_t)(MFS_PROF_SUSPEND | MFS_PROF_MULTIPLANE), 800u, 5000u, 120u},

    /* 4 — ZNS: el medio expone zonas con reset nativo; no hay GC de dispositivo
     * que ocultar, así que el mapeo es 1:1 y la cota la fija el medio. */
    {"ZNS_NAND", "NVMe ZNS / UFS 4.0", MFS_MEDIA_ZNS_NAND, MFS_ENGINE_ZONED,
     2ull * TIB, 4u << 20, 4u << 20, 4096u, 4096u, 4096u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_CQE | MFS_PROF_ECC_ON_DIE), 700u,
     4000u, 100u},

    /* 5 — FRAM: byte-addressable, sin borrado ni desgaste efectivo. */
    {"FRAM", "datasheet FRAM SPI · §11.11", MFS_MEDIA_FRAM, MFS_ENGINE_RAW,
     16ull << 20, 4096u, 0u, 1u, 1u, 1u, 4096u,
     (uint16_t)(MFS_PROF_BYTE_ADDR | MFS_PROF_NO_ERASE), 100u, 0u, 20u},

    /* 6 — MRAM: byte/word-addressable, sin borrado. */
    {"MRAM", "datasheet MRAM SPI · §11.11", MFS_MEDIA_MRAM, MFS_ENGINE_RAW,
     1ull * GIB, 4096u, 0u, 64u, 8u, 1u, 4096u,
     (uint16_t)(MFS_PROF_BYTE_ADDR | MFS_PROF_NO_ERASE), 100u, 0u, 30u},

    /* 7 — EEPROM: escritura por byte, latencia de programación alta. */
    {"EEPROM", "datasheet EEPROM I2C/SPI", MFS_MEDIA_EEPROM, MFS_ENGINE_RAW,
     1ull << 20, 256u, 0u, 64u, 1u, 1u, 512u,
     (uint16_t)(MFS_PROF_BYTE_ADDR | MFS_PROF_NO_ERASE), 5000u, 0u, 500u},

    /* 8 — SD (SDXC/SDUC): FTL propio ⇒ motor MANAGED. AU de 4 MB, direccionado
     * por bloque de 512 B y borrado/discard a granularidad de AU. */
    {"SD", "SD Physical Layer Spec (SDXC ≤ 2 TB · SDUC ≤ 128 TB)", MFS_MEDIA_SD,
     MFS_ENGINE_MANAGED, CAP_2T, 4u << 20, 4u << 20, 512u, 512u, 512u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_REMOVABLE | MFS_PROF_ECC_ON_DIE),
     10000u, 250000u, 500u},

    /* 9 — eMMC 5.1: AU_SIZE del EXT_CSD (4 MB típico) y erase group de 512 KiB;
     * command queuing y suspensión disponibles. */
    {"EMMC", "JEDEC JESD84-B51 (eMMC 5.1)", MFS_MEDIA_EMMC, MFS_ENGINE_MANAGED,
     CAP_2T, 4u << 20, 512u << 10, 512u, 512u, 512u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_CQE | MFS_PROF_SUSPEND |
                MFS_PROF_ECC_ON_DIE),
     10000u, 100000u, 400u},

    /* 10 — USB flash (MSC/UASP): FTL opaco; no se asume borrado y se usa un
     * clúster moderado para no amplificar escrituras pequeñas. */
    {"USB", "USB MSC / UASP (SCSI SBC-4)", MFS_MEDIA_USB, MFS_ENGINE_MANAGED,
     2ull * TIB, 1u << 20, 0u, 512u, 512u, 512u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_REMOVABLE | MFS_PROF_ECC_ON_DIE),
     20000u, 0u, 800u},

    /* 11 — SSD NVMe (PCIe): LBA de 512 B/4 KB, deallocate como TRIM y colas
     * profundas. Cota de perfil: 8 TB (M.2 2280 actual; el estándar admite
     * más). */
    {"NVME", "NVMe 2.0 (NVM Command Set)", MFS_MEDIA_NVME, MFS_ENGINE_MANAGED,
     8ull * TIB, 1u << 20, 0u, 4096u, 512u, 512u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_CQE | MFS_PROF_ECC_ON_DIE), 1000u, 0u,
     100u},

    /* 12 — SSD SATA (ATA-8/ACS-4, SCSI SBC): bloques lógicos de 512 B o 4 KB y
     * TRIM por DATA SET MANAGEMENT / UNMAP; sin operación de borrado expuesta
     * (el FTL del dispositivo la gestiona). */
    {"SATA", "ATA-8/ACS-4 · SCSI SBC (UNMAP/DSM)", MFS_MEDIA_SATA,
     MFS_ENGINE_MANAGED, 4ull * TIB, 1u << 20, 0u, 512u, 512u, 512u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_ECC_ON_DIE), 2000u, 0u, 200u},

    /* 13 — UFS (JEDEC JESD220): LUN de bloques de 4 KB, purge/erase group y
     * command queuing. Cota de perfil: 4 TB. */
    {"UFS", "JEDEC JESD220 (UFS 3.1/4.0)", MFS_MEDIA_UFS, MFS_ENGINE_MANAGED,
     4ull * TIB, 4u << 20, 4u << 20, 4096u, 4096u, 4096u, 4096u,
     (uint16_t)(MFS_PROF_TRIM | MFS_PROF_CQE | MFS_PROF_SUSPEND |
                MFS_PROF_ECC_ON_DIE),
     1000u, 10000u, 100u},
};

const mfs_media_profile_t *mf_media_profile(mfs_media_type_t t) {
  if ((unsigned)t >= (unsigned)MFS_MEDIA_COUNT)
    return &PROFILES[0]; /* genérico: sin geometría certificada */
  return &PROFILES[(unsigned)t];
}

const char *mf_engine_name(mfs_engine_t e) {
  switch (e) {
  case MFS_ENGINE_RAW:
    return "RAW";
  case MFS_ENGINE_ZONED:
    return "ZONED";
  case MFS_ENGINE_MANAGED:
    return "MANAGED";
  default:
    return "?";
  }
}

mfs_st mfs_profile_check(mfs_media_type_t t, uint64_t bytes) {
  const mfs_media_profile_t *p = mf_media_profile(t);
  if (p->max_bytes == 0ull)
    return MFS_EINVAL; /* medio desconocido: no se puede certificar */
  if (bytes > p->max_bytes)
    return MFS_ENOTVIABLE; /* MFS-CAP-001: excede el máximo del perfil */
  return MFS_OK;
}
