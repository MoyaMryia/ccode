#ifndef CCODE_TLS_BACKEND_H
#define CCODE_TLS_BACKEND_H
/* TLS backend selection, shared by http.c and webfetch.c.
 *   CCODE_TLS_NONE      plain HTTP only (HTTP_ONLY build)
 *   CCODE_TLS_MBEDTLS   vendored mbedTLS 2.28 (modern host, default)
 *   CCODE_TLS_POLARSSL  vendored PolarSSL 1.3.9 (retro guest: gcc 2.7/egcs 1.1.2)
 *   CCODE_TLS_OPENSSL   dynamically linked system OpenSSL (opt-in, not yet)
 * The Makefile passes the macro explicitly per build mode; the defaults
 * below keep hand-rolled builds working. */
#define CCODE_TLS_NONE      0
#define CCODE_TLS_MBEDTLS   1
#define CCODE_TLS_POLARSSL  2
#define CCODE_TLS_OPENSSL   3

#ifndef CCODE_TLS_BACKEND
/* Order matters: the RETRO build also defines CCODE_HTTP_ONLY (historically
 * the two were synonyms), so RETRO must win the derivation. */
#if defined(CCODE_RETRO)
#define CCODE_TLS_BACKEND CCODE_TLS_POLARSSL
#elif defined(CCODE_HTTP_ONLY)
#define CCODE_TLS_BACKEND CCODE_TLS_NONE
#else
#define CCODE_TLS_BACKEND CCODE_TLS_MBEDTLS
#endif
#endif
#endif
