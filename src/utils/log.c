/*
 * utils/log.c - lightweight event log (new in the rebuild, FIX-7)
 *
 * Writes rare, diagnostic events to nicochuniio.log next to the DLL.
 * Normal gameplay produces no log traffic; only state transitions
 * (watchdog, reconnects, open/close, write failures) are recorded.
 */
#include "nicochuniio.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static HANDLE g_log_mutex;
static char   g_log_path[MAX_PATH];
static bool   g_log_ready;

void log_init(HINSTANCE self)
{
    g_log_mutex = CreateMutexA(NULL, FALSE, NULL);

    if (self == NULL)
        return;

    char path[MAX_PATH];
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0)
        return;

    int len = (int)strlen(path);
    while (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/')
        len--;

    snprintf(g_log_path, sizeof(g_log_path), "%.*snicochuniio.log", len, path);
    g_log_ready = true;
}

void log_write(const char *fmt, ...)
{
    if (!g_log_ready)
        return;

    if (WaitForSingleObject(g_log_mutex, 100) != 0)
        return;

    FILE *f = fopen(g_log_path, "a");
    if (f != NULL) {
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu] ",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                (unsigned long)GetCurrentProcessId());

        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);

        fputc('\n', f);
        fclose(f);
    }

    ReleaseMutex(g_log_mutex);
}
