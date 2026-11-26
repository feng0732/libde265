/*
 * Test-only build configuration.
 *
 * The reference picture management tests compile the library's scalar sources
 * directly into a test support library without the arch-specific SIMD
 * translation units. The top-level generated config.h would enable the SSE/AVX
 * feature macros, leaving the init_acceleration_functions_* references
 * unresolved, so the test build points HAVE_CONFIG_H at this file instead.
 */

#ifndef LIBDE265_TESTS_CONFIG_H
#define LIBDE265_TESTS_CONFIG_H

/* Platform allocation primitives available on the test host. */
#define HAVE_MALLOC_H 1
#define HAVE_POSIX_MEMALIGN 1

/* The scalar fallback acceleration table is sufficient for these tests. */
/* HAVE_SSE4_1 / HAVE_AVX2 / HAVE_AVX512 / HAVE_ARM32 intentionally undefined. */

#endif  // LIBDE265_TESTS_CONFIG_H
