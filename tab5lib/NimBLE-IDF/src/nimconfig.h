// NimBLE-Arduino's nimconfig.h, for the framework's own NimBLE (ESP-IDF's bt component, as its sdkconfig has it): the
// C++ API's own settings only. The host's are the sdkconfig's (CONFIG_BT_NIMBLE_*), and its headers ESP-IDF's
#pragma once

#include "sdkconfig.h"

#ifndef CONFIG_BT_CONTROLLER_DISABLED
#define CONFIG_BT_CONTROLLER_DISABLED 0
#endif

#if CONFIG_BT_NIMBLE_ENABLE_PERIODIC_ADV && !CONFIG_BT_NIMBLE_EXT_ADV
#  error Extended advertising must be enabled to use periodic advertising.
#endif

/* Must have max instances and data length set if extended advertising is enabled */
#if CONFIG_BT_NIMBLE_EXT_ADV
#  if !defined(CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES)
#    define CONFIG_BT_NIMBLE_MAX_EXT_ADV_INSTANCES 1
#  endif
#  if !defined(CONFIG_BT_NIMBLE_MAX_EXT_ADV_DATA_LEN)
#    define CONFIG_BT_NIMBLE_MAX_EXT_ADV_DATA_LEN 1650
#  endif
#  if !defined(CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE)
#    define CONFIG_BT_NIMBLE_EXT_ADV_MAX_SIZE CONFIG_BT_NIMBLE_MAX_EXT_ADV_DATA_LEN
#  endif
#endif

/* Must set max number of syncs if periodic advertising is enabled */
#if CONFIG_BT_NIMBLE_ENABLE_PERIODIC_ADV && !defined(CONFIG_BT_NIMBLE_MAX_PERIODIC_SYNCS)
#  define CONFIG_BT_NIMBLE_MAX_PERIODIC_SYNCS 1
#endif

#ifndef CONFIG_NIMBLE_CPP_DEBUG_ASSERT_ENABLED
#define CONFIG_NIMBLE_CPP_DEBUG_ASSERT_ENABLED 0
#endif

#ifndef CONFIG_NIMBLE_CPP_FREERTOS_TASK_BLOCK_BIT
#define CONFIG_NIMBLE_CPP_FREERTOS_TASK_BLOCK_BIT 31
#endif

#if CONFIG_NIMBLE_CPP_DEBUG_ASSERT_ENABLED && !defined NDEBUG
void nimble_cpp_assert(const char *file, unsigned line) __attribute((weak, noreturn));
# define NIMBLE_ATT_VAL_FILE  (__builtin_strrchr(__FILE__, '/') ? \
                            __builtin_strrchr (__FILE__, '/') + 1 : __FILE__)
# define NIMBLE_CPP_DEBUG_ASSERT(cond) \
    if (!(cond)) { \
        nimble_cpp_assert(NIMBLE_ATT_VAL_FILE, __LINE__); \
    }
#else
# define NIMBLE_CPP_DEBUG_ASSERT(cond) (void(0))
#endif

// (not USING_NIMBLE_ARDUINO_HEADERS: the framework's NimBLE headers, as esp-nimble-cpp has them)
