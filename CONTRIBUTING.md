# Contribuir a MatrixFS Ultra «ATLAS»

Gracias por tu interés en mejorar **MatrixFS Ultra**. Al ser un sistema de archivos con pretensiones de uso en sectores regulados (industrial, médico, automoción), el proceso de contribución está diseñado para mantener la **trazabilidad normativa**: toda mejora debe poder auditarse contra la especificación MFS-SPEC-003.

## 📌 Antes de escribir código

1. Lee la sección correspondiente de [DOCS/…Guide.md](DOCS/MatrixFS%20Ultra%20-%20Technical%20Specifications%20and%20Implementation%20Guide.md). Las reglas normativas tienen ID (`MFS-RES-001`, `MFS-VIA-002`, `MFS-SEC-005`, …) que debes citar en PRs e issues.
2. Busca en [Issues](../../issues) si tu idea o bug ya está reportado.
3. Para cambios sustanciales, abre primero un issue tipo `enhancement` describiendo: problema, propuesta, secciones afectadas y **impacto esperado en RSC/pila/WCET** (§6–7, §18.2).

## 🔀 Flujo de trabajo

```bash
git checkout -b feat/<id-regla-o-tema>        # p.ej. feat/mfs-via-003-stack-profiler
# ...cambios...
make && make test                              # todo verde antes de commitear
git commit -s                                  # DCO sign-off OBLIGATORIO
git push origin feat/...
```

- **Ramas:** `main` (estable) · `develop` (integración) · `feat/*`, `fix/*`, `docs/*`.
- **Commits:** Conventional Commits (`feat:`, `fix:`, `perf:`, `docs:`, `test:`, `refactor:`) + referencia a sección: `fix(wal): token B no monótono tras carry TFC (§8.4, MFS-TOK-002)`.
- **DCO:** cada commit DEBE incluir `Signed-off-by:` (`git commit -s`). Al firmar aceptas licenciar tu contribución bajo **Apache 2.0** (§5 del LICENSE).
- **Estilo de código:** C11, sin heap (MFS-RES-001 — los revisores buscarán `malloc/calloc/realloc/free` en el diff), MISRA C:2012 orientado, cero warnings con `-Wall -Wextra`, funciones con cota de pila documentada cuando toquen pools.

## 🧪 Requisitos de verificación por tipo de cambio

| Tipo de cambio | Mínimo exigible |
|---|---|
| Corrección de bug | Test que falla antes y pasa después; si es de corrupción: caso FIH con semilla reproducibles |
| Nueva funcionalidad | KATs/tests §27 aplicables + actualización de `docs/` + matriz de conformidad |
| Cambio criptográfico | Vectors oficiales + round-trip S0–S3 + revisión de nonce (MFS-SEC-001) y ceroización (MFS-SEC-002) |
| Cambio de recursos (pools/modos) | Nuevo desglose de viabilidad (firmware+stack+perif+margen ≥10 %) y certificado RSC actualizado |
| Layout on-flash | Migración/versionado del superblock + fsck FULL sobre imagen antigua en tests |
| Rendimiento | Escenario MFS-Bench v2 concreto (W1–W11) con p50/p99/p99.9 antes/después (MFS-SCOPE-001) |

## 🐞 Reportar bugs (incluida tu propia investigación)

Usa la plantilla de Bug report e incluye: geometría del medio (real o vFlash), modo operativo, secuencia mínima de operaciones, **punto exacto de corte de energía** si hay corrupción, export HCT (`mf_health_export`) y semilla del generador FIH. Los bloqueadores de corrupción reciben prioridad P0 y análisis forense obligatorio.

## 🔐 Vulnerabilidades de seguridad

**No abras issues públicos.** Escribe a `security@matrixfs.example` con CVSS preliminar, suites/medios afectados y PoC confidencial. Divulgación coordinada: acuse ≤ 72 h, parche objetivo ≤ 90 días, crédito público opcional en el advisory.

## 💡 Proponer mejoras

Todo enhancement se evalúa contra: (a) coherencia con los 12 pilares, (b) coste de recursos en los modos inferiores (Ultra-Nano/Nano/Compact), (c) impacto de patentes/normativa, (d) complejidad verificable. Las mejoras aceptadas se anuncian en [CHANGELOG.md](CHANGELOG.md) citando la sección de spec que modifican; si cambian comportamiento normativo, requieren bump de edición de MFS-SPEC-003.

## 📝 Documentación

Las correcciones de documentación (erratas, ejemplos, claridad de reglas) se aceptan por PR directo con etiqueta `documentation`. Mantén el español como idioma base de la spec y README; añade traducciones como archivos `.en.md` paralelos, no sustitutivos.

## ✅ Checklist final del PR

- [ ] `make && make test` en verde (y `make fih-short` si toca WAL/ZLF/E2G)
- [ ] Cero asignaciones dinámicas nuevas (MFS-RES-001)
- [ ] Warnings nuevos: ninguno
- [ ] Tests que cubren el cambio (KAT/invariante/FIH según tabla superior)
- [ ] Docs/CHANGELOG actualizados
- [ ] Reglas MFS-* afectadas citadas en la descripción del PR
- [ ] `git commit -s` en todos los commits

Consejo: ejecuta `tools/mfstool conform --check` para verificar la matriz de conformidad localmente antes del review.
