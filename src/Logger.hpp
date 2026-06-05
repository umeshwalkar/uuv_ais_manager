#pragma once
#include <cstdint>
#include <mutex>
#include <chrono>
#include <cstdarg>

/*
 * Logger — tick-stamped, level-gated console logger
 *
 * Output format:
 *   [0000012345ms] [ERR] [ModuleName    ] message text
 *
 * Levels:
 *   ERROR — always printed; hard faults, unrecoverable failures
 *   WARN  — always printed; recoverable issues, degraded state
 *   INFO  — printed when debug is enabled; lifecycle events
 *   DEBUG — printed when debug is enabled; payloads, counters, fine-grained flow
 *
 * Global debug switch controlled by Logger::setEnabled(). Per-channel payload
 * printing is the caller's responsibility (check channel.debug before calling
 * LOG_DBG for payload content).
 */

enum class LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERROR = 3 };

class Logger {
public:
    static void     init();                  // record app start time — call once at main()
    static void     setEnabled(bool en);     // master switch for INFO/DEBUG messages
    static void     setLevel(LogLevel lvl);  // minimum level (default WARN when disabled)
    static uint64_t tick();                  // milliseconds since init()

    // ERR / WRN are ALWAYS printed regardless of enabled / level.
    static void error(const char* module, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));
    static void warn (const char* module, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));
    // INF / DBG respect enabled flag and min_level_.
    static void info (const char* module, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));
    static void debug(const char* module, const char* fmt, ...)
        __attribute__((format(printf, 2, 3)));

private:
    static std::chrono::steady_clock::time_point start_;
    static bool      enabled_;
    static LogLevel  min_level_;
    static std::mutex mutex_;

    static void emit(const char* tag, const char* module, const char* fmt, std::va_list ap);
};

// ── Convenience macros ──────────────────────────────────────────────────────
#define LOG_ERR(mod, ...) Logger::error(mod, __VA_ARGS__)
#define LOG_WRN(mod, ...) Logger::warn (mod, __VA_ARGS__)
#define LOG_INF(mod, ...) Logger::info (mod, __VA_ARGS__)
#define LOG_DBG(mod, ...) Logger::debug(mod, __VA_ARGS__)
