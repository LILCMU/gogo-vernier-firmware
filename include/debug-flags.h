#pragma once

// INFO: --- Features enabled flags ---
#define ENABLE_CDC_LOG


// INFO: --- Logging enabled flags ---
#define ENABLE_LOGGING_DEBUG 0

#define LOGGING_NONE         (0)
#define LOGGING_DEBUG        (1 << ENABLE_LOGGING_DEBUG)

#if (CORE_DEBUG_LEVEL == ARDUHAL_LOG_LEVEL_NONE)
#define ENABLE_LOGGING (LOGGING_NONE)
#else
#define ENABLE_LOGGING (LOGGING_DEBUG)
#endif

#if (ENABLE_LOGGING == LOGGING_NONE)
#define CHECK_LOGGING_FLAG(logging_bit) (0)
#else
#define CHECK_LOGGING_FLAG(logging_bit) (((ENABLE_LOGGING) >> (logging_bit)) & 0x01)
#endif
