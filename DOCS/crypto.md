# Criptografía: BLAKE3, SHA-256/HMAC, suites S0–S3 y cadena PQ

Spec: §10.5, §10.6, §15, §27.1.
Implementación: `src/crypto/mfs_blake3.c`, `src/crypto/mfs_sha256.c`,
`src/crypto/mfs_suites.c`, `src/sec/mfs_pq.c`; vectores en
`tests/kat_crypto.c`.

El cifrado es **por registro** (por página E2G), no por volumen: el nonce se
deriva de la posición lógica y de la época, de modo que no hay estado de cifrado
que reconstruir tras un corte y la escritura sigue siendo determinista
(MFS-SEC-001).

## Nonce normativo (MFS-SEC-001)

```c
nonce[16] = st64(epoch_seq_nonce) ‖ st64(epoch_seq_nonce ^ 0xA5A5A5A5A5A5A5A5)
```

`epoch_seq_nonce` se construye a partir de `época ‖ secuencia`; en
[`zone.md`](zone.md) se detalla su composición para el registro E2G
(`nonce = época ‖ (zona.seq + idx)`), lo que garantiza **unicidad por página**
sin contadores globales ni aleatoriedad. El sufijo XOR evita que dos páginas con
el mismo `epoch_seq_nonce` compartan también la parte alta si un cifrador sólo
usara los primeros bytes.

Un nonce **no repetido** es la única precondición del cifrado en flujo: la
unicidad la garantiza el layout (época + secuencia de zona + índice), no el
generador.

## Suites (§10.6)

| ID | Suite | Cifrado | Integridad | Requisito HW |
|---|---|---|---|---|
| `MFS_SUITE_S0` = 0 | AES-256-CTR + HMAC-SHA256 | AES-256-CTR | HMAC-SHA256 (truncado a 16 B) | software |
| `MFS_SUITE_S1` = 1 | AES-256-GCM | AES-256-GCM | tag GCM | `CRYPTO_HW` (+`B3_HW` ideal) |
| `MFS_SUITE_S2` = 2 | Ascon-128a (SP 800-232) | Ascon-128a | tag AEAD | `ASCON_HW` o `B3_HW` |
| `MFS_SUITE_S3` = 3 | ChaCha20-Poly1305 (RFC 8439) | ChaCha20 | Poly1305 | software (siempre viable) |
| `MFS_SUITE_NONE` = 0xFF | — | ninguna | CRC-32C de E2G | Ultra-Nano / sin clave |

La negociación (HAL) degrada S1/S2 a **S3** cuando el hardware declarado no
permite ejecutarlas: S3 es la implementación software determinista y siempre
está disponible.

### Formato del criptograma

En todos los casos el texto cifrado va seguido del tag: `ct ‖ tag(16 B)`, de
modo que el llamante conoce `olen = len + 16` sin metadatos extra.

| Suite | Nonce usado | Detalle |
|---|---|---|
| S0 | `nonce[0..11]` | CTR con contador en los 4 últimos bytes del bloque; **Encrypt-then-MAC** (MFS-SEC-005): `HMAC(key, nonce ‖ ct)` |
| S1 | `nonce[0..11]` | `J0 = nonce ‖ 00000001`; AAD vacío; tag `GHASH(ct) ⊕ E(J0)` |
| S2 | `nonce[0..15]` | clave de 16 B derivada de la de 32 por `K = k[0..11] ‖ k[20..23]` |
| S3 | `nonce[4..15]` (12 B) | `polykey = ChaCha20block(key, nonce, ctr=0)[0..31]`; cifrado con `ctr=1` |

### `mfs_suite_seal` / `mfs_suite_open`

1. `suite == 0xFF` ⇒ ruta **CRC-only** (Ultra-Nano, o sin clave): copia el
   payload y `olen = len`. Es lo que permite que un volumen con `MFS_SUITE_NONE`
   siga teniendo integridad mediante E2G.
2. `open` valida `clen ≥ 16` (si no ⇒ `MFS_EBADMSG`).
3. **Verificar antes de descifrar** (MFS-SEC-005): se recalcula el tag/MAC sobre
   el criptograma y se compara con `mfs_ct_equal` (**tiempo constante**). Sólo
   si coincide se descifra. Un tag incorrecto ⇒ `MFS_ESECURITY_STATE` y la salida
   no se publica.
4. Suite desconocida ⇒ `MFS_ECIPHER`.
5. Los búferes intermedios se borran con `mfs_zeroize` (volátil, anti-DCE) al
   terminar (MFS-SEC-002).

## MAC de token (§9.2)

`mfs_token_mac(suite, key, body, blen, mac[8])`:

- S3 ⇒ `polykey = ChaCha20block(key, body[0..min(blen,12)), 0)[0..31]` y
  `Poly1305(polykey, body)` truncado a 8 B.
- Resto de suites ⇒ `mfs_b3_mac_trunc(key, body, blen)`: **BLAKE3 en modo keyed**
  truncado a 8 B.

El token T1 ya lleva además su propio CRC-32C: el CRC detecta corrupción y el
MAC detecta manipulación.

## BLAKE3-256 (§10.5, MFS-B3-001)

Implementación completa del árbol (no un hash plano):

| Parámetro | Valor |
|---|---|
| `BLOCK_LEN` / `CHUNK_LEN` | 64 / 1024 B |
| Flags | `CHUNK_START 1`, `CHUNK_END 2`, `PARENT 4`, `ROOT 8`, `KEYED_HASH 16` |
| Rondas | 7 (permutación `{2,6,3,10,7,0,4,13,1,11,12,5,9,14,15,8}`) |
| Rotaciones G | 16, 12, 8, 7 |
| IV | vector estándar BLAKE2s/BLAKE3 |

- `mfs_b3_256(key, keylen, in, inlen, out[32])`: sin clave usa el IV; con clave
  de 32 B entra en `KEYED_HASH`; con otra longitud, la clave se hashea primero
  (árbol con IV sin flags) y se usan los 32 B resultantes como clave.
- Árbol: `tree_cv` divide por la mayor potencia de 2 × 1024 estrictamente menor
  que la longitud (partición canónica BLAKE3), con contadores por subárbol.
- `mfs_b3_mac_trunc` — MAC keyed truncado (§9.2).
- `mfs_b3_merkle_root(leaves, nleaf, leaf_len, root)` — raíz Merkle para el
  *checkpoint* (§9.5), hasta 64 hojas; el nivel impar se promueve sin duplicar
  (no es el padding de Bitcoin).

## SHA-256 y HMAC-SHA256

Implementación en streaming **sin heap** (§2 P2), usada por S0 y por HKDF:

- `mfs_sha256_init/update/final` con estado `h[8]`, `len`, `buf[64]`; `final`
  aplica el padding (`0x80`, ceros, longitud en **bits big-endian** en el offset
  56) y **ceroiza el contexto**.
- `mfs_hmac_sha256(key, klen, msg, mlen, out[32])` — RFC 2104/4231: clave > 64 B
  se hashea primero; `ipad = 0x36`, `opad = 0x5C`.

## KATs (§27.1)

`tests/kat_crypto.c` fija los vectores que deben reproducirse en cualquier
plataforma:

| Prueba | Vector | Esperado |
|---|---|---|
| `kat_crc32c` | `"123456789"` | `0xE3069283` |
| `kat_sha256` | `"abc"` | `ba7816bf…20015ad` |
| `kat_sha256` | vacío | `e3b0c442…852b855` |
| `kat_hmac_sha256` | RFC 4231 caso 1 | `b0344c61…e32cff7` |
| `kat_blake3` | entrada vacía | `af1349b9…e41f3262` |
| `kat_aesgcm_roundtrip` | S1 | round-trip + tamper |
| `kat_ascon_roundtrip` | S2 | round-trip + tamper |
| `kat_chacha_roundtrip` | S3 | round-trip + tamper |
| `kat_suite_tamper` | S0, S3 | clave ajena ⇒ `MFS_ESECURITY_STATE` |

La prueba genérica `suite_roundtrip()` verifica, para S1/S2/S3:

1. `seal` ⇒ `MFS_OK` y `olen = len + 16`.
2. **Determinismo**: repetir `seal` con el mismo nonce produce el mismo
   criptograma byte a byte (no hay aleatoriedad en la ruta).
3. `open` recupera el texto plano exacto.
4. Manipular el criptograma ⇒ `MFS_ESECURITY_STATE`.
5. Manipular el tag ⇒ `MFS_ESECURITY_STATE`.
6. Cambiar el nonce ⇒ `MFS_ESECURITY_STATE` (el nonce está **autenticado**).

SHA-256 se prueba además con *streaming* en trozos `{1, 63, 64}` B frente al
cálculo de una sola pasada, y BLAKE3 comprueba que el modo keyed difiere del
unkeyed y que el MAC truncado no coincide con los 8 primeros bytes del hash.

## PUF, KDF y cadena post-cuántica (§15)

`src/sec/mfs_pq.c` completa la cripto-agilidad:

| Función | Propósito |
|---|---|
| `mfs_hkdf_sha256(_info)` | HKDF (RFC 5869) sobre SHA-256: deriva la clave de trabajo del volumen a partir del secreto de arranque |
| `mfs_puf_enroll` / `mfs_puf_reproduce` | *fuzzy extractor* del PUF de SRAM con **corrección por mayoría de 3**; `helper` (64 B) y `salt` (16 B) persistidos en el HWV |
| `mfs_puf_fallback_key` | clave de respaldo derivada del UID del silicio si el PUF no es estable |
| `mfs_lms_keygen` / `mfs_lms_sign` / `mfs_lms_verify` | LMS (SP 800-208) con parámetros `(n = 32, w = 4, H = 8)`, en lugar de SLH-DSA |

Detalles y alcance en §15 y en
[`known-limitations.md`](known-limitations.md); los KATs de esta parte están en
`tests/test_pq.c`.
