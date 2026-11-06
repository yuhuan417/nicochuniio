/*
 * core/config.c - ini configuration (.\segatools.ini)
 *
 * Sections/keys and defaults recovered from the original binary:
 *   [io3]  test(F1) service(F2) coin(F3) ir(space / 0xAA=io4) ir%i(cell defaults) cell%i
 *   [nico] baudRate(115200) port(COM1) vid(VID_1B4F) pid(PID_8D21)
 *          autoReconnect(1) reconnectInterval(800)
 *   [gfx]  windowed(0)
 */
#include "nicochuniio.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int chuni_io_default_cells[32] = {
    0x4C, 0x4C, 0x4C, 0x4C,   /* L */
    0x4B, 0x4B, 0x4B, 0x4B,   /* K */
    0x4A, 0x4A, 0x4A, 0x4A,   /* J */
    0x48, 0x48, 0x48, 0x48,   /* H */
    0x47, 0x47, 0x47, 0x47,   /* G */
    0x46, 0x46, 0x46, 0x46,   /* F */
    0x44, 0x44, 0x44, 0x44,   /* D */
    0x53, 0x53, 0x53, 0x53,   /* S */
};

static const int chuni_io_default_ir[6] = { 0x34, 0x35, 0x36, 0x37, 0x38, 0x39 };

void chuni_io_config_load(struct chuni_io_config *cfg, const wchar_t *filename)
{
    assert(cfg != NULL);
    assert(filename != NULL);

    cfg->vk_test    = (uint8_t)GetPrivateProfileIntW(L"io3", L"test",    0x70, filename);
    cfg->vk_service = (uint8_t)GetPrivateProfileIntW(L"io3", L"service", 0x71, filename);
    cfg->vk_coin    = (uint8_t)GetPrivateProfileIntW(L"io3", L"coin",    0x72, filename);
    cfg->vk_ir_emu  = (uint8_t)GetPrivateProfileIntW(L"io3", L"ir",      0x20, filename);

    for (int i = 0; i < 6; i++) {
        wchar_t key[8];
        swprintf_s(key, 8, L"ir%d", i + 1);
        cfg->vk_ir[i] = (uint8_t)GetPrivateProfileIntW(L"io3", key, chuni_io_default_ir[i], filename);
    }

    for (int i = 0; i < 32; i++) {
        wchar_t key[8];
        swprintf_s(key, 8, L"cell%d", i + 1);
        cfg->vk_cell[i] = (uint8_t)GetPrivateProfileIntW(L"io3", key, chuni_io_default_cells[i], filename);
    }

    wchar_t port_buffer[16];

    /* baudRate (default 115200) */
    cfg->serial_baud = (int32_t)GetPrivateProfileIntW(
        L"nico", L"baudRate", 115200, filename);
    if (cfg->serial_baud <= 0)
        cfg->serial_baud = 115200;

    /* port (default COM1, prefixed with \\.\ ) */
    GetPrivateProfileStringW(L"nico", L"port", L"COM1",
                             port_buffer, (DWORD)(sizeof(port_buffer) / sizeof(port_buffer[0])), filename);
    wcsncpy(cfg->serial_port, L"\\\\.\\", 6);
    wcsncat_s(cfg->serial_port, sizeof(cfg->serial_port) / sizeof(cfg->serial_port[0]),
              port_buffer, sizeof(port_buffer) / sizeof(port_buffer[0]));

    /* vid / pid (default VID_1B4F / PID_8D21) */
    wchar_t vid[10], pid[10];
    GetPrivateProfileStringW(L"nico", L"vid", L"VID_1B4F", vid, 10, filename);
    GetPrivateProfileStringW(L"nico", L"pid", L"PID_8D21", pid, 10, filename);
    wcstombs(cfg->vid, vid, 10);
    wcstombs(cfg->pid, pid, 10);

    /* autoReconnect (default on) */
    cfg->auto_reconnect =
        GetPrivateProfileIntW(L"nico", L"autoReconnect", 1, filename) != 0;

    /* reconnectInterval (default 800 ms) */
    cfg->reconnect_interval_ms =
        (uint32_t)GetPrivateProfileIntW(L"nico", L"reconnectInterval", 800, filename);

    /* windowed comes from [gfx], used by the cursor feature */
    cfg->windowed =
        GetPrivateProfileIntW(L"gfx", L"windowed", 0, filename) != 0;
}
