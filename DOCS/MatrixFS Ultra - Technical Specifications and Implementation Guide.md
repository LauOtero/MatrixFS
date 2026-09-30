# MatrixFS Ultra — Especificación Técnica y Guía de Implementación

## Edición  1.0 «ATLAS»

| | |
|---|---|
| **Documento** | MFS-SPEC-003 |
| **Versión** | 1.0 |
| **Clasificación** | Ingeniería de producto · Almacenamiento embebido crítico |
| **Convenciones** | **DEBE / DEBERÍA / PUEDE** según RFC 2119 |
| **Normativa** | MISRA C:2012 · IEC 61508 SIL-2 · ISO 26262 · NIST SP 800-193 · NIST SP 800-232 · FIPS 205 · NIST SP 800-208 · IEC 62443 · ISO/SAE 21434 |
| **Arquitecturas mínimas** | **16 y 32 bits. Las arquitecturas de 8 bits NO están soportadas (MFS-ARCH-010)** |

---

## Índice

**Parte I — Fundamentos**
1. Resumen ejecutivo
2. Principios rectores
3. Alcance, arquitecturas soportadas y medios

**Parte II — Arquitectura y recursos**
4. Arquitectura general
5. HAL SafeProfile y HWV persistido
6. Viabilidad por MCU y selección de modo
7. Contrato de Recursos Estáticos (RSC) y overlay pools

**Parte III — Núcleo funcional**
8. Formato físico en flash
9. Transaccionalidad WAL+ v3
10. Pipeline CCD v2 y suites criptográficas
11. FTL Ultra 2

**Parte IV — Resiliencia, tiempo y energía**
12. Recuperación de celdas y datos · EDP
13. DAIO v2 · ELD
14. Aceleración por hardware y reglas del bus

**Parte V — Seguridad**
15. Seguridad industrial, cripto-agilidad y post-cuántico

**Parte VI — Observabilidad y garantías**
16. HCT v2
17. KPIs y banco MFS-Bench v2
18. Modos operativos y límites normativos
19. Riesgos y mitigaciones

**Parte VII — Guía de implementación (base para el código fuente)**
20. Modelo de programación y contrato de puerto
21. API pública completa
22. Estructuras on-flash (nivel de byte)
23. Estructuras en RAM y overlays por modo
24. Máquinas de estado
25. Tabla de constantes normativas
26. `mfstool` y proceso de build
27. Plan de verificación y CI

**Parte VIII — Cierre**
28. Roadmap
29. Conclusiones
30. Glosario
31. Alcance integrado en esta edición

---

# PARTE I — FUNDAMENTOS

## 1. Resumen ejecutivo

MatrixFS Ultra es un sistema de archivos embebido determinista que opera directamente sobre memoria no volátil controlable por el MCU: **NOR SPI/QSPI/OSPI, NAND raw, ONFI/Toggle, NAND con semántica de zonas (ZNS-like), FRAM, MRAM, EEPROM y SD/eMMC 5.1**, incluidas **combinaciones heterogéneas de dos medios** (NVM byte-addressable + flash de bloques) bajo un único árbol de directorios.

Está dirigido a dispositivos industriales, IoT crítico, automoción, equipamiento médico y registro seguro, donde fiabilidad, vida útil del medio, energía, seguridad y determinismo pesan tanto como el rendimiento.

**Doce pilares funcionales:**

| # | Pilar | Propósito |
|---|---|---|
| 1 | Autoconfiguración segura | Detección completa, HWV persistido en flash, perfiles firmados |
| 2 | Memoria estática certificada | Sin heap dinámico, peor caso probado en build |
| 3 | **Viabilidad explícita** | Análisis de presupuesto de RAM por MCU; sin soporte ⇒ sin arranque |
| 4 | Transaccionalidad ACID | Commit atómico, recuperación determinista, drenaje de emergencia |
| 5 | Determinismo contractual | Clases RT-A/B/C con presupuestos derivados del datasheet |
| 6 | Durabilidad auditable | Wear leveling, GC y migración sobre salud real + térmica |
| 7 | Cripto-agilidad post-cuántica | Suites S0–S3, ancla SLH-DSA, cero secretos en reposo |
| 8 | Observabilidad accionable | Telemetría firmada (CBOR/COSE), energía por operación |
| 9 | Snapshots y parches atómicos | MFS-Snap O(1), OTA con revert instantáneo (FPT) |
| 10 | Eficiencia integral | CDC, cuckoo, ODT, FSST, XIP directo, SRB in-place |
| 11 | Energía de primera clase | Presupuesto ELD para todo mantenimiento; métrica J/Op |
| 12 | Adaptatividad determinista | Auto-ajuste como autómata sembrado y verificado (DAB/CUSUM) |

**Objetivos consolidados** (todos auditables por MFS-Bench v2, §17):

| Métrica | Objetivo |
|---|---|
| RAM del sistema por modo | **720 B / 1,5 KB / 3,5 KB / 11,5 KB / 21,5 KB** |
| WAF workload mixto (W4) | **≤ 1,25** |
| p99.9 commit RT-A (NOR / con T0) | **≤ 2,5 ms / ≤ 0,5 ms** |
| Montaje @512 MB | **≤ 12 ms** (cota fija, independiente de suciedad) |
| Corrupción bajo corte de energía | **0** (10⁵ cortes + 10⁴ drenajes EDP) |
| Snapshot crear/revertir | **< 1 ms** |
| Vida útil (log compresible 3:1) | **6–9× baseline** |
| Carga CPU en idle | **≤ 0,4 %** |

## 2. Principios rectores

| # | Principio | Descripción |
|---|---|---|
| P1 | **Autoconfiguración segura** | Detección automática, HWV persistido, perfiles firmados; conservador ante capacidades no verificadas |
| P2 | **Sin heap dinámico** | Todo estado crítico en pools estáticos dimensionados en build |
| P3 | **Fallos deterministas** | Errores de capacidad, tiempo, salud, energía o arquitectura tipificados explícitos; sin degradación silenciosa |
| P4 | **Viabilidad honesta** | El presupuesto de RAM se calcula contra la memoria real del MCU (firmware + stack + periféricos + margen). Si no cabe, el modo se descarta y el MCU se declara no soportado |
| P5 | **Presupuestos de datasheet** | Los tiempos se derivan de `T_max` del fabricante, nunca de promedios |
| P6 | **Durabilidad por evidencia** | Wear leveling gobernado por salud efectiva y temperatura |
| P7 | **Seguridad por diseño** | Cifrado, integridad, anti-rollback y cero secretos en reposo |
| P8 | **Observabilidad accionable** | Modelo vivo exportable, firmado criptográficamente |
| P9 | **Reconstrucción total** | No existe camino de apagado ordenado obligatorio |
| P10 | **Corrección formal** | Protocolo transaccional y autómatas adaptativos verificados antes de release |
| P11 | **Cripto-agilidad** | Suites negociables con ruta de deprecación documentada |
| P12 | **Adaptatividad certificable** | Toda política adaptativa es un autómata acotado, sembrado y con replay determinista |
| P13 | **Energía como recurso** | Todo mantenimiento sujeto a presupuesto ELD; J/Op obligatorio |
| P14 | **Sinergia estructural** | Cada subsistema reutiliza primitivas existentes (FPT sobre Snap; CDC sobre dedup; HMT sobre HAL) |
| P15 | **No interferencia con el bus** | El bus de datos se consulta en solo lectura para presupuestos; nunca se reconfigura en runtime ni consume RAM persistente |
| P16 | **Espectro mínimo realista** | Solo arquitecturas de 16/32 bits; las de 8 bits se rechazan en build y runtime |

## 3. Alcance, arquitecturas soportadas y medios

### 3.1 Requisitos de arquitectura (normativos)

**MFS-ARCH-010 (exclusión de 8 bits):** El sistema NO DEBE compilar ni ejecutarse en arquitecturas de 8 bits (AVR/ATmega/ATtiny, 8051 y derivados, PIC10/12/16/18, STM8, y cualquier núcleo con palabra ≤ 8 bits). El puerto DEBE declarar `arch_class` en el HWV y el HAL DEBE rechazar la inicialización con `MFS_EARCH` en caso contrario.

**Justificación:** tamaño de palabra para contadores y CRC/BLAKE3 eficientes, disponibilidad de compiladores certificados, atomización fiable de 16/32 bits y coste de verificación (las garantías p99.9 no son demostrables en núcleos de 8 bits con interruptores de 8 ciclos).

**Requisitos mínimos del objetivo:**

| Requisito | Mínimo |
|---|---|
| Palabra de CPU | 16 bits (MSP430, PIC24) o 32 bits (Cortex-M, RISC-V RV32, Xtensa) |
| Modelo de datos C | `int` ≥ 16 bits; se exige uso exclusivo de `<stdint.h>` de ancho fijo |
| Compilador | C11 (o C99 + stdint), MISRA C:2012 obligatorio |
| RAM mínima viable | 8 KB (con firmware ≤ 3 KB) |
| Flash mínima de código | 32 KB |
| Primitivas de puerto | Secciones críticas, contador de ciclos, `memcpy/memmove/memcmp`, WFI |

**MFS-PORT-001:** Toda aritmética multi-byte sobre formato en flash DEBE usar los accesores de serialización del puerto (§20.4); el acceso no alineado directo está prohibido por defecto (obligatorio en MSP430/PIC24).

### 3.2 Medios soportados

| Medio | Nivel | Especialización |
|---|---|---|
| **NOR SPI/QSPI/OSPI** | Completo | WOM-p, erase-suspend, extents XIP-aware, XDAM |
| **NAND SPI / paralelo raw** | Completo | ZLF, SLEC, RAS, EBA, OOB íntegro |
| **ONFI / Toggle NAND** | Completo | Multi-plano, suspend/resume, read-retry |
| **ZNS NAND / ZNS-like** | Completo | ZLF-Z: mapeo 1:1 zona↔zona, reset nativo |
| **FRAM / MRAM** | Completo | Fast-path de token sin erase, escritura byte-directa |
| **EEPROM** | Completo/parcial | Según granularidad |
| **SD / eMMC 5.1** | Managed | AU, TRIM/ERASE, RPMB, CQE |

### 3.3 Fuera de alcance

SSD NVMe/SATA/USB (FTL opaco), almacenamiento en red, medios sin geometría documentable, arquitecturas de 8 bits.

**MFS-SCOPE-001:** Toda afirmación de rendimiento o vida útil citable DEBE estar ligada a un escenario del banco MFS-Bench v2 (§17).

---

# PARTE II — ARQUITECTURA Y RECURSOS

## 4. Arquitectura general

```
┌────────────────────────────────────────────────────────────────────────┐
│ L8  API POSIX-subset · DAIO v2 (multi-cola RT-A/B/C) · VIO/O_RAW       │
│     · SDP · MFS-Snap · FPT · Savepoints                                │
├────────────────────────────────────────────────────────────────────────┤
│ L7  WAL+ v3 · TCB-DA · Token · GLD · BMT · hooks EDP · RCU             │
├────────────────────────────────────────────────────────────────────────┤
│ L6  CCD v2 · CDC(Gear) → CFX(cuckoo) → LZ4/ODT/FSST → B3/S0–S3         │
├────────────────────────────────────────────────────────────────────────┤
│ L5  ART (extents+namespace, path-copying) · L2P hash · CoW · snaps     │
├────────────────────────────────────────────────────────────────────────┤
│ L4  FTL Ultra 2 · HAWL+·ELM·PEP · WEP · ZLF/ZLF-Z · WOM-p · AGCB+      │
│     · RAS·TG · EBA · SLEC · HMT · ZRP · Regulation Pools               │
├────────────────────────────────────────────────────────────────────────┤
│ L3  HAL SafeProfile · HWV persistido · PUF · ADC rail · viabilidad     │
├────────────────────────────────────────────────────────────────────────┤
│ L2  Drivers · SPI/QSPI/OSPI(XIP) · ONFI/Toggle · SDIO/eMMC-CQE         │
│     · FRAM/MRAM (T0)                                                   │
├────────────────────────────────────────────────────────────────────────┤
│ L1  Primitivas · WOB · ECC · E2G · monitores EDP · puerto (port layer) │
└────────────────────────────────────────────────────────────────────────┘
  Transversal: RSC · FormalCore · vFlash · HCT v2 · Policy Engine
  (DAB+CUSUM) · ELD · Crypto-Agility · Security Monitor
```

**MFS-ARCH-001:** Cada capa DEBE ser sustituible tras su interfaz; los perfiles de medio NO DEBEN requerir cambios en L5–L8.
**MFS-ARCH-002 (HMT):** Con medios T0+T1 declarados, WAL/tokens/metadata-hot/refcounts/HCT residen en T0 y los datos en T1, sin cambiar interfaces L5–L8.
**MFS-ARCH-003 (medio único byte-addressable):** Con FRAM/MRAM/EEPROM como medio único, aplica el fast-path de token sin erase (§9.2) sin HMT.

### 4.1 Capas transversales

| Capa | Responsabilidad |
|---|---|
| **Resource Governor** | Cuotas estáticas, admission control, backpressure, errores tipificados |
| **Policy Engine** | DAB (auto-ajuste) + CUSUM (anomalías SPC); toda decisión se registra en HCT |
| **Security Monitor** | Claves, integridad, anti-rollback, suites, manipulación |
| **ELD** | Presupuesto energético de mantenimiento, J/Op |
| **HCT v2** | Salud, energía, políticas; export firmado |
| **WCET Scheduler** | Presupuestos temporales, token buckets, clases RT, SPDR |
| **Crypto-Agility** | Negociación S0–S3, deprecación, hibridación PQ |
| **RSC** | Certificado de Recursos Estáticos en build |
| **FormalCore** | TLA+/Promela: protocolo transaccional y autómatas DAB/CUSUM/EDP |
| **vFlash** | Simulación multi-medio, inyección de fallos (incl. corte de rail con supercap) |

## 5. HAL SafeProfile y HWV persistido

### 5.1 Cascada de detección

| Fase | Método | Información |
|---|---|---|
| 1 | JEDEC ID (`0x9F`) | Fabricante, familia, densidad |
| 2 | SFDP (`0x5A`, JESD216) | Parámetros NOR/QSPI/OSPI |
| 3 | CFI (`0x98`) | Geometría y tiempos |
| 4 | ONFI / Toggle | NAND: página, bloque, planos, ECC on-die |
| 5 | EXT_CSD / Inquiry | SD/eMMC: AU, reliable write, RPMB, discard, CQE |
| 6 | Detección ZNS-like | Semántica y tamaño de zona |
| 7 | Medio T0 secundario | FRAM/MRAM/EEPROM: densidad, t_write, endurance |
| 8 | Assets de plataforma | PUF, ADC de rail, supercap, TrustZone |
| 9 | Prueba activa limitada | Bloques defectuosos, anomalías |
| 10 | Validación de perfil firmado | Confirmación contra base declarativa (200+ familias, arch ≥ 16 bits) |

### 5.2 HWV — Hardware Capability Vector persistido

**MFS-HWV-001:** El resultado completo de la detección (caps de medios, perfil de MCU, velocidad de bus, clase de arquitectura, assets, CRC) DEBE persistirse como **HWV en flash** en el primer arranque y verificarse en arranques posteriores. En RAM solo reside el handle de 8 bytes `{hwv_flash_addr, hwv_crc}`. Ahorro: −180 B frente a mantener el vector completo en RAM.

```c
typedef struct {          /* mfs_hwv_t — serializado en flash, LE */
    uint8_t  magic[4];        /* "MHWV" */
    uint16_t hw_version;
    uint8_t  arch_class;      /* 1=16-bit, 2=32-bit; 0 ⇒ rechazo */
    uint8_t  mode_forced;     /* modo fijado por manifiesto o 0xFF=auto */
    uint32_t ram_total;
    uint32_t bus_speed_hz;    /* min(MCU, flash) — solo lectura */
    uint32_t t_prog_max_us, t_erase_max_us, t_read_max_us, t_suspend_max_us;
    uint32_t program_granularity, erase_unit;
    uint16_t oob_bytes;
    uint8_t  flags0;          /* suspend_e, suspend_p, multi_plane, ecc_on_die */
    uint8_t  flags1;          /* byte_addr, managed, ppp, slc, asym, se */
    uint8_t  flags2;          /* puf, adc_rail, supercap, trustzone */
    uint8_t  flags3;          /* crypto_hw, ascon_hw, b3_hw, cqe, zns, dma_crc */
    uint32_t t0_size, t0_write_ns;
    uint32_t profile_id;      /* perfil firmado */
    uint32_t crc;             /* CRC-32C del cuerpo */
} mfs_hwv_t;                  /* 64 B */
```

**MFS-HAL-001:** Los presupuestos temporales DEBEN derivarse exclusivamente de los campos `t_*_max_us` y de `bus_speed_hz`.
**MFS-HAL-002:** Ante medio no identificado: perfil genérico conservador (granularidad 1 byte, sin suspend, sin multi-plano, sin ECC on-die, sin HMT/EDP) y estado "medio desconocido" exportado por HCT.
**MFS-HAL-003:** EDP requiere `adc_rail && supercap`; PUF requiere `puf_enrolled`.

### 5.3 Modo conservador

Si falta una capacidad crítica: WOM-p off, dedup/CDC off, compresión solo LZ4, sin HMT/EDP, DAB congelado a defaults, PUF→HKDF(UID+sal) con evento HCT. El dispositivo se marca `best-effort`, no `hard-real-time`.

## 6. Viabilidad por MCU y selección de modo

### 6.1 Criterio de viabilidad

**MFS-VIAB-001:** Un modo es viable en un MCU si y solo si:

```
RAM_total ≥ RAM_firmware_min + RAM_stack_min + RAM_periféricos + RAM_MatrixFS + Margen
```

| Componente | Default | Descripción |
|---|---:|---|
| `RAM_firmware_min` | 50 % | Memoria que el firmware del usuario necesita |
| `RAM_stack_min` | 15 % | Pila mínima del sistema |
| `RAM_periféricos` | 8 % | Buffers DMA, UART, ADC, timers |
| `Margen` | 10 % | Picos y holgura (configurable, mínimo 10 %) |

Los porcentajes son constantes de build sobreescribibles por manifiesto; la suma usada por el selector queda registrada en el certificado RSC.

### 6.2 Algoritmo de selección

```c
static mfs_mode_t select_mode(const mfs_hwv_t *hwv) {
    uint32_t avail = hwv->ram_total
                   - hwv->ram_total * 50 / 100   /* firmware  */
                   - hwv->ram_total * 15 / 100   /* stack     */
                   - hwv->ram_total *  8 / 100   /* periféricos */
                   - hwv->ram_total * 10 / 100;  /* margen    */

    if (avail >= 21504) return MFS_MODE_EXTENDED;    /* 21,5 KB */
    if (avail >= 11520) return MFS_MODE_BALANCED;    /* 11,5 KB */
    if (avail >=  3584) return MFS_MODE_COMPACT;     /*  3,5 KB */
    if (avail >=  1536) return MFS_MODE_NANO;        /*  1,5 KB */
    if (avail >=   720) return MFS_MODE_ULTRA_NANO;  /*  720 B  */
    return MFS_MODE_UNSUPPORTED;
}
```

**MFS-VIAB-002:** Si el resultado es `MFS_MODE_UNSUPPORTED`, el sistema NO DEBE arrancar: `mf_init` DEBE retornar `MFS_ENOTVIABLE`, registrar el evento y ceder el control al firmware. Queda prohibida la degradación silenciosa o el consumo no declarado.

### 6.3 Matriz de viabilidad (sin arquitecturas de 8 bits)

| MCU | Arq. | RAM | Disponible (17 %) | Modo seleccionado |
|---|---|---:|---:|---|
| MSP430FR5994 | 16-bit | 8 KB | 1,36 KB | **Ultra-Nano** |
| PIC24FJ256GA | 16-bit | 16 KB | 2,72 KB | **Nano** |
| STM32F030 | 32-bit M0 | 8 KB | 1,36 KB | **Ultra-Nano** |
| STM32F103 | 32-bit M3 | 20 KB | 3,4 KB | **Compact** |
| GD32VF103 (RISC-V) | 32-bit | 32 KB | 5,4 KB | **Compact** |
| STM32F411 | 32-bit M4 | 128 KB | 21,8 KB | **Extended** |
| STM32F407 | 32-bit M4 | 192 KB | 32,6 KB | **Extended** |
| nRF52840 | 32-bit M4F | 256 KB | 43,5 KB | **Extended** |
| RP2040 (dual M0+) | 32-bit | 264 KB | 44,9 KB | **Extended** |
| ESP32-S3 | 32-bit LX7 | 512 KB | 87 KB | **Extended** |
| i.MX RT1176 | 32-bit M7 | 2 MB TCM | 340 KB | **Extended** |

**Rechazo por arquitectura (MFS_EARCH):** ATmega328P/2560, ATtiny, 8051, PIC10/12/16/18, STM8 y toda familia de 8 bits, con independencia de su RAM.

## 7. Contrato de Recursos Estáticos (RSC) y overlay pools

**MFS-RES-001:** El sistema NO DEBE contener asignación dinámica de memoria. Todo fallo de capacidad DEBE ser un error tipificado explícito.

### 7.1 Herramienta `mfstool plan`

**Entrada:** manifiesto (archivos abiertos, savepoints, snapshots, ventana WAL, zonas, medios T0/T1, porcentajes de viabilidad) + HWV + timings del driver.
**Salida:**
1. `matrixfs_resources.h` con `static_assert(sizeof(pool) <= RAM_BUDGET)`.
2. **Certificado de Recursos Estáticos** firmado: peor caso de RAM, overhead de flash, cota de montaje, WCET por clase DAIO, WCET de autómatas DAB/CUSUM/EDP, suite cripto, perfil de fallback, cota de pila propia (verificada por stack-painting).

**MFS-RES-002:** La ausencia de ruta al heap DEBE verificarse por análisis del mapa de enlazado (`mfstool check-map`).

### 7.2 Overlay pools (memoria compartida temporal)

Pools con uso temporalmente disjunto comparten buffer físico mediante unions:

| Grupo | Pools que comparten | Invariante de exclusión |
|---|---|---|
| **OVERLAY-A** | WAL, GC | WAL no programa mientras GC ejecuta slices |
| **OVERLAY-B** | Crypto, Compresión, Dedup(CFX) | Secuencial en el pipeline CCD |
| **OVERLAY-C** | Metadatos, Caché de extents | Fases de montaje/commit disjuntas |
| **OVERLAY-D** | HCT ring, Savepoints, ELD/DAB | Alternancia por evento |

**MFS-RES-003:** Los overlays DEBEN estar protegidos por aserciones de fase en build (RSC) y el certificado DEBE demostrar la disjunción temporal de cada grupo. Ahorro: **−45 %** de RAM estática.

```c
typedef union { uint8_t wal[2560]; uint8_t gc[2560]; }      mfs_ovl_a_t; /* Balanced */
typedef union { uint8_t crypto[256]; uint8_t comp[768];
                uint8_t cfx[1024]; }                        mfs_ovl_b_t;
typedef union { uint8_t meta[512];  uint8_t ext[768]; }     mfs_ovl_c_t;
typedef union { uint8_t hct[512];   uint8_t sp_eld[640]; }  mfs_ovl_d_t;
```

### 7.3 Técnicas de reducción aplicadas (normativas)

| Técnica | Regla | Ahorro |
|---|---|---:|
| **Chunk adaptativo** | Tamaño de chunk por modo: 128/256/512/4096/4096 B (UN→Ext) | Buffer de pipeline lineal al chunk |
| **Metadatos flash-first** | Ventanas mínimas en RAM: bitmap 64 B, inodos 128 B (8 activos), extents 64 B; resto bajo demanda con prefetch secuencial | −62 % metadatos cacheados |
| **HCT sin RAM (UN/Nano)** | Contadores agregados a flash en sync/unmount; sin ring en UN | −1,5 KB en Nano |
| **WEP sin caché en modos bajos** | Recálculo on-the-fly (~1,2 µs/op); caché 16 entradas solo Balanced+ | 32 B |
| **Savepoints bajo demanda** | Bitmask + offsets; reserva solo si la app los usa | hasta −1,5 KB |
| **HWV en flash** | Handle de 8 B en RAM | −180 B |
| **Struct packing** | `packed` + reordenación mayor→menor en structs de control | −412 B |
| **SRB in-place** | Pipeline CCD sobre buffer rotativo único (ratio ≥ 1,1×) | −4 KB |
| **Stack acotado** | Cota de pila propia por modo, verificada por stack-painting | 64–1024 B |

### 7.4 Errores tipificados

| Código | Significado |
|---|---|
| `MFS_OK` / `MFS_EINVAL` / `MFS_EIO` | Éxito / argumento inválido / error de medio |
| `MFS_ENOSPC` | Sin espacio efectivo (reservas intactas) |
| `MFS_EBACKPRESSURE` | Deuda GC o slack insuficiente |
| `MFS_ETIMEDOUT_BUDGET` | Sin presupuesto temporal |
| `MFS_ETABLEFULL` | Tabla estática saturada |
| `MFS_EHW_UNSUPPORTED` | Capacidad no verificada |
| `MFS_EHEALTH_BLOCKED` | Bloque en riesgo alto |
| `MFS_ESECURITY_STATE` | Clave, integridad o anti-rollback inválido |
| `MFS_EENERGY` | Presupuesto ELD agotado para mantenimiento |
| `MFS_EPUF` | PUF no calibrado o extractor en fallo |
| `MFS_ECIPHER` | Suite no disponible o deprecada |
| `MFS_ESNAPMAX` | Límite de snapshots del modo alcanzado |
| `MFS_ENOTVIABLE` | Presupuesto de RAM insuficiente en este MCU |
| `MFS_EARCH` | **Arquitectura no soportada (8 bits o puerto incompleto)** |
| `MFS_EBUSY` / `MFS_ECORRUPT` / `MFS_ENOTMOUNTED` | Ocupado / corrupción detectada / no montado |

---

# PARTE III — NÚCLEO FUNCIONAL

## 8. Formato físico en flash

### 8.1 Jerarquía y layout del volumen

```
Medio → [Superblock A | Superblock B | HWV | Zona de descriptores] → Zonas (ZLF) → Bloques → Páginas → Registros
```

- **Superblock A/B (256 B cada uno):** raíces, época, secuencia, suite, descriptores (§22.1).
- **HWV (64 B):** vector de capacidades persistido (§5.2).
- **Zona:** 1–4 bloques (NOR: grupo de sectores). Escritura estrictamente secuencial.

### 8.2 Cabecera de registro (E2G) — nivel de byte

| Campo | NOR (20 B) | NAND (32 B) |
|---|---:|---:|
| Magic + versión | 2 B | 2 B |
| Época | 1 B | 2 B |
| Secuencia monótona por zona | 4 B | 8 B |
| LBA lógico (E2G) | 4 B | 4 B |
| Generación (4b) + snapshot-id (4b) | 1 B | 2 B |
| Meta: tipo(4b)·wom-gen(2b)·hotness(2b)·dict-id(4b)·flags(4b) | 2 B | 2 B |
| Longitud payload | 2 B | 2 B |
| CRC-32C (cabecera+payload) | 4 B | 4 B |
| Reservado | — | 6 B |

**MFS-FMT-001:** Toda lectura DEBE validar `{LBA, generación, CRC-32C}` contra el mapeo esperado. Discrepancia ⇒ recuperación nivel 4–5 (§12). Elimina por construcción el fallo "mapeo obsoleto devuelve página antigua".
**MFS-FMT-002:** El campo snapshot-id multiplexa la generación de snapshot sin bytes adicionales.

### 8.3 ECC y checksums

| Medio | Esquema |
|---|---|
| NOR sin OOB | CRC-32C + Hamming SECDED por bloque lógico |
| NAND OOB escaso | Hamming SECDED (1 bit/256 B) |
| NAND OOB medio | BCH t=4/t=8 parametrizado |
| NAND ECC on-die | LDPC interno (nunca software) |
| FRAM/MRAM | CRC-32C por registro |

Selección dinámica por región vía **EBA** (§11.8). Integridad criptográfica y fingerprints: **BLAKE3-256** (§10.5); cada archivo exporta su raíz Merkle B3; el superblock lleva la raíz global firmada.

### 8.4 Contadores Termométricos (TFC)

Contador P/E por bloque codificado como bits programados secuencialmente (1 bit por intervalo de 16 ciclos); la página del contador nunca se reescribe; carry anticipado con 2 bits de aviso; 4 bits de tendencia BER. Coste: 1 página/bloque (~0,2 %). En T0, contadores por escritura byte-directa sin coste.

### 8.5 ZRP — Zone Rescue Parity (opcional)

**MFS-ZRP-001:** En zonas `archive/cold`, al cerrar la zona el sistema PUEDE computar paridad **RS(16,1) sobre GF(2⁸)** por grupo de 16 páginas, almacenada en la cabecera de zona. Una página irrecuperable se reconstruye re-leyendo el grupo, sin acudir a WAL/réplica. Sobrecoste +6,25 % solo en zonas ZRP (nunca hot/RT); off por defecto.

## 9. Transaccionalidad WAL+ v3

### 9.1 Crash-Only Design

No existe camino de apagado ordenado obligatorio. Montaje = {replay WAL acotado + validación E2G + reconstrucción perezosa + superblock candidates ampliados con raíces de snapshot}. Todo corte en cualquier punto deja el sistema consistente en la época anterior o posterior a cada token.

### 9.2 Token de commit

- Una única operación de programación por token: `{txid, época, CRC, MAC}` en slot alternante A/B con contador termométrico; regla "una programación en vuelo" con barreras WOB.
- Formato T1 (32 B): magic 2 · txid 4 · época 4 · seq 8 · crc32c 4 · MAC truncado 8 · pad 2. Formato T0 (16 B): magic 2 · txid 4 · época 2 · seq 4 · crc 2 · MAC 2 (escritura atómica por palabra).
- **MAC en streaming** durante la programación del payload (−72 % SW; −96 % con acelerador).
- En Ultra-Nano (sin suites activas): token = `{txid, época, seq, CRC-32C}` + alternancia A/B; la detección de token rasgado usa CRC + comparación de secuencia monótona.
- **MFS-HMT-001 (fast-path):** con T0, token y WAL viven en T0 como escritura byte-directa: sin erase, sin hazard; la época anti-rollback se espeja en T1 mediante página WOM-p monotónica. Lo mismo aplica a medio único FRAM/MRAM (MFS-ARCH-003).

### 9.3 TCB-DA — group commit por deadline

Ventanas por clase: `τ_RT-A ≤ 250 µs`, `τ_RT-B ≤ 500 µs`, `τ_RT-C ≤ 2 ms` (ajustables por DAB dentro de límites duros del perfil). RT-A nunca espera a RT-B/C. **Commit folding:** transacciones idénticas en ventana se colapsan con refcount.

**MFS-WAL-001:** p99.9 commit RT-A ≤ 2,5 ms (NOR) / ≤ 0,5 ms (T0) / ≤ 2 ms (NAND).

### 9.4 GLD — GC-Debt Ledger

```
deuda += 1  por página de datos
deuda += 2  por página de metadatos
pago idle: 1 slice ≤ 500 µs → ≤ 4 bloques escaneados + ≤ 1 reubicación
```

**MFS-GLD-001:** RT-A DEBE rechazarse con `MFS_EBACKPRESSURE` si `deuda > D_max` (default 256) antes de iniciarse.

### 9.5 BMT — Bounded Mount Time

Ventana WAL W = 32–256 páginas; checkpoint incremental cada W/2 (delta + rollup cada 16) con árbol BLAKE3 incremental; checkpoint asíncrono con watermark 0,75×W; **Panic-Safe Truncation** solo tras readback verificado. Presupuesto de montaje por fases (QSPI 512 MB, ≤ 12 ms): superblock 0,3 · replay WAL ≤ 3 · checkpoint ≤ 4 · raíces ART ≤ 2 · misc ≤ 2,7.

### 9.6 Savepoints y RCU

Savepoints anidados hasta profundidad 4 (bitmask + posiciones, rollback O(1), reserva bajo demanda). Metadatos de lectura intensiva se publican por intercambio de puntero con reclamation por épocas (RCU): lectores nunca se bloquean; reclamation en slices RT-C.

## 10. Pipeline CCD v2 y suites criptográficas

### 10.1 Orden normativo (SRB)

```
stream → CDC(Gear) → CFX(dedup, texto claro) → comprimir (LZ4/ODT/FSST, in-place)
       → cifrar (suite activa) → BLAKE3 Merkle + E2G/ECC → hint → programar
```

**SRB:** fases in-place sobre buffer rotativo único (tamaño = chunk del modo) cuando ratio ≥ 1,1×.

### 10.2 CDC — Content-Defined Chunking

**MFS-CDC-001:** En modos con dedup (Balanced+), el chunking DEBE ser por contenido: Gear hash con máscara normalizada de dos niveles, min 1 KB / avg 4 KB / max 16 KB (escalado proporcional en Extended para streams grandes). Coste objetivo ≤ 2 ciclos/byte (Cortex-M4); solo en RT-C e ingesta masiva. Beneficio (W8): +15–30 % de ratio de dedup frente a chunking fijo.

### 10.3 CFX — filtro cuckoo

Fingerprints de 8 bits, 2 buckets × 2 slots, **borrable** (requerido por refcounts), reemplazo determinista sembrado; ≤ 1 KB en Balanced, ≤ 2 KB en Extended; índice persistente en flash. **MFS-CCD-001:** prohibido deduplicar sobre texto cifrado; con cifrado activo requiere `MFS_OPT_CONVERGENT=1` documentado; off por defecto en UN/Nano/Compact.

### 10.4 ODT / FSST / TS-Delta

- **ODT:** diccionarios Zstd de 1–4 KB por clase de contenido, entrenados offline (`mfstool train-dict`) u on-device lazy (RT-C, coste acotado); selector `dict-id` en cabecera; diccionario corrupto ⇒ fallback LZ4 + evento HCT.
- **FSST:** compresión de strings/logs con acceso aleatorio (Extended).
- **TS-Delta:** preset de codificación delta-of-delta + Gorilla para series temporales, seleccionable por hint.

### 10.5 BLAKE3

**MFS-B3-001:** En Balanced+, integridad criptográfica de datos, fingerprints de dedup y árbol Merkle global DEBEN usar **BLAKE3-256** (keyed con clave derivada de época; modo árbol para raíces). En Nano/Compact: superblock y metadatos. En UN: solo superblock. Estado < 200 B. HMAC-SHA256/AES-GCM se conservan como rutas legacy/HW.

### 10.6 Suites criptográficas (crypto-agility)

| Suite | Datos | Metadatos | Uso |
|---|---|---|---|
| **S0** (legacy) | AES-256-CTR + HMAC-SHA256 | AES-GCM/GMAC | Perfiles con HW AES |
| **S1** (default Bal/Ext) | AES-256-CTR/GCM HW + B3 | AES-GCM + B3 | MCU con acelerador |
| **S2** (ligera) | **Ascon-128a** (NIST SP 800-232) + B3 | Ascon AEAD | Nano/Compact sin HW AES |
| **S3** (fallback SW) | **ChaCha20-Poly1305** + B3 | Poly1305 | MCU sin HW AES en modos grandes |

**MFS-CAG-001:** La suite se negocia en montaje según `{HW, perfil, deprecación}` y se registra en HCT y superblock. Nonces únicos por clave (`época ‖ seq`) en todas las suites.

### 10.7 Hint de escritura y borrado seguro

Hint opt-in solo RT-C (prohibido en RT-A/B y high-security; telemetría `write_savings_ratio`; claim hasta +68 % auditable). Clasificación de borrado seguro en 4 clases (`hot/cold × secure/unsecure`) con políticas WOM-p/cifrado/réplica/sobrescritura.

### 10.8 FPT — FlashPatch (OTA delta atómico)

**MFS-FPT-001:** (1) snapshot MFS-Snap del slot activo (O(1)); (2) parche por delta (diff + LZ4) escrito como **overlay de extents** en streaming con buffers 2×chunk; (3) activación = swap de raíz ART + época tras validar hash B3; (4) **revert = re-fijar la raíz anterior**, instantáneo y sin copia.

## 11. FTL Ultra 2

### 11.1 HAWL+ con ELM y PEP

Fases de vida: temprana (σ=15 %) / media (σ=20 %) / tardía (σ=10 %) / crítica (forzado).

```
salud(b) = 0.5·(1 − PE_b/PE_max) + 0.3·(1 − BER_b/BER_fallo) + 0.2·(1 − tendencia_b)
RUL(b) ≈ (BER_fallo − BER_b) / pendiente_EMA(BER_b)      [α = 1/32]
Disparo proactivo: RUL(b) < 0.2 × RUL_medio
```

**PEP (Extended):** 4 perceptrones cuantizados 8-bit, 32 características, voto por mayoría, **WCET < 5 µs**, ROM < 1 KB; solo refina victim selection en RT-C; **ELM es siempre el baseline determinista** y prevalece ante conflicto (evento registrado).

### 11.2 WEP

Red de Feistel 16 bits (4 rondas, `F(x) = ((x·K1) ^ (x>>3))·K2`), claves derivadas del UID; caché 16 entradas solo Balanced+ (recálculo directo en UN/Nano/Compact); coste RAM cero en modos bajos.

### 11.3 Regulation Pools

Regiones dedicadas (WAL, metadata hot, small files, logs, telemetría) con rotación por desgaste relativo (`1,3 × desgaste medio`); objetivo ≥ 30 % de reducción verificable.

### 11.4 WOM-p

Generaciones/borrado: 2 (3 solo NAND multinivel); sobrecoste 25–35 %; reclaim proactivo al agotar gen 2; gate por `partial_page_program` verificado. No aplica en T0.

### 11.5 Paralelismo NAND y CQE

Multi-plane program/read/erase + entrelazado (efecto en latencia 2–4×). Con **CQE eMMC 5.1**, los iocbs DAIO se mapean a tareas de cola HW (hasta 16); RT-A mantiene reliable-write directo.

### 11.6 ZLF / ZLF-Z

Zona = 1–4 bloques, escritura estrictamente secuencial, estados `EMPTY → OPEN → FULL → RECLAIM`, reclaim determinista (`válidos < 30 %` en RT-C). **ZLF-Z:** mapeo 1:1 con zonas del medio ZNS-like usando reset nativo y respetando el write-pointer. **MFS-FTL-001:** sin GC intercalado dentro de zona ⇒ latencia acotable por construcción y menor WAF.

### 11.7 SLEC

Ventana SLC-dinámica en NAND MLC/TLC para WAL y metadatos calientes cuando no existe T0; plegado a TLC en idle (≤ 1 bloque/ventana, RT-C).

### 11.8 EBA

ECC adaptativa por región; regla `BER_región < capacidad_ECC / 2`; exceso ⇒ scrub urgente (RAS).

### 11.9 RAS + TG — Thermal Governor

Reubicación si `edad > 0,5 × retención_espec(BER, temp)`; read-disturb por bloque (termométrico); presupuesto ≤ 1 página/slice idle; nunca en RT-A; **sujeto a ELD**.
**MFS-TG-001:** urgencia de scrub, read-retry y refresco DEBEN ajustarse por temperatura según tabla del perfil; en extremos térmicos se aplazan programaciones no-RT y se prohíben refuerzos de tensión de programación.

### 11.10 AGCB+, Capacity Variance y vírgenes

Victim selection por etapas sobre deuda GLD con dirty-list incremental de 32 entradas enriquecida con hotness de CFX; ventana `max(5 ms, 2 × latencia_media_últimos_100_IO)`. Capacity Variance: bloques próximos al límite reducen espacio usable (parametrizado por ELM/TG). Preservación de vírgenes con histéresis si `wear_deficit > 0,2 × σ_objetivo`.

### 11.11 HMT — Heterogeneous Media Tiering

**MFS-HMT-002:** con `T0` (FRAM/MRAM/EEPROM) y `T1` (flash): WAL+tokens+metadata hot+refcounts+raíces ART+HCT en T0; datos en T1. Presupuesto T0 ≥ 32 KB para Balanced (menor ⇒ degradación a mono-medio con evento). ELM/TG/CV operan por-tier; EEPROM finita usa journalling corto in-place.

---

# PARTE IV — RESILIENCIA, TIEMPO Y ENERGÍA

## 12. Recuperación de celdas y datos

### 12.1 Niveles

| Nivel | Técnica | Coste |
|---|---|---|
| 0 | Prevención (TG, read-disturb, suspend) | Continuo |
| 1 | ECC Hamming SECDED / BCH | Inline, µs |
| 2 | LDPC soft-decision (on-die) | HW |
| 3 | Read Retry | ms, RT-C |
| 4 | Copia desde WAL / réplica / snapshot root | Transaccional |
| 5 | Reconstrucción extents + **paridad ZRP** | Montaje/RT-C |
| 6 | Retiro + remapeo + HCT | Permanente |

### 12.2 Invariante de doble-metadatos y RPO/RTO

Sumarios A/B con validación cruzada; réplica de inodos críticos; raíz Merkle BLAKE3 firmada; **raíces de snapshot como superblock candidates**. RPO: 0 pérdidas de transacciones confirmadas; RTO acotado por tamaño WAL; metadatos sub-segundo; detección de corrupción silenciosa continua (E2G + Merkle + ZRP).

### 12.3 EDP — Emergency Drain Protocol

**MFS-EDP-001:** Con `adc_rail && supercap`, el sistema DEBE: (1) muestrear el rail y estimar pendiente; **disparo** cuando el tiempo-restante < 1,5 × t_drain; (2) congelar admisión y drenar por prioridad `{tokens → sumarios → hot}` verificando energía `E_cap = ½·C·(V0² − Vmin²)` contra `E_prog = I_cc·V·t_prog` antes de cada programación; (3) si ventana < 4 ms, drenar solo tokens; (4) abortar en cualquier punto dejando la época anterior consistente; (5) registrar el evento en HCT. Objetivo (W6-E): 0 corrupciones en 10⁴ cortes con ventana ≥ 8 ms. Sin assets: barrera power-loss básica (semántica invariante).

## 13. DAIO v2 y ELD

### 13.1 Clases y multi-cola

| Clase | Contrato | GC/WL inline | Anillo |
|---|---|---|---|
| **RT-A** | Garantizado o rechazo explícito | Nunca | Propio, prof. 4 |
| **RT-B** | Degradación acotada (≤ 1 slice) | Limitado | Propio, prof. 8 |
| **RT-C** | Mejor esfuerzo | Completo | Propio, prof. 16 |

**MFS-DAIO-002:** Cada clase DEBE tener anillo de envío independiente con anillo de completación unificado — sin head-of-line blocking entre clases. API: `mf_submit(iocb{op, deadline_us, clase})`, `mf_readv/writev` (VIO), `O_RAW` (sin caché, E2G obligatorio).
**SPDR:** scheduler estático de prioridades con deuda por rebanadas: difiere trabajo interno (GC slices, scrub, plegado SLEC) a ventanas entre operaciones de usuario. Niveles: 2 en Compact; completo (multi-cola) en Balanced+.

### 13.2 Presupuestos (NOR QSPI)

| Operación | Datasheet | Presupuesto RT-A |
|---|---:|---:|
| Page program (4 KB) | 3 ms | 3,5 ms |
| Read (4 KB) | 200 µs | 0,4 ms |
| Commit (grupo RT-A) | τ_A + prog | 2,5 ms |
| **Erase (4 KB)** | **400 ms** | **Prohibido inline** |

**ESP:** erases con suspend/resume en cuantos de 250 µs con cesión a RT-A; CPU en WFI durante el cuanto (consumo en erase largo: 3 % → 0,4 %).

### 13.3 ELD — Energy Ledger

**MFS-ELD-001:** Todo mantenimiento (GC, RAS/TG, plegado SLEC, entrenamiento ODT, reconstrucción ZRP, exploración DAB extra) DEBE consumir tokens del presupuesto ELD, modelado con corrientes/tiempos del perfil. Hook `energy_state` (batería/cosechadora) fija el presupuesto por ventana; sin presupuesto ⇒ diferido con `MFS_EENERGY` visible en HCT (nunca difiere integridad crítica). Métrica obligatoria: **J/Op por clase**.

### 13.4 Modo RT estricto

Desactiva: GC inline, WOM-p reescritura, ZSTD idle, prefetch, PEP/exploración DAB, dedup/CDC inline, plegado SLEC, hint, sobrescritura segura; DAB congelado a valores del certificado. Garantía: p99.9 RT-A ≤ presupuesto declarado, verificado en CI.

## 14. Aceleración por hardware y reglas del bus

### 14.1 Aceleradores

| Acelerador | Uso | Beneficio |
|---|---|---|
| DMA | Flash ↔ RAM zero-copy | Carga CPU mínima |
| DMA-CRC (linked-list) | CRC en tránsito (SDP) | Captura sin CPU |
| AES-256 / **Ascon HW** | Suites S1/S2 | Sin coste CPU |
| SHA-256 / CRC-32C | Legacy / integridad | Detección rápida |
| **CQE eMMC 5.1** | Cola HW de 16 tareas | −30–50 % sobrecoste CPU |
| Caché / **XIP (XDAM)** | Lecturas / activos RO | Cero RAM de datos |

**MFS-HW-001:** Cada acelerador DEBE tener fallback software con la misma API; el certificado RSC recalcula el WCET del fallback activo. **MFS-HW-002:** el escalado de frecuencia en low-power revalida presupuestos.

**XDAM (MFS-XDAM-001):** activos read-only mapeados XIP con verificación E2G por muestreo auditado (política 1/n en HCT) e invalidación por época obligatoria.
**SDP (MFS-SDP-001):** con `dma_scatter_gather && dma_crc`, captura sensor → DMA → `POOL_SDP` con CRC en tránsito; IRQ de completación dispara `mf_submit(O_RAW)` al WAL. Objetivo: < 5 % CPU a 1 MSps.

### 14.2 Reglas del bus (no interferencia)

**MFS-BUS-001 (solo lectura):** La detección del bus y su velocidad se realiza exclusivamente mediante consultas read-only (registros de clock del MCU, SFDP, EXT_CSD) y una medición temporal de un ping de prueba. NUNCA se modifican parámetros del bus, temporizaciones ni estados de la flash que afecten al usuario.

**MFS-BUS-002 (negociación acotada al init):** La negociación de ancho/modo (Single/Quad/Octo + DDR) se ejecuta **una única vez dentro de `mf_init`, bajo propiedad del HAL**, con fallback a single-lane si falla la calibración; el resultado se registra en el HWV en flash. Tras retornar `mf_init`, el runtime NO ESCRIBE ninguna configuración del bus.

**MFS-BUS-003 (cero RAM persistente):** La detección no consume RAM persistente (0 B): registro de clock y SFDP se leen a buffers temporales (< 48 B en pila) y el resultado se persiste únicamente como campo read-only del HWV.

**MFS-BUS-004 (uso restringido):** La velocidad detectada se usa exclusivamente para: presupuestos WCET (`t_read_max_us`, `t_prog_max_us`…), tamaño de chunk óptimo, decisión de prefetch (bus > 50 MHz) y timeouts de polling.

Ganancia de negociación init-time: QSPI 5,8 → 41,2 MB/s @ 50 MHz; OSPI DDR 21 → 76,4 MB/s @ 200 MHz. Prefetch secuencial (2 lecturas < 100 µs): +12 % W1. Burst polling (`Read Status Enhanced`): −18 % comandos.

---

# PARTE V — SEGURIDAD

## 15. Seguridad industrial, cripto-agilidad y post-cuántico

| Requisito | Contenido |
|---|---|
| **MFS-SEC-001** | Nonces únicos por clave (`época ‖ seq`) en todas las suites |
| **MFS-SEC-002** | Ceroización en `mf_deinit()`; **cero secretos en reposo** |
| **MFS-SEC-003** | Anti-rollback: época en RPMB, página WOM-p monotónica (NOR) o espejo T0 (HMT) |
| **MFS-SEC-004** | Secure boot: hash de superblock (BLAKE3) en la cadena de confianza |
| **MFS-SEC-005** | Encrypt-then-MAC/AEAD obligatorio; verificación antes de entregar datos |
| **MFS-PUF-001** | Con PUF SRAM: claves derivadas por boot (fuzzy extractor, helper data públicas); fallback HKDF(UID+sal) solo conservador con evento |
| **MFS-PQ-001** | Cadena híbrida: ancla **SLH-DSA** (FIPS 205) verificada una vez en secure-boot; perfiles/épocas firmados con **LMS** (SP 800-208) o Ed25519; la ancla PQ habilita rotación post-cuántica total |
| **MFS-CAG-001** | Suites S0–S3 negociables con deprecación documentada |
| **Export** | Telemetría **CBOR + COSE_Sign1** firmada por dispositivo (compacta, offline) |

**Aislamiento:** con TrustZone/MPU IoT, pools cripto + slots de clave + metadatos raíz en dominio seguro vía secure gateway.

**Alineación:** IEC 62443 · NIST SP 800-193 · ISO/SAE 21434 · ISO 26262 · IEC 61508 SIL-2 · NIST SP 800-232 · FIPS 205 · SP 800-208.

---

# PARTE VI — OBSERVABILIDAD Y GARANTÍAS

## 16. HCT v2

### 16.1 Métricas

| Grupo | Métricas |
|---|---|
| Salud física | Histograma P/E, tendencia BER, read-count, temperatura, deratings TG |
| Eficiencia | WAF por workload, ratio CDC, stats ODT, `write_savings_ratio` |
| Rendimiento | Latencia p50/p99/p99.9 por clase, free-space efectivo, deuda GLD |
| Energía | J/Op, presupuesto ELD, eventos de diferido |
| Política | Log de decisiones DAB, alarmas CUSUM, estado PEP |
| Ciclo de vida | Tasa de retiro, snapshots activos, eventos FPT |
| Seguridad | Errores cripto/integridad, power-loss/EDP, suite activa |

### 16.2 API y dimensionado

```c
int mf_ioctl(mf_t *fs, MFS_IOCTL_HEALTH, mfs_health_t *out);
int mf_export_health(mf_t *fs, void *buf, size_t len);   /* CBOR + COSE_Sign1 */
```

| Modo | Ring HCT | Estado DAB/CUSUM/ELD | Persistencia |
|---|---:|---:|---|
| Ultra-Nano | 0 B | 0 B | Agregados a flash en sync/unmount |
| Nano | 128 B (agregados) | 0 B | Agregados a flash en sync/unmount |
| Compact | 512 B | ≤ 128 B | Checkpoint con metadatos |
| Balanced | 1 KB | ≤ 256 B | Checkpoint con metadatos |
| Extended | 2 KB | ≤ 256 B | Checkpoint con metadatos |

**Digital Twin:** predicción de vida por bloque/dispositivo (ELM+PEP) y consumo energético proyectado; recomendación de migraciones; anomalías CUSUM; validación de garantías; mantenimiento predictivo de flota con bundles firmados.

## 17. KPIs y banco MFS-Bench v2

### 17.1 Workloads normativos

| ID | Workload | Métrica |
|---|---|---|
| W1 | Append-log 4 KB | MB/s |
| W2 | Metadata storm | IOPS |
| W3 | Random write 4 KB | IOPS + WAF |
| W4 | Mixto hot/cold 90/10 | IOPS + WAF + vida |
| W5 | Streaming OTA | MB/s |
| W6 | Tortura corte de energía (vFlash) | Corrupción=0, t_recuperación |
| W6-E | Corte con supercap + EDP (10⁴) | Corrupción=0, t_drain |
| W7 | Hint opt-in | `write_savings_ratio` + vida |
| W8 | Dedup stream heterogéneo | Ratio CDC vs fijo |
| W9 | Energía: data logger 24 h | J/Op, autonomía, diferidos |
| W10 | Snapshot/parche/FPT | Tiempos, atomicidad |
| W11 | HMT dual-medio | p99.9 commit, WAF WAL |

Percentiles p50/p99/p99.9 obligatorios. Baselines externas: littlefs 2.x, UBIFS, YAFFS2, FatFS.

### 17.2 Objetivos normativos

| Métrica | Objetivo |
|---|---:|
| W1 (NOR QSPI) | ≥ 28 MB/s (cota de bus 41,2 MB/s) |
| W2 / W3 | ≥ 6.000 / ≥ 4.500 IOPS |
| WAF (W4) | ≤ 1,25 |
| W8 dedup | +15–30 % |
| W10 snapshot | < 1 ms crear, < 1 ms revertir |
| W11 commit RT-A | ≤ 0,5 ms |
| Montaje @512 MB | ≤ 12 ms (cota fija) |
| p99.9 RT-A commit (NOR) | ≤ 2,5 ms |
| p99.9 RT-A read | ≤ 0,4 ms |
| CPU idle | ≤ 0,4 % |
| Corrupción W6/W6-E | 0 |

### 17.3 Modelo de vida descompuesto

```
Vida_relativa ≈ (WAF_baseline / WAF_MatrixFS) × R_CCD × F_WOM × F_CV × F_hint
WAF_MatrixFS  = WAF_gc (≤ 1,32) × WAF_meta (≤ 1,18) × WAF_wl (≤ 1,05)
```

| Escenario | Factor objetivo |
|---|---:|
| Log compresible 3:1 | 6–9× |
| Mixto compresible 2:1 | 3,5–5× |
| Incompresible, disperso | 2–2,8× |
| Metadatos intensivos | 3–4,5× |
| Hint opt-in | hasta +68 % adicional |

**Cada factor es medible por HCT: la afirmación se audita, no se asume.**

## 18. Modos operativos y límites normativos

### 18.1 Matriz de características

| Característica | Ultra-Nano | Nano | Compact | Balanced | Extended | RT Strict |
|---|---|---|---|---|---|---|
| Chunk | 128 B | 256 B | 512 B | 4 KB | 4 KB | según perfil |
| Suites | — | S2 opt. | S1/S2 | S1/S3 | S0–S3 | S1 HW |
| BLAKE3 | superblock | SB+meta | SB+meta | completo | completo | meta |
| CDC | ❌ | ❌ | ❌ | ✅ | ✅ | ❌ |
| Dedup (CFX) | ❌ | ❌ | ❌ | ✅ | ✅ | ❌ |
| ODT / FSST | ❌ | ❌ | ❌ | ODT×1 | ODT×4+FSST | ❌ |
| Snapshots | 0 | 0 | 2 | 8 | 64 | perfil |
| FPT (OTA) | ❌ | ❌ | ✅ | ✅ | ✅ | perfil |
| DAB / CUSUM | ❌ | ❌ | opt. | ✅ | ✅ | congelado |
| ELD | ❌ | ❌ | ✅ | ✅ | ✅ | ✅ |
| HMT (T0) | ❌ | opt. | opt. | recom. | recom. | perfil |
| EDP | solo tokens | ✅ | ✅ | ✅ | ✅ | ✅ |
| SDP / XDAM | ❌ | ❌ | opt. | ✅ | ✅ | perfil |
| ZRP | ❌ | ❌ | opt. | ✅ | ✅ | perfil |
| PEP | ❌ | ❌ | ❌ | ❌ | ✅ | ❌ |
| SPDR | ❌ | ❌ | 2 niveles | completo | completo | reservas |
| HCT ring | 0 B | 128 B | 512 B | 1 KB | 2 KB | perfil |

### 18.2 Límites por modo

| Límite | Ultra-Nano | Nano | Compact | Balanced | Extended |
|---|---:|---:|---:|---:|---:|
| Archivos abiertos | 2 | 4 | 8 | 16 | 32 |
| Longitud de path | 64 B | 128 B | 256 B | 256 B | 256 B |
| Snapshots | 0 | 0 | 2 | 8 | 64 |
| Savepoints (prof.) | 1 | 2 | 4 | 4 | 4 |
| Ring iocb | 2 | 4 | 8 | 8 | 16 |
| Tamaño máx. archivo | 256 KB | 1 MB | 4 MB | 1 GB | 4 GB |
| Volumen (presupuesto BMT fijo) | ≤ 4 MB | ≤ 16 MB | ≤ 64 MB | ≤ 512 MB | ≤ 512 MB (re-param. > 512 MB) |
| **RAM MatrixFS total** | **720 B** | **1 536 B** | **3 584 B** | **11 520 B** | **21 504 B** |
| **Pila propia (WCET, pintada)** | **64 B** | **96 B** | **256 B** | **512 B** | **1 024 B** |

## 19. Riesgos y mitigaciones

| Riesgo | Mitigación |
|---|---|
| Coste CPU de CDC en MCU modesto | Gate por modo/RT-C, skip-mask, min-chunk 1 KB |
| PUF con deriva térmica | Fuzzy extractor con margen + calibración + fallback documentado |
| Verificación SLH-DSA lenta | Ancla solo en secure-boot; perfiles con LMS/Ed25519 |
| HMT duplica matriz de fallos | vFlash con fault-injection cruzada T0×T1; reglas HMT en FormalCore |
| DAB opaco | P12: autómata acotado, sembrado, replay, guardas duras, freeze-on-violation |
| CUSUM con falsos positivos | Umbrales k=0,5/h=5 + histéresis + registro |
| ZRP penaliza WAF | Off por defecto; solo clases frías; medible |
| EDP falso positivo | Histéresis + margen 1,5× + modelo ½CV² calibrado |
| Diccionario ODT pobre | Fingerprint de validación + fallback LZ4 + métrica HCT |
| Compresión+cifrado filtra longitud | Padding o desactivar compresión en high-sec |
| XDAM sirve datos obsoletos | Invalidación por época + auditoría por muestreo |
| Overlay activado en fase equivocada | Aserciones de fase en build (certificado RSC) |
| MCU justo de RAM mal estimado | MFS-VIAB-002: rechazo explícito, jamás degradación silenciosa |
| Metadata root corrupta | Mirror + Merkle B3 + raíces de snapshot como candidatos |
| Sobrepromesa de garantías | Todo objetivo ligado a W1–W11; lenguaje acotado |

---

# PARTE VII — GUÍA DE IMPLEMENTACIÓN

## 20. Modelo de programación y contrato de puerto

### 20.1 Reglas de codificación

1. **C11** (o C99 + `<stdint.h>`); **MISRA C:2012** obligatorio; sin `malloc/free`; sin recursión salvo where provada acotada (recovery: profundidad `4 + log2(W)`).
2. Solo tipos de ancho fijo (`uint8_t/16/32/64_t`); `char` solo para strings de path.
3. Sin acceso no alineado: usar accesores del puerto (obligatorio en MSP430/PIC24).
4. Formato en flash **little-endian** normativo; serialización solo por accesores.
5. Toda ISR-safety según tabla §21.3; secciones críticas vía puerto, nunca `__disable_irq` directo fuera del puerto.
6. Constantes mágicas y tablas solo desde §25; prohibidos números mágicos.
7. Cada error de la §7.4 es terminal en su capa: propagar sin envolver.

### 20.2 Contrato del puerto (`mfs_port.h`)

```c
/* Obligatorio implementar por integrador: */
void     mfs_port_crit_enter(void);
void     mfs_port_crit_exit(void);
uint32_t mfs_port_cycles(void);            /* contador de ciclos libre-corriente */
uint32_t mfs_port_time_us(void);
void     mfs_port_wfi(void);
/* Driver L2 (registrado en mfs_config): */
mfs_st   mfs_port_flash_read (uint32_t addr, void *dst, uint32_t len);
mfs_st   mfs_port_flash_prog (uint32_t addr, const void *src, uint32_t len);
mfs_st   mfs_port_flash_erase(uint32_t addr);
mfs_st   mfs_port_t0_read/prog (...);        /* solo si HMT */
bool     mfs_port_rail_ok(mfs_rail_state *); /* solo si EDP */
/* Opcionales: dma_read/dma_crc, crypto_hw, wfi_hooks */
```

### 20.3 Requisitos del puerto

- Secciones críticas de latencia acotada (≤ 1 µs de entrada).
- `mfs_port_cycles` con resolución ≤ 1 µs para WCET y EDP.
- Flash driver con barreras WOB: `prog` retorna solo tras verificación de estado; `erase` soporta suspend/resume si `flags0.suspend_e`.

### 20.4 Accesores de serialización

```c
static inline uint16_t mfs_ld16(const uint8_t *p){ return (uint16_t)(p[0] | p[1]<<8); }
static inline uint32_t mfs_ld32(const uint8_t *p){ return (uint32_t)p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }
static inline uint64_t mfs_ld64(const uint8_t *p); /* ídem, 8 bytes */
void mfs_st16/32/64(uint8_t *p, uint16_t/32_t/64_t v);
```

## 21. API pública completa

### 21.1 Prototipos

```c
/* Ciclo de vida */
int  mf_init(mf_t *fs, const mfs_config *cfg);      /* cfg: driver L2, overlays opcionales,
                                                       modo forzado, presupuesto viabilidad */
int  mf_format(mf_t *fs, const mfs_format_opts *o);
int  mf_sync(mf_t *fs);
int  mf_deinit(mf_t *fs);                            /* ceroización + sync final */

/* POSIX-subset */
int  mf_open(mf_t*, const char *path, uint32_t flags, mfs_file **f);
int  mf_close(mfs_file *f);
int  mf_read (mfs_file *f, void *buf, size_t len, size_t *rd);
int  mf_write(mfs_file *f, const void *buf, size_t len, size_t *wr);
int  mf_seek (mfs_file *f, int64_t off, int whence); int mf_tell(mfs_file*, uint64_t*);
int  mf_stat (mf_t*, const char *path, mfs_stat *st);
int  mf_unlink(mf_t*, const char *path);  int mf_rename(mf_t*, const char*, const char*);
int  mf_mkdir(mf_t*, const char *path);   int mf_truncate(mfs_file*, uint64_t);
int  mf_opendir/readdir/closedir(...);

/* VIO y DAIO */
int  mf_readv / mf_writev(mfs_file*, const mfs_iovec *v, int n);
int  mf_submit(mf_t *fs, mfs_iocb *cb);              /* ISR-safe */
int  mf_poll  (mf_t *fs, mfs_iocb **done, int max, uint32_t timeout_us);

/* Transacciones */
int  mf_tx_begin(mf_t*);  int mf_tx_commit(mf_t*);  int mf_tx_abort(mf_t*);
int  mf_sp_create(mf_t*, mfs_sp *sp);  int mf_sp_rollback(mfs_sp);  int mf_sp_release(mfs_sp);

/* Snapshots y OTA */
int  mf_snap_create (mf_t*, mfs_snap_id *id);
int  mf_snap_delete (mf_t*, mfs_snap_id id);
int  mf_snap_revert (mf_t*, mfs_snap_id id);
int  mf_fpt_begin   (mf_t*, mfs_fpt *h);
int  mf_fpt_apply   (mfs_fpt h, const void *delta, size_t len);
int  mf_fpt_activate(mfs_fpt h);   int mf_fpt_rollback(mfs_fpt h);

/* Salud y verificación */
int  mf_ioctl(mf_t*, uint32_t cmd, void *arg);       /* HEALTH/STATS/... */
int  mf_export_health(mf_t*, void *buf, size_t len);
int  mf_verify(mf_t *fs, mfs_verify_level lvl);       /* fsck read-only */
```

### 21.2 Semántica clave

- `mf_submit` nunca bloquea: acepta o rechaza (`MFS_ETIMEDOUT_BUDGET` / `MFS_EBACKPRESSURE` / `MFS_ETABLEFULL`).
- `mf_write` es transaccional por defecto: visible atómicamente tras commit interno (TCB-DA); `O_RAW` escribe sin caché con E2G obligatorio.
- Los savepoints son de alcance de transacción; los snapshots de alcance de volumen.
- `mf_fpt_apply` acepta el delta en cualquier tamaño de fragmento (streaming); `activate` es un único swap de raíz.

### 21.3 Seguridad de interrupciones

| Función | ISR | Tarea |
|---|---|---|
| `mf_submit`, `mf_poll` | ✅ | ✅ |
| `mf_read` (O_RAW, página cacheada) | ❌ | ✅ |
| Todo lo demás | ❌ | ✅ (protegido por sección crítica corta) |

## 22. Estructuras on-flash (nivel de byte)

### 22.1 Superblock (256 B, slots A/B)

| Off | Campo | Tamaño |
|---:|---|---:|
| 0 | Magic `"MFSU"` + versión | 4 |
| 4 | Época | 4 |
| 8 | Secuencia global | 8 |
| 16 | suite_id (1) · mode_id (1) · flags (2) | 4 |
| 20 | Raíz ART `{zone:2, page:4}` | 6 |
| 26 | Descriptor WAL `{start:4, len:2, W:2}` | 8 |
| 34 | Raíces de snapshot (8 × 6 B) + count | 49 |
| 83 | Fingerprints ODT (4 × 1 B) | 4 |
| 87 | HWV addr (4) + HWV crc (4) | 8 |
| 95 | build_id (8) | 8 |
| 103 | golden_crc (4) | 4 |
| 107 | MAC (B3-keyed/HMAC truncado) | 16 |
| 123 | Reservado | 133 |
| 256 | | **Total** |

Selección A/B: mayor `{época, seq}` con MAC válida; empate ⇒ B (regla de desempate normativa).

### 22.2 Cabecera de zona (64 B útiles)

| Off | Campo | Tamaño |
|---:|---|---:|
| 0 | Magic + versión | 2 |
| 2 | zone_id | 2 |
| 4 | Estado (FSM §24.1) | 1 |
| 5 | Secuencia | 4 |
| 9 | Generación + WOM-gen | 3 |
| 12 | Clase (hotness/uso) + flags ZRP | 2 |
| 14 | Válidos (hint, 16b) | 2 |
| 16 | CRC-32C | 4 |
| 20 | Reservado | 44 |

### 22.3 Inodos

- **INO-S (32 B, UN/Nano):** ino 4 · gen 2 · type/flags 2 · size 4 · nlink 2 · mtime 4 · rootref `{zone:2,page:4}` 6 · extents inline 0 · crc 4 · pad 2.
- **INO-L (64 B, Compact+):** metadatos 16 B + 4 extents directos de 12 B.

### 22.4 Registro de extent (12 B)

`{vpage:4, ppage:4, npages:2, flags:2}` — flags: generación(4b) · snapshot(4b) · compartido(1b) · CoW(1b) · ZRP(1b) · reservados.

### 22.5 Checkpoint

Sumario delta encadenado: `{prev_hash_b3, delta_comprimido, mac}`; rollup cada 16 deltas; truncado solo tras readback verificado del nuevo sumario.

## 23. Estructuras en RAM y overlays por modo

### 23.1 Núcleo común

```c
typedef struct {                 /* mfs_t — 96 B (packed) */
    const mfs_config *cfg;
    mfs_hwv_handle_t  hwv;         /* 8 B */
    uint32_t epoch;  uint64_t seq;
    uint8_t  mode, suite, flags;
    uint16_t debt_gld;             /* GLD */
    uint8_t  edp_state, dab_state;
    void    *ovl_a, *ovl_b, *ovl_c, *ovl_d;   /* unions según modo */
    /* raíces ART vivas, ventana WAL, contadores críticos */
} mfs_t;

typedef struct {                 /* mfs_file — 24 B */
    uint32_t ino;  uint64_t pos;  uint32_t size;
    uint16_t flags;  uint8_t ref;  uint8_t pad;
    uint32_t dirty_hint;  /* extent actual */
} mfs_file;

typedef struct { uint32_t op, deadline_us; uint16_t len;
                 uint16_t class_flags; void *buf; int32_t status; } mfs_iocb; /* 20 B */
```

### 23.2 Presupuestos por modo (sumas exactas)

**Ultra-Nano — 720 B:**

| Bloque | B |
|---|---:|
| OVERLAY-A (WAL 192 \| GC) | 192 |
| OVERLAY-C (meta 64 \| extents 32) | 96 |
| OVERLAY-D (agregados HCT \| SP) | 32 |
| SRB (chunk 128) | 128 |
| Contexto + 2 handles | 96 |
| Pila propia | 64 |
| Margen interno | 112 |
| **Total** | **720** |

**Nano — 1 536 B:** A 384 · C 160 · B 96 (Ascon opt.) · D 128 · SRB 256 · contexto+4 handles 160 · pila 96 · margen 256.

**Compact — 3 584 B:** A 1 024 · B 512 · C 512 · D 256 · SRB 512 · contexto+8 handles 256 · pila 256 · margen 256.

**Balanced — 11 520 B:** A 2 560 · B 1 024 (CFX máx.) · C 768 · D 640 · SRB 4 096 · contexto+16 handles 608 · pila 512 · margen 1 312.

**Extended — 21 504 B:** A 4 096 · B 3 072 (CFX 2 K máx.) · C 2 048 · D 1 536 · SRB×2 8 192 · contexto+32 handles 896 · pila 1 024 · margen 1 640.

## 24. Máquinas de estado

### 24.1 Zona

```
EMPTY → OPEN → FULL → RECLAIMING → EMPTY
   │       │       │        │
   └───────┴───────┴────────┴──→ QUARANTINE (fallo E2G/CRC irrecuperable) → retiro (nivel 6)
```

| Transición | Disparador | Acción atómica |
|---|---|---|
| EMPTY→OPEN | primera escritura | cabecera de zona programada (estado + seq) |
| OPEN→FULL | puntero al final | sellado + ZRP opcional |
| FULL→RECLAIMING | `válidos < 30 %` (RT-C) o deuda GLD | copia válidos a zona OPEN destino |
| RECLAIMING→EMPTY | readback verificado | erase diferido (ESP), nunca RT-A |
| *→QUARANTINE | 2 fallos E2G nivel 4–5 | bloqueo + evento HCT + remapeo |

### 24.2 Montaje (FSM de 8 pasos, ≤ 12 ms @512 MB)

1. Cargar HWV (o detectar + persistir) · 2. Chequeo arquitectura (`MFS_EARCH`) · 3. Viabilidad (§6) · 4. Velocidad de bus read-only (§14.2) · 5. Superblock A/B (época+seq+MAC) · 6. Replay WAL acotado W · 7. Checkpoint/raíces (incl. snapshots como candidatos) · 8. Evento HCT de arranque. Fallo en 5–7 ⇒ fallback al candidato anterior; fallo total ⇒ `MFS_ECORRUPT` + propuesta `mf_format` explícita (nunca automática).

### 24.3 EDP

`MONITOR → ARMED (pendiente de rail < umbral) → DRAIN{1:tokens, 2:sumarios, 3:hot} → DONE`
Guardas: presupuesto por programación; ventana < 4 ms ⇒ solo fase 1; aborto válido en todo punto.

### 24.4 DAB (autómata EXP3 sembrado)

- Parámetros `P = {τ_A, τ_B, τ_C, D_max, cdc_mask, nivel_compresión}`; 3 brazos acotados por parámetro (límites duros de §25).
- Actualización cada N = 256 operaciones; recompensa normalizada de `{throughput, p99.9, WAF}` con pesos fijos del perfil.
- **Freeze-on-violation:** cualquier incumplimiento p99.9 RT-A congela el autómata a valores del certificado RSC.
- Todo cambio se registra en HCT y es **replay-determinista** (misma semilla ⇒ misma secuencia); verificado en FormalCore.

## 25. Tabla de constantes normativas

| Constante | Default | Rango | Uso |
|---|---:|---|---|
| `τ_RT-A / τ_RT-B / τ_RT-C` | 250/500/2000 µs | 50 µs–2 ms | TCB-DA |
| `D_max` (GLD) | 256 | 64–1024 | backpressure |
| `W` (ventana WAL) | 64 | 32–256 | BMT |
| Watermark checkpoint | 0,75×W | fijo | async |
| Umbral reclaim zona | 30 % válidos | 20–50 % | ZLF |
| Redondeo WOM-p | 2 gen/borrado | 2–3 (NAND ML) | WOM |
| Rondas WEP | 4 | 4 | Feistel |
| CDC min/avg/max | 1/4/16 KB | – | Balanced+ |
| CFX | 2×2 slots, fp 8b | 1–2 KB | dedup |
| ZRP | RS(16,1) | k≤4 | zonas frías |
| ELM pesos | 0,5/0,3/0,2 | fijos | salud |
| EMA RUL | α = 1/32 | fijos | RUL |
| CUSUM | k=0,5, h=5 | perfil | anomalías |
| DAB | N=256 ops | 64–1024 | update |
| ESP cuantos | 250 µs | 100–500 µs | erase |
| EDP margen | 1,5× t_drain | 1,2–2× | disparo |
| EDP ventana mínima | 4 ms | fijo | solo tokens |
| TFC intervalo | 16 ciclos/bit | fijo | desgaste |
| AGCB+ dirty-list | 32 entradas | 16–64 | víctimas |
| Máx. tx por commit | 16 (4 en UN/Nano) | 4–32 | TCB |

## 26. `mfstool` y proceso de build

| Comando | Función |
|---|---|
| `mfstool plan` | Genera `matrixfs_resources.h` + certificado RSC firmado (RAM, flash, WCET, autómatas, suite, fallback) |
| `mfstool check-map` | Verifica ausencia de ruta al heap en el mapa de enlazado |
| `mfstool train-dict` | Entrena diccionarios ODT desde corpus del usuario |
| `mfstool sign-profile` | Firma perfiles de dispositivo (ancla PQ opcional) |
| `mfstool bench` | Ejecuta MFS-Bench v2 sobre vFlash o target |
| `mfstool verify-hwv` | Valida/pinna un HWV para producción |

Pipeline obligatorio: `plan → build → check-map → KATs → vFlash (FIH) → bench → firmar certificado`. Un release sin certificado RSC firmado NO DEBE distribuirse.

## 27. Plan de verificación y CI

1. **KATs criptográficos:** CRC-32C (0xE3069283), BLAKE3 oficial, Ascon (SP 800-232), ChaCha20-Poly1305 (RFC 8439), AES-GCM (NIST), HMAC (RFC 4231), SLH-DSA/LMS (NIST vectors).
2. **vFlash — matriz de fallos:** corte de rail con barrido de fase (0–100 % de transacción, 10⁵ cortes), drenajes EDP (10⁴), fallos cruzados T0×T1, bit-flips, stuck-at, corrupción OOB, páginas rasgadas, tokens A/B incompletos.
3. **FormalCore:** TLA+ del protocolo WAL+ (tokens, checkpoint, snapshots), Promela de DAB/CUSUM/EDP (sin contraejemplos antes de release).
4. **Cobertura:** MC/DC ≥ 90 % en módulos críticos (WAL, token, recovery, E2G, GLD).
5. **Stack-painting:** verificar cotas de pila de §18.2 por modo y por target.
6. **MFS-Bench v2:** W1–W11 con p50/p99/p99.9 publicados y ligados a MFS-SCOPE-001.
7. **Rechazo temprano:** test de puerta que inyecta `arch_class=0` y RAM insuficiente ⇒ exige `MFS_EARCH` / `MFS_ENOTVIABLE`.

---

# PARTE VIII — CIERRE

## 28. Roadmap

**Fase 1 — Núcleo y viabilidad:** puerto (`mfs_port.h`), HWV, casacada HAL, análisis de viabilidad y selección de modo, RSC + overlays, pools estáticos, extents + L2P, ART, WAL+ (token, TCB-DA, BMT, checkpoint B3), modos UN/Nano/Compact completos, CRC cache, DMA alineado, reglas MFS-BUS.
*Gate:* cero heap verificado; `MFS_EARCH`/`MFS_ENOTVIABLE` en tests de puerta; UN en target de 8 KB real; FIH 10⁵ cortes sin corrupción; certificado RSC en 3 targets (MSP430FR, STM32F1, STM32F4).

**Fase 2 — Diferenciación:** GLD, WOM-p, TFC, E2G, ZLF, HCT, CCD (LZ4+AES), MAC streaming, AGCB+ dirty-list, CFX+CDC, ODT, multi-cola DAIO + SPDR, MFS-Snap, FPT, ELD.
*Gate:* WAF(W4) ≤ 1,35; montaje ≤ 15 ms; p99.9 RT-A ≤ 3 ms; W8 ≥ +10 %; W10 atómico; Compact viable en 20 KB real.

**Fase 3 — Industrial:** HAWL+·ELM, WEP, CV, SLEC, RAS+TG, EBA, DAIO completo, HW con fallback, borrado seguro, HMT, EDP, ZRP, PUF, cadena PQ, TrustZone, XDAM, prefetch/burst/WFI, FormalCore+vFlash en CI.
*Gate:* TLA+ sin contraejemplos (transaccional y autómatas); MC/DC ≥ 90 %; W6-E con 0 corrupciones; W11 ≤ 0,5 ms; safety/security case.

**Fase 4 — Optimización avanzada:** hint RT-C, PEP + tuning DAB por perfil, SDP, FSST, ZLF-Z, CQE, diccionarios on-device, reportes de durabilidad/energía por familia, bundles COSE de flota.
*Gate:* W7–W11 con mejoras medibles; sin regresión W1–W6; validación externa.

**Fase 5 — Certificación:** dossier SIL-2/21434, deprecación programada de suites, tooling de flota, reportes por familia validados por terceros.

## 29. Conclusiones

MatrixFS Ultra define, en un único documento normativo, un sistema de archivos embebido cuyo rasgo distintivo es la **conversión de cada promesa en un artefacto auditable**:

| Garantía | Artefacto |
|---|---|
| Sin heap dinámico | Certificado RSC firmado + `check-map` |
| Viabilidad honesta | MFS-VIAB + rechazo explícito (`MFS_ENOTVIABLE`, `MFS_EARCH`) |
| Determinismo | Presupuestos desde `T_max` + p99.9 en CI + autómatas model-checked |
| Durabilidad | WAF descompuesto medido por HCT + TG + ELD |
| Recuperación | Verificación formal + 10⁵ cortes + 10⁴ drenajes sin corrupción |
| Seguridad | Crypto-agility, ancla SLH-DSA, PUF, cero secretos en reposo |
| Ciclo de vida | Snapshots O(1), FPT con revert instantáneo, telemetría COSE |
| Energía | J/Op medible; mantenimiento con presupuesto ELD |
| No interferencia | MFS-BUS-001..004: bus read-only, cero RAM persistente |

El espectro soportado es explícito y realista: desde 720 B de RAM en MCUs de 8 KB (16/32 bits) hasta configuraciones Extended con dual-medio, PQ y ML certificable — siempre con el mismo contrato de recursos estáticos, cota fija de montaje y verificación formal.

## 30. Glosario

| Sigla | Significado |
|---|---|
| AGCB+ | GC adaptativo sobre deuda GLD (dirty-list) |
| ART | Adaptive Radix Tree con path-copying |
| B3 | BLAKE3 (hash con modo árbol y keyed) |
| BMT / GLD / ELM / EBA | Cota de montaje / deuda GC / modelo de vida / presupuesto de error |
| CDC / CFX | Chunking por contenido / filtro cuckoo |
| CQE / XDAM / SDP | Cola HW eMMC / modo XIP directo / ruta DMA sensor→WAL |
| DAB / CUSUM / PEP / SPDR | Bandit determinista / control SPC / ensamble de perceptrones / scheduler estático de prioridades por rebanadas |
| DAIO / TCB-DA / ESP | I/O con deadline / commit por deadline / erase-suspend |
| E2G / ECC / EDP / ELD | Guarda extremo-a-extremo / corrección de errores / drenaje de emergencia / presupuesto energético |
| FPT / HCT / HMT | Parche OTA atómico / telemetría de salud / tiering heterogéneo |
| HWV | Hardware Capability Vector persistido en flash |
| KAT | Known Answer Test |
| MFS-Snap / ODT | Snapshot O(1) / diccionarios de compresión on-device |
| PUF / PQ / RCU | Physical Unclonable Function / post-cuántico / read-copy-update |
| RSC / RT-A/B/C | Certificado de recursos estáticos / clases de servicio |
| SLH-DSA / LMS | Firmas hash-based (FIPS 205 / SP 800-208) |
| SRB / TFC / TG | Buffer rotativo único / contadores termométricos / gobernador térmico |
| WAF / WCET / WEP / WOM-p | Amplificación de escritura / peor caso temporal / placement por entropía / reescritura program-only |
| ZLF / ZLF-Z / ZRP / ZNS | Log por zonas / variante ZNS / paridad de rescate / zoned namespace |

## 31. Alcance integrado en esta edición

Esta edición consolida en un solo documento normativo la totalidad del sistema:

1. **Viabilidad y honestidad de recursos** — criterio MFS-VIAB con presupuesto real del MCU (firmware + stack + periféricos + margen), matriz de viabilidad, rechazo explícito sin arranque, y cotas de pila propia verificadas por stack-painting.
2. **Modos ultra-compactos** — Ultra-Nano (720 B) y Nano (1,5 KB) para MCUs de 8 KB, y Compact (3,5 KB) para 16–20 KB, con ladder completo hasta Balanced (11,5 KB) y Extended (21,5 KB).
3. **Técnicas de reducción de RAM** — overlay pools con invariantes de fase certificadas, chunk adaptativo (128 B–4 KB), metadatos flash-first con ventanas mínimas, HCT agregado sin RAM en modos bajos, savepoints bajo demanda, HWV en flash con handle de 8 B, packing selectivo y SRB in-place.
4. **Reglas del bus** — detección read-only para presupuestos, negociación acotada al init y registrada en HWV, cero RAM persistente y cero reconfiguración en runtime.
5. **Exclusión de 8 bits** — regla MFS-ARCH-010 con rechazo en build y runtime (`MFS_EARCH`); espectro mínimo: 16/32 bits con 8 KB de RAM.
6. **Núcleo completo** — WAL+ v3 (token, TCB-DA, GLD, BMT, savepoints, RCU), CCD v2 (CDC, cuckoo, ODT/FSST/TS-Delta, BLAKE3, suites S0–S3 con Ascon y ChaCha20), FTL Ultra 2 (HAWL+·ELM·PEP, WEP, ZLF/ZLF-Z, WOM-p, SLEC, EBA, RAS+TG, AGCB+, HMT, ZRP), E2G a nivel de byte, snapshots O(1) y FPT.
7. **Tiempo, energía y resiliencia** — DAIO multi-cola con SPDR, ESP, ELD con J/Op, EDP con modelo ½CV², niveles de recuperación 0–6, anti-rollback, PUF, cadena PQ (SLH-DSA/LMS), export CBOR/COSE.
8. **Base de implementación** — contrato de puerto, API pública con semántica e ISR-safety, layouts on-flash a nivel de byte, estructuras RAM con sumas exactas por modo, máquinas de estado, tabla de constantes, `mfstool`, y plan de verificación (KATs, vFlash, FormalCore, MC/DC, stack-painting).

---