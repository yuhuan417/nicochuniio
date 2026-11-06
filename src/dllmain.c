/*
 * dllmain.c - nothing to do (same as the original)
 */
#include "nicochuniio.h"

BOOL APIENTRY DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        log_init(hinst);
        log_write("=== nicochuniio (source rebuild) loaded, build " __DATE__ " " __TIME__ " ===");
    }
    return TRUE;
}
