/*
 * core/input_packet.c - consumer side of the two input streams
 */
#include "nicochuniio.h"
#include <string.h>

/* ------------------------------------------------------------------ */
/* serial stream consumer (slider thread context)                      */
/* ------------------------------------------------------------------ */
void process_serial_data(uint8_t *pressure,
                         uint8_t *air_status,
                         uint8_t led_status,
                         bool auto_reconnect,
                         DWORD *last_reconnect_attempt,
                         DWORD reconnect_interval_ms,
                         const char *vid,
                         const char *pid,
                         const wchar_t *serial_port,
                         int baud_rate)
{
    static packets g_packet;              /* 0x6804a2e0 */
    static bool    g_packet_init_done;    /* 0x6804a2c0 */
    static DWORD   last_tick;             /* 0x6804a360 */
    static BOOL    serial_open_state;     /* 0x6804a364 */
    static BOOL    need_reconnect;        /* 0x6804a365 */
    const DWORD    watchdog_ms = 100;

    if (!g_packet_init_done) {
        packet_init(&g_packet);
        g_packet_init_done = true;
    }

    /* every 100 ms: refresh the serial open-state and flag drops */
    DWORD now = GetTickCount();
    if (now - last_tick >= watchdog_ms) {
        last_tick = now;
        BOOL open = IsSerialPortOpen();
        if (open != serial_open_state) {
            serial_open_state = open;
            if (!open) {
                slider_led_tx_set_ready(FALSE);
                need_reconnect = TRUE;
                log_write("serial open state lost (comm error/closed), scheduling reconnect");
            }
        }
    }

    if (!serial_open_state)
        goto try_reconnect;

    {
        uint8_t cmd = serial_read_cmd(&g_packet);

        switch (cmd) {
        case 0x01:                          /* slider frame: 32 cells + air */
            memcpy(pressure, &g_packet.data[3], 0x20);
            if (g_packet.data[2] == 0x21) {
                uint8_t air_mask = (led_status != 0) ? 0xFF : 0x00;
                *air_status = (uint8_t)(g_packet.data[0x23] & air_mask);
            }
            packet_init(&g_packet);
            break;

        case 0x05:                          /* dedicated air frame */
        {
            uint8_t air_mask = (led_status != 0) ? 0xFF : 0x00;
            *air_status = (uint8_t)(g_packet.data[3] & air_mask);
            packet_init(&g_packet);
            break;
        }

        case 0xEE:                          /* resync request */
            report_init();
            packet_init(&g_packet);
            break;

        case 0xF0:                          /* hardware info ack */
            HardwareInit = 1;
            break;

        case 0xFF:                          /* error frame: close + reconnect */
            log_write("board sent error frame (0xFF), closing port");
            if (pressure != NULL)
                memset(pressure, 0, 0x20);
            close_port();
            slider_led_tx_set_ready(FALSE);
            serial_open_state = FALSE;
            need_reconnect = TRUE;
            break;

        default:                            /* 0xFE (no frame) / garbage */
            break;
        }
    }

try_reconnect:
    if (!need_reconnect || !auto_reconnect || serial_port == NULL)
        return;

    now = GetTickCount();
    if (reconnect_interval_ms >= now - *last_reconnect_attempt)
        return;

    *last_reconnect_attempt = now;

    {
        static unsigned attempts;
        if (attempts++ % 20 == 0)
            log_write("reconnect attempt #%u", attempts);
    }

    if (pressure != NULL)
        memset(pressure, 0, 0x20);

    if (connect_serial_port(vid, pid, serial_port, (DWORD)baud_rate)) {
        log_write("reconnect OK");
        report_init();
        slider_led_tx_set_ready(TRUE);
        serial_open_state = TRUE;
        need_reconnect = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* io4 HID stream consumer (io4 thread context)                        */
/* ------------------------------------------------------------------ */
BOOL process_io4_data(void)
{
    static HANDLE hReadEvent;             /* 0x6804a368 */
    static BOOL   read_pending;           /* 0x6804a36c */
    static OVERLAPPED ov;                 /* 0x6804a370 */
    static uint8_t read_buf[0x40];        /* 0x6804a3a0 */

    if (!IsHidDeviceOpen()) {
        if (hReadEvent != NULL) {
            CloseHandle(hReadEvent);
            hReadEvent = NULL;
        }
        read_pending = FALSE;
        return FALSE;
    }

    if (hReadEvent == NULL) {
        hReadEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (hReadEvent == NULL)
            return FALSE;
    }

    if (!read_pending) {
        DWORD bytes = 0;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = hReadEvent;
        ResetEvent(hReadEvent);

        if (ReadFile(hHidDevice, read_buf, 0x40, &bytes, &ov)) {
            if (bytes == 0x40) {
                memcpy(&io4_received_data, read_buf, 0x40);
                return TRUE;
            }
            return FALSE;
        }

        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING)
            goto fail;

        read_pending = TRUE;
        return FALSE;
    }

    {
        DWORD bytes = 0;
        if (!GetOverlappedResult(hHidDevice, &ov, &bytes, FALSE)) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_INCOMPLETE)
                return FALSE;
            goto fail;
        }

        read_pending = FALSE;
        if (bytes == 0x40) {
            memcpy(&io4_received_data, read_buf, 0x40);
            return TRUE;
        }
        return FALSE;
    }

fail:
    read_pending = FALSE;
    close_hid_device();
    return FALSE;
}
