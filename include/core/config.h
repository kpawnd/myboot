/*
 * Build-configuration switches. The Makefile defines exactly one of
 * BOOT_CONFIG_DEBUG / BOOT_CONFIG_BALANCED / BOOT_CONFIG_RELEASE; everything
 * else is derived here so source files only ever test BOOT_* feature macros.
 *
 *   BOOT_LOG      diagnostic console output (banner, discovery summary)
 *   BOOT_DEBUG    verbose output and assertions
 *   BOOT_PROFILE  TSC timing points
 *
 * Code under a disabled switch must compile away completely: release images
 * contain no logging strings, no formatting code and no timing state.
 */
#ifndef CORE_CONFIG_H
#define CORE_CONFIG_H

#if defined(BOOT_CONFIG_DEBUG)
#  define BOOT_CONFIG_NAME L"debug"
#  define BOOT_LOG     1
#  define BOOT_DEBUG   1
#  define BOOT_PROFILE 1
#elif defined(BOOT_CONFIG_BALANCED)
#  define BOOT_CONFIG_NAME L"balanced"
#  define BOOT_LOG     1
#  define BOOT_DEBUG   0
#  define BOOT_PROFILE 1
#elif defined(BOOT_CONFIG_RELEASE)
#  define BOOT_CONFIG_NAME L"release"
#  define BOOT_LOG     0
#  define BOOT_DEBUG   0
#  define BOOT_PROFILE 0
#else
#  error "define one of BOOT_CONFIG_DEBUG, BOOT_CONFIG_BALANCED, BOOT_CONFIG_RELEASE"
#endif

#endif
