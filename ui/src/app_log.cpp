/* SPDX-License-Identifier: GPL-3.0-or-later */

#include "app_log.hpp"

#include "platform/ps5/system.hpp"

#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>

namespace ghostcontrol
{
namespace
{
/* A title's sandbox has no /data; /download0 is its own writable storage. */
constexpr const char *kDir = "/download0/ghostcontrol";
constexpr const char *kPath = "/download0/ghostcontrol/app.log";
/* Best-effort mirror outside the sandbox: if this title can write it at all,
 * the PC can read the log even while the app is closed. Failure is silent. */
constexpr const char *kAltPath = "/data/ghostcontrol/logs/app-ui.log";

std::FILE *g_file = nullptr;
std::FILE *g_alt_file = nullptr;
} // namespace

void app_log_open()
{
    if (g_file)
        return;
    mkdir("/download0", 0777);
    mkdir(kDir, 0777);
    /* Truncated per run: the last run is the one being diagnosed. */
    g_file = std::fopen(kPath, "w");
    if (!g_file)
        hui::sys::log("[GC] could not open %s", kPath);
    /* Truncated per run as well; this one is the cross-sandbox probe. */
    g_alt_file = std::fopen(kAltPath, "w");
    if (!g_alt_file)
        hui::sys::log("[GC] could not open %s", kAltPath);
}

void app_log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0)
        return;

    /* Always to the kernel channel, so a run with no writable storage still
     * says something somewhere. */
    hui::sys::log("%s", line);
    if (g_alt_file)
    {
        std::fputs(line, g_alt_file);
        std::fputc('\n', g_alt_file);
        std::fflush(g_alt_file);
    }
    if (!g_file)
        return;
    std::fputs(line, g_file);
    std::fputc('\n', g_file);
    /* Flushed every line: a stall leaves no chance to flush later. */
    std::fflush(g_file);
}

void app_log_close()
{
    if (g_alt_file)
    {
        std::fclose(g_alt_file);
        g_alt_file = nullptr;
    }
    if (!g_file)
        return;
    std::fclose(g_file);
    g_file = nullptr;
}

} // namespace ghostcontrol
