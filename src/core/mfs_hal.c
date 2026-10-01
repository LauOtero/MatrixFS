/* mfs_hal.c — cascada de detección HAL (§5.1), selección de modo y viabilidad
 * (§6.1 MFS-VIA-001/002), medición de bus read-only (§14.2 MFS-BUS-001..004)
 * y perfilado HMT (§12.4). */
#include "mfs_internal.h"
#include <string.h>

static const mfs_hal_ops *g_hal;
void mfs_set_hal_ops(const mfs_hal_ops *ops) { g_hal = ops; }

mfs_hwv_t g_last_hwv;
mfs_mode_t g_last_selected = MFS_MODE_UNSUPPORTED;
mf_t *g_mfs_instance = NULL;

/* ==== Medición de velocidad de bus (MFS-BUS-001: SOLO lectura) ==== */
uint32_t mfs_bus_measure_hz(const mfs_l2_driver *drv, void *ctx) {
  /* lee un bloque de prueba desde addr 0 y cronometra con cycles() */
  uint8_t buf[64];
  uint32_t c0, c1;
  /* primera pasada (calienta caché si existe) */
  if (drv->read(ctx, 0u, buf, sizeof(buf)) != MFS_OK)
    return 0u;
  c0 = mfs_port_cycles();
  for (int i = 0; i < 8; i++) {
    if (drv->read(ctx, 0u, buf, sizeof(buf)) != MFS_OK)
      return 0u;
  }
  c1 = mfs_port_cycles();
  uint32_t cyc = (uint32_t)(c1 - c0);
  if (cyc == 0u)
    return 0u;
  /* bytes/ciclo * frecuencia desconocida ⇒ devolvemos tasa relativa en
   * "bytes por ciclo*1e6"; el integrador convierte con F_CPU conocida.
   * Convención del estándar: la medida se expresa como Hz efectivos asumiendo
   * que cada byte cuesta 1 acceso de bus; se documenta en DOCS/hal.md. */
  uint64_t bytes = 64ull * 8ull;
  uint64_t hz_est = (bytes * 1000000ull) / ((uint64_t)cyc * 8ull);
  return (uint32_t)hz_est;
}

/* ==== Cascada de detección (§5.1) ==== */
static void geom_defaults(mfs_media_geom *g) {
  memset(g, 0, sizeof(*g));
  g->type = MFS_MEDIA_NOR_SPI;
  g->erase_unit = 4096u; /* sector mínimo NOR (§25) */
  g->page_size = 256u;
  g->program_granularity = 1u;
  g->t_prog_max_us = 700u;
  g->t_erase_max_us = 45000u;
  g->t_read_max_us = 100u;
}

mfs_st mfs_hal_detect(mf_t *fs, const mfs_config *cfg, mfs_hwv_t *out) {
  mfs_media_geom g;
  memset(&g, 0, sizeof(g));
  bool have_geom = false;

  if (cfg->geom) {
    g = *cfg->geom;
    have_geom = true;
  } else if (g_hal) {
    /* fase 1-3: JEDEC → SFDP → CFI; aquí se delega al BSP (el core no
     * habla con el controlador directamente, §2 P4). */
    geom_defaults(&g);
    uint8_t id[4] = {0};
    if (g_hal->jedec_id && g_hal->jedec_id(id) == MFS_OK && id[0] != 0xFFu) {
      out->hw_version = (uint16_t)((id[1] << 8) | id[2]);
    }
    if (g_hal->sfdp_parse && g_hal->sfdp_parse(&g) == MFS_OK)
      have_geom = true;
  }

  if (!have_geom && !cfg->drv)
    return MFS_EINVAL;

  memset(out, 0, sizeof(*out));
  out->magic[0] = 'M';
  out->magic[1] = 'H';
  out->magic[2] = 'W';
  out->magic[3] = 'V';
  out->arch_class = cfg->arch_class; /* 0 ⇒ MFS_EARCH (§24.2 p2) */
  out->mode_forced = (cfg->forced_mode == MFS_MODE_UNSUPPORTED)
                         ? 0xFFu
                         : (uint8_t)cfg->forced_mode;
  out->ram_total = cfg->ram_total;
  out->program_granularity = g.program_granularity ? g.program_granularity : 1u;
  out->erase_unit = g.erase_unit ? g.erase_unit : 4096u;
  /* El layout reserva sectores completos para SB/HWV/tokens: se exige un
   * erase_unit >= 1024 B (NOR sector típico 4 KB, §25). */
  if (out->erase_unit < 1024u)
    out->erase_unit = 4096u;
  out->oob_bytes = g.oob_bytes;
  out->t_prog_max_us =
      g.t_prog_max_us ? g.t_prog_max_us : 700u; /* §25 default */
  out->t_erase_max_us = g.t_erase_max_us ? g.t_erase_max_us : 45000u;
  out->t_read_max_us = g.t_read_max_us ? g.t_read_max_us : 100u;
  out->t_suspend_max_us = g.t_suspend_max_us;
  out->flags0 = g.flags0;
  out->flags1 = g.flags1;
  out->media_type = (uint32_t)g.type;
  out->media_size = g.size;
  out->profile_id = cfg->profile_id;

  /* MFS-CAP-001: la capacidad del medio T1 debe caber en el máximo
   * certificado por el perfil de su tecnología (mfs_profile.c). Un medio mayor
   * que su cota de perfil se rechaza de forma tipificada en vez de direccionar
   * en silencio una región no certificada. */
  {
    mfs_st cap = mfs_profile_check((mfs_media_type_t)out->media_type, g.size);
    if (cap != MFS_OK)
      return cap;
  }

  /* fase 8: assets de plataforma (PUF/rail/supercap/TZ + aceleradores) */
  if (g_hal && g_hal->assets) {
    uint8_t f2 = 0, f3 = 0;
    if (g_hal->assets(&f2, &f3) == MFS_OK) {
      out->flags2 = f2;
      out->flags3 = f3;
    }
  }

  /* T0 (FRAM/MRAM/EEPROM secundario) para HMT (§12.4) */
  if (cfg->drv_t0 && cfg->geom_t0) {
    /* MFS-CAP-001: el tier T0 también se valida contra su perfil. */
    mfs_st cap0 = mfs_profile_check((mfs_media_type_t)cfg->geom_t0->type,
                                    (uint64_t)cfg->geom_t0->size);
    if (cap0 != MFS_OK)
      return cap0;
    out->t0_size = cfg->geom_t0->size;
    /* t0_write_ns: medido o declarado */
    out->t0_write_ns = cfg->geom_t0->t_prog_max_us * 1000u;
  }

  /* capacidad suspend: solo si el driver provee suspend/resume */
  if (!(cfg->drv && cfg->drv->suspend))
    out->flags0 &= (uint8_t)(0xFFu & ~(uint32_t)MFS_HWV0_SUSPEND_E);

  /* velocidad de bus: declarada o medida (read-only, MFS-BUS-001) */
  if (cfg->bus_speed_hz)
    out->bus_speed_hz = cfg->bus_speed_hz;
  else if (cfg->drv)
    out->bus_speed_hz = mfs_bus_measure_hz(cfg->drv, cfg->drv->ctx);

  fs->hwv = *out;
  return MFS_OK;
}

/* ==== Selección de modo (§6.1) ==== */
mfs_mode_t mfs_select_mode(const mfs_hwv_t *hwv, const mfs_config *cfg) {
  /* manifiesto fijado manda (fase 10) */
  if (hwv->mode_forced != 0xFFu && hwv->mode_forced < MFS_MODE_COUNT) {
    return (mfs_mode_t)hwv->mode_forced;
  }
  /* presupuesto disponible = ram_total − firmware − stack − perifs − margen
   * (MFS-VIA-002: los valores son ABSOLUTOS; 0 ⇒ defaults porcentuales) */
  uint32_t total = hwv->ram_total;
  uint32_t fw =
      cfg->ram_firmware_min ? cfg->ram_firmware_min : (total / 2u); /* 50% */
  uint32_t stk = cfg->ram_stack_min ? cfg->ram_stack_min : (total * 15u / 100u);
  uint32_t peri =
      cfg->ram_peripherals ? cfg->ram_peripherals : (total * 8u / 100u);
  uint32_t marg = cfg->ram_margin ? cfg->ram_margin : (total * 10u / 100u);
  if (marg < total / 10u)
    marg = total / 10u; /* margen ≥10% obligatorio (§6.1) */
  if (fw + stk + peri + marg > total)
    return MFS_MODE_UNSUPPORTED; /* ENOTVIABLE */
  uint32_t avail = total - fw - stk - peri - marg;

  for (int m = MFS_MODE_EXTENDED; m >= MFS_MODE_ULTRA_NANO; m--) {
    if (avail >= mfs_limits[m].ram_total)
      return (mfs_mode_t)m;
  }
  return MFS_MODE_UNSUPPORTED;
}

/* ==== Suite preferida/negociada (§10.6) ==== */
bool mfs_edp_capable(mf_t *fs) {
  /* EDP exige ADC de rail o supercap declarados (§12.3) o hook de test */
  if (fs == NULL || fs->cfg == NULL)
    return false;
  extern bool g_mfs_edp_inject;
  return ((fs->hwv.flags2 & (MFS_HWV2_ADC_RAIL | MFS_HWV2_SUPERCAP)) != 0u) ||
         g_mfs_edp_inject;
}

uint8_t mfs_negotiate_suite(const mfs_hwv_t *hwv, const mfs_config *cfg,
                            mfs_mode_t mode) {
  if (mode == MFS_MODE_ULTRA_NANO)
    return (uint8_t)MFS_SUITE_NONE; /* sin suites */
  if (cfg->key == NULL)
    return (uint8_t)MFS_SUITE_NONE;
  bool crypto_hw = (hwv->flags3 & MFS_HWV3_CRYPTO_HW) != 0u;
  bool b3_hw = (hwv->flags3 & MFS_HWV3_B3_HW) != 0u;
  bool ascon_hw = (hwv->flags3 & MFS_HWV3_ASCON_HW) != 0u;
  if (cfg->suite_preferred != 0xFFu) {
    uint8_t want = cfg->suite_preferred;
    if (want == MFS_SUITE_S1 && !crypto_hw && !b3_hw) {
      /* S1 exige HW AES o B3; sin ninguno degradar a S3 SW */
      return (uint8_t)MFS_SUITE_S3;
    }
    if (want == MFS_SUITE_S2 && !ascon_hw && !b3_hw)
      return (uint8_t)MFS_SUITE_S3;
    return want;
  }
  if (crypto_hw && b3_hw)
    return (uint8_t)MFS_SUITE_S1;
  if (ascon_hw || b3_hw)
    return (uint8_t)MFS_SUITE_S2;
  return (uint8_t)MFS_SUITE_S3; /* fallback SW determinista */
}

/* Exportar HWV para tests/diagnóstico */
const mfs_hwv_t *mf_last_hwv(void) { return &g_last_hwv; }
mfs_mode_t mf_last_selected(void) { return g_last_selected; }
