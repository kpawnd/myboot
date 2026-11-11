/* Build-configuration switches. */
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

#ifndef BOOT_CSME_QUERY
#  define BOOT_CSME_QUERY 1
#endif

#endif
