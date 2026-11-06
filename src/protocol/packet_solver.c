/*
 * protocol/packet_solver.c - framing for the board serial protocol
 *
 * Frame layout (logical bytes after unescaping):
 *   [0xFF] [cmd] [size] [payload ...] [checksum]
 * checksum = two's-complement negation of the byte sum, so that the
 * receiver's running sum (seeded with 0xFF) ends at 0.
 * Payload escaping: 0xFF -> FD FE, 0xFD -> FD FC.
 */
#include "nicochuniio.h"
#include <string.h>

static HANDLE hWriteEvent;                /* 0x6804aa28 */

/* ------------------------------------------------------------------ */
void packet_init(packets *request)
{
    if (request != NULL)
        memset(request->data, 0, 0x80);
}

/* ------------------------------------------------------------------ */
BOOL send_packet(int length, uint8_t *send_buffer)
{
    if (length <= 0 || length > 0x80)
        return FALSE;

    if (WaitForSingleObject(serial_mutex, 10) != 0)
        return FALSE;

    BOOL ok = FALSE;

    if (hPort == INVALID_HANDLE_VALUE)
        goto out;

    if (hWriteEvent == NULL)
        hWriteEvent = CreateEventA(NULL, TRUE, FALSE, NULL);

    if (hWriteEvent != NULL) {
        OVERLAPPED ov;
        DWORD written = 0;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = hWriteEvent;
        ResetEvent(hWriteEvent);

        if (WriteFile(hPort, send_buffer, (DWORD)length, &written, &ov)) {
            ok = (written == (DWORD)length);
            goto out;
        }

        /* NOTE: MinGW's winerror.h swaps ERROR_IO_INCOMPLETE/PENDING vs the
         * real SDK; the original binary tests status 997 (0x3E5), which in
         * MinGW headers is ERROR_IO_PENDING.  Use the literal to stay exact. */
        if (GetLastError() == 997 /* ERROR_IO_INCOMPLETE, real SDK value */) {
            if (WaitForSingleObject(hWriteEvent, 20) == WAIT_OBJECT_0 &&
                GetOverlappedResult(hPort, &ov, &written, FALSE)) {
                ok = (written == (DWORD)length);
            } else {
                CancelIo(hPort);          /* original does this on wait failure */
            }
            goto out;
        }
        goto out;
    }

    /* no event available: plain synchronous write (same fallback) */
    {
        DWORD written = 0;
        if (WriteFile(hPort, send_buffer, (DWORD)length, &written, NULL))
            ok = (written == (DWORD)length);
    }

out:
    {
        /* FIX-1 diagnostic: detect the "board stops draining RX" state */
        static bool last_ok = true;
        if (last_ok && !ok)
            log_write("serial write FAILED (len=%d) - board RX wedged?", length);
        else if (!last_ok && ok)
            log_write("serial write recovered");
        last_ok = ok;
    }
    ReleaseMutex(serial_mutex);
    return ok;
}

/* ------------------------------------------------------------------ */
void pack_data(packets *request)
{
    /* 0x100-byte buffer: escaped LED frames can legitimately reach
     * 198 bytes on the wire.  send_packet() keeps the original 0x80 cap,
     * so oversized frames are dropped cleanly instead of smashing the
     * stack the way the original's 128-byte buffer could. */
    uint8_t out[0x100];
    uint8_t out_len = 0;

    uint8_t checksum = (uint8_t)(-(int)request->data[0]);
    checksum -= request->data[1];
    checksum -= request->data[2];
    for (uint8_t i = 0; i < request->data[2]; i++)
        checksum -= request->data[3 + i];

    out[out_len++] = request->data[0];    /* 0xFF, set by output_packet */
    out[out_len++] = request->data[1];    /* cmd */
    out[out_len++] = request->data[2];    /* size */

    for (uint8_t i = 0; i < request->data[2]; i++) {
        uint8_t b = request->data[3 + i];
        if (b == 0xFF || b == 0xFD) {
            out[out_len++] = 0xFD;
            out[out_len++] = (uint8_t)(b - 1);
        } else {
            out[out_len++] = b;
        }
    }

    out[out_len++] = checksum;
    send_packet(out_len, out);
}

/* ------------------------------------------------------------------ */
static HANDLE hWriteEvent;                /* 0x6804aa28 */

/* ------------------------------------------------------------------ */
/* serial RX ring (single consumer: the slider thread)                  */
/* ------------------------------------------------------------------ */
static uint8_t read_buffer[0x400];        /* 0x6804a620 */
static DWORD   read_pos;                  /* 0x6804aa20 */
static DWORD   read_len;                  /* 0x6804aa24 */

/*
 * FIX-3: the refill no longer takes serial_mutex.  read_pos/read_len are
 * only touched by the slider thread, and open/close run on that same
 * thread, so there is no race; the mutex keeps protecting open/close and
 * the LED write path.  Combined with FIX-2 the ReadFile below sleeps in
 * the kernel until data arrives instead of the thread busy-spinning.
 */
BOOL data_reader(uint8_t *result)
{
    if (result == NULL)
        return FALSE;

    if (read_pos < read_len) {
        *result = read_buffer[read_pos++];
        return TRUE;
    }

    read_pos = 0;
    read_len = 0;

    if (hPort == INVALID_HANDLE_VALUE)
        return FALSE;

    if (WaitForSingleObject(serial_mutex, 0) != WAIT_OBJECT_0)
        return FALSE;
    BOOL read_ok = ReadFile(hPort, read_buffer, 0x400, &read_len, NULL);
    ReleaseMutex(serial_mutex);
    if (!read_ok)
        return FALSE;
    if (read_len == 0)
        return FALSE;

    *result = read_buffer[read_pos++];
    return TRUE;
}

/* ------------------------------------------------------------------ */
uint8_t serial_read_cmd(packets *response)
{
    static bool  initialized;             /* 0x6804aa2c */
    static uint8_t checksum;              /* 0x6804aa30 (seeded 0xFF) */
    static uint8_t state;                 /* 0x6804aa31 */
    static bool  escape;                  /* 0x6804aa34 */
    static DWORD start_time;              /* 0x6804aa38 */

    const int max_bytes_per_call = 0x200;

    if (!initialized) {
        packet_init(response);
        checksum = 0;
        state = 0;
        escape = false;
        start_time = GetTickCount();
        initialized = true;
    }

    /*
     * FIX-5: frame-header watchdog.  Returning 0xFF makes the caller close
     * the port and reconnect, which is how a silently-dead port recovers.
     * The original used 2 s, which made the DLL close/reopen the port every
     * ~2.8 s whenever the board was merely idle; with FIX-2 in place 20 s
     * keeps the watchdog without the churn.
     */
    if (GetTickCount() - start_time > 20000) {
        log_write("WATCHDOG: no frame header for >20000 ms -> forcing reconnect");
        initialized = false;
        return 0xFF;
    }

    for (int i = 0; i < max_bytes_per_call; i++) {
        uint8_t byte;
        if (!data_reader(&byte))
            break;

        if (byte == 0xFF) {               /* frame header */
            packet_init(response);
            state = 0;
            checksum = 0xFF;
            escape = false;
            response->data[0] = 0xFF;
            start_time = GetTickCount();
            continue;
        }

        if (response->data[0] != 0xFF)
            continue;                     /* waiting for header */

        if (byte == 0xFD) {               /* escape marker */
            escape = true;
            continue;
        }

        if (escape) {
            byte++;
            escape = false;
        }

        if (response->data[1] == 0) {     /* command byte */
            response->data[1] = byte;
            checksum += byte;
            continue;
        }

        if (state == 0) {                 /* size byte */
            response->data[2] = byte;
            checksum += byte;
            state = 3;
            continue;
        }

        response->data[state] = byte;     /* payload / checksum byte */
        checksum += byte;
        state++;

        if (state == (uint8_t)(response->data[2] + 4)) {
            if (checksum == 0) {          /* frame complete and valid */
                initialized = false;
                return response->data[1];
            }
            packet_init(response);        /* checksum error: resync */
            checksum = 0;
            state = 0;
        }

        if (state > 0x80) {
            packet_init(response);
            checksum = 0;
            state = 0;
        }
    }

    if (!IsSerialPortOpen()) {
        log_write("serial port no longer open (read side)");
        initialized = false;
        return 0xFF;
    }

    return 0xFE;                          /* no complete frame this call */
}
