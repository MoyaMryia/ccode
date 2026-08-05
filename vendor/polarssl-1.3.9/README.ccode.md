# PolarSSL 1.3.9 (vendored for ccode retro HTTPS)

Upstream: https://github.com/Mbed-TLS/mbedtls/tree/polarssl-1.3.9
License: GPLv2 (see LICENSE). sha256 of upstream tarball
`mbedtls-polarssl-1.3.9.tar.gz`: cdff95a7163da4b1952f998222c282e419c683a06c59b42eb8a79e43cbec7ff7

## Purpose

A TLS 1.2 client library that compiles with the BasicLinux retro toolchain
(gcc 2.7.2.3 / egcs 1.1.2, libc5, i586). mbedTLS 2.28 (vendor/mbedtls) is
C99-only and cannot build there; this tree serves the RETRO build while the
modern host build keeps mbedTLS (or can link system OpenSSL dynamically).

## Local patches (keep this list in sync)

1. `include/polarssl/config.h` — replaced upstream default (saved at
   `configs/config-default.h`) with a minimal TLS 1.2 certificate-client
   config. Verified working (host + guest, egcs 1.1.2):
   - base: PLATFORM/ERROR/MD/CIPHER/SHA1/SHA256
   - ciphers: AES + GCM + CIPHER_MODE_CBC
   - key exchange: ECDHE-RSA + RSA (POLARSSL_KEY_EXCHANGE_*_ENABLED)
   - RSA: PKCS1_V15 (missing this silently strips all v1.5 verify paths)
   - ECC: ECP + secp256r1 + ECDH
   - X.509: PK/PK_PARSE/X509_USE/X509_CRT_PARSE/ASN1/OID/PEM_PARSE/BASE64
     (1.3.9 uses POLARSSL_PEM_PARSE_C, not the mbedTLS-2.x name PEM_C)
   - TLS: SSL_TLS_C/SSL_CLI_C/SSL_PROTO_TLS1_2/SSL_SERVER_NAME_INDICATION
   - entropy: ENTROPY(+FORCE_SHA256)/CTR_DRBG/TIMING/HAVEGE
   - system: HAVE_LONGLONG/HAVE_TIME/HAVE_TIME_DATE/FS_IO
   No server side, no SSL3/TLS1.0/1.1, no weak ciphers, no asm, no SSE2.
2. `library/Makefile` — dropped `-D_FILE_OFFSET_BITS=64` (libc5 has no
   64-bit off_t; the flag breaks stdio.h on BasicLinux).

## Build (retro)

```sh
make -C vendor/polarssl-1.3.9/library \
  CC=gcc-egcs-1.1.2 \
  CFLAGS="-O2 -I../include -include src/compat/compat.h -Isrc/compat"
```
