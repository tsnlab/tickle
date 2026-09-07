#pragma once

// Log levels, from most to least verbose. Messages below the current level are dropped.
typedef enum { TT_LOG_DEBUG = 0, TT_LOG_INFO = 1, TT_LOG_WARNING = 2, TT_LOG_ERROR = 3, TT_LOG_NONE = 4 } tt_LogLevel;

// Set the minimum level that gets logged (default: TT_LOG_INFO).
void tt_log_set_level(tt_LogLevel level);
