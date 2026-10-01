/* mfs_types.h — MatrixFS Ultra «ATLAS» v1.0 — tipos básicos, estados y límites
 *
 * Referencias normativas:
 *   §3.1  MFS-ARCH-010 rev.3 (clases de arquitectura 8/16/32/64 bits)
 *   §7.4  Errores tipificados
 *   §18.2 Límites por modo
 *   §25   Tabla de constantes normativas
 *   §20.1 Reglas de codificación (C11, <stdint.h>, sin números mágicos sueltos)
 */
#ifndef MATRIXFS_MFS_TYPES_H
#define MATRIXFS_MFS_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ==== Clases de arquitectura (MFS-ARCH-010 rev. 3) ======================
 * MatrixFS se adapta a objetivos de 8, 16, 32 y 64 bits. La clase es un dato
 * del HWV (§5.2) y gobierna la selección de modo y el reparto de RAM (§6.1).
 * La detección es automática (src/core/mfs_arch.c); el integrador puede
 * forzarla declarando cfg->arch_class.
 * ======================================================================== */
typedef enum {
  MFS_ARCH_8BIT = 0,
  MFS_ARCH_16BIT = 1,
  MFS_ARCH_32BIT = 2,
  MFS_ARCH_64BIT = 3,
  MFS_ARCH_AUTO = 0xFF /* cfg.arch_class ⇒ autodetectar */
} mfs_arch_class_t;

/* Capacidades de aceleración por hardware detectadas (MFS-HW-001).
 * La ausencia de un bit ⇒ se usa la ruta software equivalente (misma API y
 * mismo resultado, MFS-HW-001). */
#define MFS_HWACCEL_NONE 0x0000u
#define MFS_HWACCEL_CRC32C                                                     \
  0x0001u                       /* CRC-32C por instrucción (SSE4.2 / ARMv8) */
#define MFS_HWACCEL_AES 0x0002u /* AES por hardware */
#define MFS_HWACCEL_SHA256                                                     \
  0x0004u /* SHA-256 por hardware                      */
#define MFS_HWACCEL_CLMUL                                                      \
  0x0008u /* multiplicación carry-less (GHASH/CRC)     */
#define MFS_HWACCEL_BLAKE3                                                     \
  0x0010u /* acelerador BLAKE3                         */
#define MFS_HWACCEL_ASCON                                                      \
  0x0020u                        /* acelerador Ascon (§10.6 S2)               */
#define MFS_HWACCEL_DMA 0x0040u  /* DMA + CRC de transporte (§14.1)  */
#define MFS_HWACCEL_RNG 0x0080u  /* TRNG/NRBG por hardware (§15)  */
#define MFS_HWACCEL_SIMD 0x0100u /* vectorización (NEON/AVX/SSE) */
#define MFS_HWACCEL_ATOMICS                                                    \
  0x0200u /* CAS/CMPXCHG atómicos (§13 RT)             */

/* Foto de la arquitectura y sus capacidades, producida por mfs_arch_detect() */
typedef struct {
  uint8_t arch_class; /* mfs_arch_class_t (0..3)     */
  uint8_t bits;       /* 8 | 16 | 32 | 64            */
  uint16_t hwaccel;   /* MFS_HWACCEL_* detectados    */
  uint32_t ram_total;
  uint32_t flash_size;
  uint32_t eeprom_size;
  uint32_t f_cpu_hz;
  const char *name; /* nombre legible (diagnóstico) */
} mfs_arch_info_t;

/* ==== Detección de arquitectura en tiempo de compilación ====
 * Los objetivos de 8 bits exigen opt-in explícito (MFS_ALLOW_8BIT_TARGET=1);
 * el resto (16/32/64 bits) se detectan solos. */
#if defined(__CHAR_BIT__) && (__CHAR_BIT__ != 8)
#error "MFS-ARCH-010: arquitectura con CHAR_BIT != 8 no soportada"
#endif

#if !defined(MFS_ALLOW_8BIT_TARGET)
#if defined(__AVR__) || defined(__CSMC__) || defined(SDCC_mcs51) ||            \
    defined(__SDCC_mcs51) || defined(_PIC14) || defined(_PIC18) ||             \
    defined(__STM8__) || defined(__STM8) || defined(__Z80__) ||                \
    defined(__SMALL_C__) || defined(__C51__) || defined(__ICC8051__) ||        \
    defined(_SDCC_) || defined(__CODE_MODEL_SMALL__) ||                        \
    defined(__CODE_MODEL_COMPACT__) || defined(__CODE_MODEL_LARGE__) ||        \
    defined(__CODE_MODEL_HUGE__)
#define MFS_IS_8BIT_TARGET 1
#else
#define MFS_IS_8BIT_TARGET 0
#endif
#else
#define MFS_IS_8BIT_TARGET 1
#endif

#if MFS_IS_8BIT_TARGET && !defined(MFS_ALLOW_8BIT_TARGET)
#error                                                                         \
    "MFS-ARCH-010: objetivo de 8 bits detectado; define MFS_ALLOW_8BIT_TARGET=1 para habilitar"
#endif

#include <limits.h>
#if (UINT_MAX < 0xFFFFu)
#error "MFS-ARCH-010: se exige int >= 16 bits"
#endif

/* Clase por defecto del objetivo (usada cuando cfg->arch_class ==
 * MFS_ARCH_AUTO) */
#if MFS_IS_8BIT_TARGET
#define MFS_ARCH_CLASS_DEFAULT MFS_ARCH_8BIT
#elif defined(__SIZEOF_POINTER__) && (__SIZEOF_POINTER__ >= 8)
#define MFS_ARCH_CLASS_DEFAULT MFS_ARCH_64BIT
#elif defined(_WIN64) || defined(__LP64__) || defined(__x86_64__) ||           \
    defined(__aarch64__) || defined(__riscv_xlen) && (__riscv_xlen == 64)
#define MFS_ARCH_CLASS_DEFAULT MFS_ARCH_64BIT
#elif (UINT_MAX == 0xFFFFu)
#define MFS_ARCH_CLASS_DEFAULT MFS_ARCH_16BIT
#else
#define MFS_ARCH_CLASS_DEFAULT MFS_ARCH_32BIT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ==== Códigos de estado tipificados (§7.4) ==== */
typedef enum {
  MFS_OK = 0,
  MFS_EINVAL = -1,           /* argumento inválido                      */
  MFS_EIO = -2,              /* error de medio                          */
  MFS_ENOSPC = -3,           /* sin espacio efectivo (reservas intactas)*/
  MFS_EBACKPRESSURE = -4,    /* deuda GC o slack insuficiente (GLD)     */
  MFS_ETIMEDOUT_BUDGET = -5, /* sin presupuesto temporal                */
  MFS_ETABLEFULL = -6,       /* tabla estática saturada                 */
  MFS_EHW_UNSUPPORTED = -7,  /* capacidad no verificada                 */
  MFS_EHEALTH_BLOCKED = -8,  /* bloque en riesgo alto                   */
  MFS_ESECURITY_STATE = -9,  /* clave/integridad/anti-rollback inválido */
  MFS_EENERGY = -10,         /* presupuesto ELD agotado                 */
  MFS_EPUF = -11,            /* PUF no calibrado / extractor en fallo   */
  MFS_ECIPHER = -12,         /* suite no disponible o deprecada         */
  MFS_ESNAPMAX = -13,        /* límite de snapshots del modo            */
  MFS_ENOTVIABLE = -14,      /* presupuesto RAM insuficiente (VIAB-002) */
  MFS_EARCH = -15,           /* arquitectura no soportada (8 bits)      */
  MFS_EBUSY = -16,
  MFS_ECORRUPT = -17, /* corrupción detectada                    */
  MFS_ENOTMOUNTED = -18,
  MFS_ENOENT = -19,    /* path/ino inexistente                    */
  MFS_EEXISTS = -20,   /* ya existe                               */
  MFS_EAGAIN = -21,    /* diferido por SPDR/ELD                   */
  MFS_ENOTSUP = -22,   /* operación no soportada en este modo     */
  MFS_ESTATE = -23,    /* transición de FSM inválida              */
  MFS_EBADMSG = -24,   /* E2G/CRC/MAC inválido (nivel 4-5 §12)    */
  MFS_EOVERFLOW = -25, /* excede límite del modo (§18.2)          */
  MFS_EDEADLK = -26,   /* savepoint/tx inconsistente              */
  MFS_EROFS = -27,     /* volumen montado en sólo lectura         */
  MFS_EACCES = -28     /* permiso denegado (§21.2 setattr/mode)   */
} mfs_st;

const char *mfs_ststr(mfs_st st);

/* ==== Modos operativos (§6.2, §18) ==== */
typedef enum {
  MFS_MODE_ULTRA_NANO = 0, /* 720 B      — 16/32-bit baseline */
  MFS_MODE_NANO = 1,       /* 1.5 KB     — 16/32-bit baseline */
  MFS_MODE_COMPACT = 2,    /* 3.5 KB     — 16/32-bit baseline */
  MFS_MODE_BALANCED = 3,   /* 11.5 KB    — 16/32-bit baseline */
  MFS_MODE_EXTENDED = 4,   /* 21.5 KB    — 16/32-bit baseline */
  /* Modos específicos 8-bit (RAM ≤ 2 KB, determinismo, resiliencia) */
  MFS_MODE_8BIT_ULTRA = 5,   /* ≤ 512 B    — 8-bit ultra-minimal, solo CRC */
  MFS_MODE_8BIT_NANO = 6,    /* ≤ 1 KB     — 8-bit + integridad (Blake3/CRC) */
  MFS_MODE_8BIT_COMPACT = 7, /* ≤ 2 KB     — 8-bit + crypto opcional (Ascon) */
  MFS_MODE_UNSUPPORTED = 0xFF
} mfs_mode_t;

#define MFS_MODE_COUNT 8

/* ==== Familias de modos (§6.2) ==========================================
 * La numeración del enumerado NO es monótona entre familias: los modos 8-bit
 * (5..7) son una familia independiente (RAM ≤ 2 KB), NO "mayores" que Extended.
 * Por eso no se debe comparar el enumerado directamente; usar estos helpers.
 * ======================================================================== */
static inline bool mfs_mode_is_8bit(mfs_mode_t m) {
  return m >= MFS_MODE_8BIT_ULTRA && m <= MFS_MODE_8BIT_COMPACT;
}
static inline bool mfs_mode_is_classic(mfs_mode_t m) {
  return m >= MFS_MODE_ULTRA_NANO && m <= MFS_MODE_EXTENDED;
}
/* "m ∈ familia clásica y m ≥ base" — comparación válida sólo dentro de la
 * familia clásica (Ultra-Nano..Extended). */
static inline bool mfs_mode_classic_ge(mfs_mode_t m, mfs_mode_t base) {
  return mfs_mode_is_classic(m) && mfs_mode_is_classic(base) && m >= base;
}

/* Presupuestos de RAM MatrixFS por modo (§18.2, sumas exactas §23.2) */
#define MFS_RAM_ULTRA_NANO 720u
#define MFS_RAM_NANO 1536u
#define MFS_RAM_COMPACT 3584u
#define MFS_RAM_BALANCED 11520u
#define MFS_RAM_EXTENDED 21504u
/* Modos 8-bit: presupuestos agresivos para ≤ 2 KB RAM total */
#define MFS_RAM_8BIT_ULTRA 512u    /* ≤ 512 B: solo metadatos mínimos + CRC */
#define MFS_RAM_8BIT_NANO 1024u    /* ≤ 1 KB:  + Blake3/CRC32C integridad */
#define MFS_RAM_8BIT_COMPACT 2048u /* ≤ 2 KB:  + Ascon-128a AEAD opcional */

/* Cotas de pila propia por modo (§18.2, verificadas por stack-painting) */
#define MFS_STACK_ULTRA_NANO 64u
#define MFS_STACK_NANO 96u
#define MFS_STACK_COMPACT 256u
#define MFS_STACK_BALANCED 512u
#define MFS_STACK_EXTENDED 1024u
#define MFS_STACK_8BIT_ULTRA 32u /* 8-bit: pila mínima, sin recursión */
#define MFS_STACK_8BIT_NANO 48u
#define MFS_STACK_8BIT_COMPACT 64u

/* Tamaño de chunk por modo (§7.3, §18.1): 128/256/512/4096/4096 + 8-bit */
#define MFS_CHUNK_ULTRA_NANO 128u
#define MFS_CHUNK_NANO 256u
#define MFS_CHUNK_COMPACT 512u
#define MFS_CHUNK_BALANCED 4096u
#define MFS_CHUNK_EXTENDED 4096u
#define MFS_CHUNK_8BIT_ULTRA 64u /* 8-bit: chunks pequeños = menos RAM */
#define MFS_CHUNK_8BIT_NANO 128u
#define MFS_CHUNK_8BIT_COMPACT 256u

/* Ventana de zonas ZLF direccionables en RAM (§8.1): ppage = (zona<<16)|idx.
 * Cota del objetivo embebido; en builds de host la geometría se deriva del
 * tamaño del medio sin superar este número de zonas. */
#define MFS_ZONE_MAX 128u

/* Límites por modo (§18.2). Índices = mfs_mode_t */
typedef struct {
  uint16_t max_open_files;
  uint16_t max_path_len;
  uint16_t max_snapshots;
  uint8_t max_savepoints; /* profundidad */
  uint8_t ring_iocb;
  uint32_t max_file_size_kb; /* tamaño máx. de archivo en KB */
  uint32_t max_volume_kb;    /* volumen máx. en KB (presupuesto BMT fijo) */
  uint16_t ram_total;
  uint16_t own_stack;
  uint16_t chunk_size;
} mfs_limits_t;

extern const mfs_limits_t mfs_limits[MFS_MODE_COUNT];

/* ==== Clases de servicio RT (§13.1) ==== */
typedef enum {
  MFS_RT_A = 0, /* garantizado o rechazo explícito; nunca GC inline */
  MFS_RT_B = 1, /* degradación acotada (<= 1 slice)                 */
  MFS_RT_C = 2  /* mejor esfuerzo; mantenimiento completo           */
} mfs_rt_class_t;

/* Ventanas TCB-DA por clase (§9.3, §25 defaults) */
#define MFS_TAU_RT_A_US 250u
#define MFS_TAU_RT_B_US 500u
#define MFS_TAU_RT_C_US 2000u

/* ==== Tipos de medio (§3.2) ==== */
typedef enum {
  MFS_MEDIA_NONE = 0,
  MFS_MEDIA_NOR_SPI,   /* NOR SPI/QSPI/OSPI       */
  MFS_MEDIA_NAND_RAW,  /* NAND SPI / paralelo raw */
  MFS_MEDIA_NAND_ONFI, /* ONFI / Toggle NAND      */
  MFS_MEDIA_ZNS_NAND,  /* ZNS NAND / ZNS-like     */
  MFS_MEDIA_FRAM,      /* FRAM                    */
  MFS_MEDIA_MRAM,      /* MRAM                    */
  MFS_MEDIA_EEPROM,    /* EEPROM                  */
  MFS_MEDIA_SD,        /* SD 5.1 managed          */
  MFS_MEDIA_EMMC,      /* eMMC 5.1 managed        */
  MFS_MEDIA_USB,       /* USB flash (MSC/UASP)    */
  MFS_MEDIA_NVME,      /* SSD NVMe (PCIe)         */
  MFS_MEDIA_SATA,      /* SSD SATA (AHCI/ATA)     */
  MFS_MEDIA_UFS        /* UFS (JEDEC JESD220)     */
} mfs_media_type_t;

#define MFS_MEDIA_COUNT 14

/* ==== Motores de almacenamiento (MFS-CAP-001) ============================
 * El medio decide el motor: la estrategia se adapta a la naturaleza física de
 * cada tecnología, no al revés.
 *
 *   RAW      —— NVM direccionable por el host (NOR/NAND/FRAM/MRAM/EEPROM): el
 *               núcleo implementa el mapeo (ZLF + E2G + L2P), el desgaste y la
 *               recuperación, porque el medio NO tiene FTL propio.
 *   ZONED    —— NVM con zonas explícitas (ZNS): mapeo 1:1 zona↔zona física con
 *               reset nativo; el host ve y gestiona las zonas, sin GC de medio.
 *   MANAGED  —— Medio con FTL propio (SD/eMMC/USB/NVMe): el dispositivo ya
 *               resuelve el mapeo físico, el desgaste y la reubicación, de modo
 *               que el sistema de archivos asigna por clúster (unidad de
 *               asignación certificada del medio) con su propia tabla y journal
 *               y NO duplica el trabajo del FTL.
 * ======================================================================== */
typedef enum {
  MFS_ENGINE_RAW = 0,
  MFS_ENGINE_ZONED = 1,
  MFS_ENGINE_MANAGED = 2
} mfs_engine_t;

/* Capacidades declaradas por el perfil del medio */
#define MFS_PROF_TRIM 0x0001u       /* discard/trim/deallocate utilizable  */
#define MFS_PROF_PPP 0x0002u        /* partial page program verificable    */
#define MFS_PROF_BYTE_ADDR 0x0004u  /* programable byte a byte             */
#define MFS_PROF_NO_ERASE 0x0008u   /* no requiere borrado previo          */
#define MFS_PROF_SUSPEND 0x0010u    /* suspend/resume de program/erase     */
#define MFS_PROF_MULTIPLANE 0x0020u /* multi-plano / LUN en paralelo       */
#define MFS_PROF_CQE 0x0040u        /* command queuing                     */
#define MFS_PROF_XIP 0x0080u        /* lectura en sitio (NOR)              */
#define MFS_PROF_ECC_ON_DIE 0x0100u /* ECC gestionada por el dispositivo   */
#define MFS_PROF_REMOVABLE 0x0200u  /* medio extraíble (SD/USB)            */

/* ==== Perfil de medio (MFS-CAP-001, §3.2, §5.1, §18.2) ===================
 * Ficha técnica por tecnología: qué unidad de asignación certifica, hasta qué
 * capacidad es válido el medio, qué operaciones nativas expone y con qué
 * presupuestos temporales. El HAL y el asignador configuran el layout a partir
 * de esta tabla, de modo que cada memoria se explota con su granularidad real
 * (AU de SD/eMMC, erase group de eMMC, bloque de NAND, sector de NOR) en vez de
 * con una heurística única.
 * ======================================================================= */
typedef struct {
  const char *name; /* nombre corto y estable (informes/diagnóstico) */
  const char *spec; /* referencia normativa de la que salen los datos */
  mfs_media_type_t type;
  mfs_engine_t engine;
  uint64_t max_bytes;  /* capacidad máxima aceptada (0 = sin cota)      */
  uint32_t alloc_unit; /* unidad de asignación (clúster) en bytes       */
  uint32_t erase_unit; /* bloque borrable mínimo (0 = no borrable)      */
  uint32_t page_size;  /* unidad de programación del medio              */
  uint32_t pgm_gran;   /* mínimo programable en bytes                   */
  uint32_t seek_unit;  /* unidad de direccionamiento del bus            */
  uint16_t chunk;      /* chunk de layout recomendado (0 = por modo)    */
  uint16_t flags;      /* MFS_PROF_*                                    */
  uint32_t t_prog_max_us;
  uint32_t t_erase_max_us;
  uint32_t t_read_max_us;
} mfs_media_profile_t;

/* Perfil del medio `t`; nunca NULL (los no reconocidos devuelven el genérico).
 */
const mfs_media_profile_t *mf_media_profile(mfs_media_type_t t);
const char *mf_engine_name(mfs_engine_t e);
/* MFS-CAP-001: valida que `bytes` quepa en el máximo certificado del perfil. */
mfs_st mfs_profile_check(mfs_media_type_t t, uint64_t bytes);

/* ==== Suites criptográficas (§10.6) ==== */
typedef enum {
  MFS_SUITE_NONE = 0xFF, /* Ultra-Nano sin suites        */
  MFS_SUITE_S0 = 0,      /* AES-256-CTR + HMAC-SHA256    */
  MFS_SUITE_S1 = 1,      /* AES-256-CTR/GCM HW + B3      */
  MFS_SUITE_S2 = 2,      /* Ascon-128a + B3 (SP 800-232) */
  MFS_SUITE_S3 = 3       /* ChaCha20-Poly1305 + B3       */
} mfs_suite_t;

/* ==== Flags del HWV (§5.2) ==== */
/* flags0 */
#define MFS_HWV0_SUSPEND_E 0x01u /* erase-suspend                       */
#define MFS_HWV0_SUSPEND_P 0x02u /* program-suspend                     */
#define MFS_HWV0_MULTI_PLANE 0x04u
#define MFS_HWV0_ECC_ON_DIE 0x08u
/* flags1 */
#define MFS_HWV1_BYTE_ADDR 0x01u /* byte-addressable (FRAM/MRAM/EEPROM) */
#define MFS_HWV1_MANAGED 0x02u   /* SD/eMMC                             */
#define MFS_HWV1_PPP 0x04u       /* partial_page_program (gate WOM-p)   */
#define MFS_HWV1_SLC 0x08u
#define MFS_HWV1_ASYM 0x10u
#define MFS_HWV1_SE 0x20u
/* flags2 (assets de plataforma, fase 8 detección) */
#define MFS_HWV2_PUF 0x01u
#define MFS_HWV2_ADC_RAIL 0x02u
#define MFS_HWV2_SUPERCAP 0x04u
#define MFS_HWV2_TRUSTZONE 0x08u
/* flags3 */
#define MFS_HWV3_CRYPTO_HW 0x01u
#define MFS_HWV3_ASCON_HW 0x02u
#define MFS_HWV3_B3_HW 0x04u
#define MFS_HWV3_CQE 0x08u
#define MFS_HWV3_ZNS 0x10u
#define MFS_HWV3_DMA_CRC 0x20u

/* ==== Manejadores opacos ==== */
typedef struct mfs_fs mf_t;       /* núcleo montado (96 B, §23.1)  */
typedef struct mfs_file mfs_file; /* handle de archivo (24 B, §23.1) */
typedef struct mfs_dir mfs_dir;
typedef uint32_t mfs_snap_id;
typedef struct {
  uint32_t raw[2];
} mfs_sp; /* savepoint  */
typedef struct {
  uint32_t raw[4];
} mfs_fpt; /* FlashPatch */

/* ==== Vectores / stat POSIX-subset ==== */
typedef struct {
  void *iov_base;
  size_t iov_len;
} mfs_iovec;

typedef struct {
  uint32_t ino;
  uint16_t mode; /* MFS_S_IFMT | permisos POSIX (0777) */
  uint16_t nlink;
  uint32_t size;
  uint32_t mtime; /* epoch seconds */
  uint8_t gen;
  uint8_t snapid;
  uint8_t hotness; /* 0=hot .. 3=cold/archive */
  uint8_t pad;
  uint32_t uid; /* propietario (§21.2; persistido on-flash) */
  uint32_t gid; /* grupo                                   */
} mfs_stat;

/* Bits de tipo de fichero y permisos (independientes de <sys/stat.h> para
 * mantener el núcleo portable y sin dependencias de plataforma). */
#define MFS_S_IFMT 0xF000u
#define MFS_S_IFREG 0x8000u /* fichero regular */
#define MFS_S_IFDIR 0x4000u /* directorio      */
#define MFS_DEFAULT_FILE_MODE 0644u
#define MFS_DEFAULT_DIR_MODE 0755u

/* Atributos modificables vía mf_setattr (§21.2). */
typedef struct {
  uint16_t mode; /* MFS_S_IFMT | permisos */
  uint32_t uid;
  uint32_t gid;
  uint32_t mtime; /* epoch seconds */
  uint64_t size;  /* truncado (requiere handle abierto) */
} mfs_attr;

#define MFS_ATTR_MODE 0x01u
#define MFS_ATTR_UID 0x02u
#define MFS_ATTR_GID 0x04u
#define MFS_ATTR_MTIME 0x08u
#define MFS_ATTR_SIZE 0x10u

/* Flags de apertura (subset POSIX) */
#define MFS_O_RDONLY 0x0001u
#define MFS_O_WRONLY 0x0002u
#define MFS_O_RDWR 0x0003u
#define MFS_O_CREAT 0x0010u
#define MFS_O_EXCL 0x0020u
#define MFS_O_TRUNC 0x0040u
#define MFS_O_APPEND 0x0080u
#define MFS_O_RAW 0x0100u /* sin caché; E2G obligatorio (§21.2)      */
#define MFS_O_DIRECT 0x0200u
#define MFS_O_HOTNESS_SHIFT 12
#define MFS_O_HOT(h) ((uint32_t)(h) << MFS_O_HOTNESS_SHIFT)

#define MFS_SEEK_SET 0
#define MFS_SEEK_CUR 1
#define MFS_SEEK_END 2

/* ==== ioctl (§16.2) ==== */
typedef enum {
  MFS_IOCTL_HEALTH = 1,
  MFS_IOCTL_STATS,
  MFS_IOCTL_SET_MODE_HINT,
  MFS_IOCTL_FREEZE_DAB,
  MFS_IOCTL_EXPORT_HWV,
  MFS_IOCTL_VERIFY_BEGIN,
  MFS_IOCTL_EDP_INJECT, /* hook de test (vFlash) */
  MFS_IOCTL_GET_JPEROP,
  MFS_IOCTL_SNAPSHOT_LIST
} mfs_ioctl_cmd;

/* Niveles de verificación (fsck read-only, §21.1) */
typedef enum {
  MFS_VERIFY_QUICK = 0, /* superblock + WAL + raíces */
  MFS_VERIFY_META = 1,  /* + inodos/extents/E2G      */
  MFS_VERIFY_FULL = 2   /* + Merkle global + ZRP     */
} mfs_verify_level;

/* Estado del rail para EDP (§12.3) */
typedef struct {
  uint16_t mv;             /* milivoltios del rail              */
  uint16_t slope_mv_ms;    /* pendiente estimada (mag. caída)   */
  uint32_t t_remaining_us; /* tiempo restante hasta Vmin        */
  bool ok;                 /* rail dentro de margen             */
} mfs_rail_state;

static inline bool mfs_is_error(mfs_st st) { return st < 0; }

#ifdef __cplusplus
}
#endif
#endif /* MATRIXFS_MFS_TYPES_H */
