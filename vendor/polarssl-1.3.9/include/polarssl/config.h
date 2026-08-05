/**
 * \file config.h
 *
 * \brief Minimal TLS 1.2 client configuration for ccode retro builds.
 *
 * Trimmed from PolarSSL 1.3.9 default config.h (saved as
 * configs/config-default.h) to the modules a certificate-based TLS 1.2
 * client needs: RSA + ECDHE-RSA over P-256, AES-GCM, X.509 parse,
 * CTR_DRBG entropy. No server side, no SSL3/TLS1.0/1.1, no weak
 * ciphers, no DHM/ECDSA/DES/ARC4, no asm (i586-safe), no SSE2.
 *
 * Copyright (C) 2006-2014, Brainspark B.V.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */
#ifndef POLARSSL_CONFIG_H
#define POLARSSL_CONFIG_H

/* ── System support ── */
#define POLARSSL_HAVE_LONGLONG
#define POLARSSL_HAVE_TIME
#define POLARSSL_HAVE_TIME_DATE
#define POLARSSL_FS_IO

/* ── Base modules ── */
#define POLARSSL_PLATFORM_C
#define POLARSSL_ERROR_C

/* ── Message digests ── */
#define POLARSSL_MD_C
#define POLARSSL_SHA1_C
#define POLARSSL_SHA256_C

/* ── Symmetric ciphers ── */
#define POLARSSL_CIPHER_C
#define POLARSSL_CIPHER_MODE_CBC
#define POLARSSL_AES_C
#define POLARSSL_GCM_C

/* ── Key exchange ── */
#define POLARSSL_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define POLARSSL_KEY_EXCHANGE_RSA_ENABLED

/* ── RSA / ECC (ECDHE-RSA needs both) ── */
#define POLARSSL_BIGNUM_C
#define POLARSSL_RSA_C
#define POLARSSL_PKCS1_V15
#define POLARSSL_ECP_C
#define POLARSSL_ECP_DP_SECP256R1_ENABLED
#define POLARSSL_ECDH_C

/* ── X.509 certificate parsing ── */
#define POLARSSL_PK_C
#define POLARSSL_PK_PARSE_C
#define POLARSSL_X509_USE_C
#define POLARSSL_X509_CRT_PARSE_C
#define POLARSSL_ASN1_PARSE_C
#define POLARSSL_ASN1_WRITE_C
#define POLARSSL_OID_C
#define POLARSSL_PEM_PARSE_C
#define POLARSSL_BASE64_C
#define POLARSSL_VERSION_C

/* ── TLS 1.2 client ── */
#define POLARSSL_SSL_TLS_C
#define POLARSSL_SSL_CLI_C
#define POLARSSL_SSL_PROTO_TLS1_2
#define POLARSSL_SSL_SERVER_NAME_INDICATION

/* ── Randomness ── */
#define POLARSSL_ENTROPY_C
#define POLARSSL_ENTROPY_FORCE_SHA256
#define POLARSSL_CTR_DRBG_C
#define POLARSSL_TIMING_C
#define POLARSSL_HAVEGE_C

#endif /* POLARSSL_CONFIG_H */
