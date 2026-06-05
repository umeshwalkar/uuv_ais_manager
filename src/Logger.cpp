#include "Logger.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── Static members ──────────────────────────────────────────────────────────

std::chrono::steady_clock::time_point Logger::start_;
bool      Logger::enabled_   = false;
LogLevel  Logger::min_level_ = LogLevel::WARN;
std::mutex Logger::mutex_;

// ── Public API ──────────────────────────────────────────────────────────────

void Logger::init()
{
    start_ = std::chrono::steady_clock::now();
}

void Logger::setEnabled(bool en)
{
    enabled_   = en;
    min_level_ = en ? LogLevel::DEBUG : LogLevel::WARN;
}

void Logger::setLevel(LogLevel lvl)
{
    min_level_ = lvl;
}

uint64_t Logger::tick()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_
        ).count()
    );
}

// ── Level entry points ──────────────────────────────────────────────────────

void Logger::error(const char* module, const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    emit("ERR", module, fmt, ap);
    va_end(ap);
}

void Logger::warn(const char* module, const char* fmt, ...)
{
    std::va_list ap;
    va_start(ap, fmt);
    emit("WRN", module, fmt, ap);
    va_end(ap);
}

void Logger::info(const char* module, const char* fmt, ...)
{
    if (!enabled_ || min_level_ > LogLevel::INFO) return;
    std::va_list ap;
    va_start(ap, fmt);
    emit("INF", module, fmt, ap);
    va_end(ap);
}

void Logger::debug(const char* module, const char* fmt, ...)
{
    if (!enabled_ || min_level_ > LogLevel::DEBUG) return;
    std::va_list ap;
    va_start(ap, fmt);
    emit("DBG", module, fmt, ap);
    va_end(ap);
}

// ── Core emit ───────────────────────────────────────────────────────────────

void Logger::emit(const char* tag, const char* module, const char* fmt, std::va_list ap)
{
    // Format message into a stack buffer; fall back to heap for long messages.
    char   stack_buf[512];
    char*  buf  = stack_buf;
    int    cap  = static_cast<int>(sizeof(stack_buf));

    std::va_list ap2;
    va_copy(ap2, ap);
    int needed = std::vsnprintf(buf, cap, fmt, ap2);
    va_end(ap2);

    char* heap_buf = nullptr;
    if (needed >= cap) {
        heap_buf = static_cast<char*>(std::malloc(needed + 1));
        if (heap_buf) {
            buf = heap_buf;
            cap = needed + 1;
            std::vsnprintf(buf, cap, fmt, ap);
        }
    }

    // Fixed-width module column (14 chars), truncated/padded with spaces.
    char mod[15];
    std::snprintf(mod, sizeof(mod), "%-14.14s", module ? module : "");

    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::printf("[%010llu] [%s] [%s] %s\n",
                    static_cast<unsigned long long>(tick()),
                    tag, mod, buf);
    }

    std::free(heap_buf);
}
