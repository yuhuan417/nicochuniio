/*
 * feat/cursor.c - park the mouse cursor out of the way in fullscreen
 */
#include "nicochuniio.h"

void __cdecl cursor_init(void)
{
    static bool parked;                   /* 0x6804aa40 */

    if (chuni_io_cfg.windowed)
        return;

    if (!parked) {
        parked = true;
        SetCursorPos(0, 0);
    }
}
