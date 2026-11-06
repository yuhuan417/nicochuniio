/*
 * hardware/serial_port.c - CDC serial transport for the slider board
 */
#include "nicochuniio.h"
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

/* globals, laid out in the same order as the original image */
CRITICAL_SECTION cs;                      /* 0x6804af28 */
DCB dcb;                                  /* 0x6804af40 */
HANDLE serial_mutex;                      /* 0x6804af5c */
HANDLE hid_mutex;                         /* 0x6804af60 */
COMMTIMEOUTS timeouts;                    /* 0x6804af64 */
char comPort[16];                         /* 0x6804af78 */

HANDLE hPort = INVALID_HANDLE_VALUE;      /* 0x68046008 */

/* enumeration result cache (3 s TTL) */
typedef struct DeviceCache {
    char vid[10];
    char pid[10];
    char port_name[256];
    DWORD last_check;
} DeviceCache;

static DeviceCache device_cache;          /* 0x6804a3e0 */

/* ------------------------------------------------------------------ */
const char *GetSerialPortByVidPid(const char *vid, const char *pid)
{
    static const char empty[] = "";
    DWORD now = GetTickCount();

    if (vid != NULL && pid != NULL &&
        strcmp(vid, device_cache.vid) == 0 &&
        strcmp(pid, device_cache.pid) == 0 &&
        device_cache.port_name[0] != '\0' &&
        now - device_cache.last_check <= 2999)
        return device_cache.port_name;

    memset(&device_cache, 0, sizeof(device_cache));
    if (vid != NULL)
    {
        strncpy_s(device_cache.vid, sizeof(device_cache.vid), vid, (size_t)-1);
    }
    if (pid != NULL)
    {
        strncpy_s(device_cache.pid, sizeof(device_cache.pid), pid, (size_t)-1);
    }
    device_cache.last_check = now;

    if (vid == NULL || pid == NULL)
        return empty;

    HDEVINFO dev_info =
        SetupDiGetClassDevsA(NULL, "USB", NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (dev_info == INVALID_HANDLE_VALUE)
        return empty;

    SP_DEVINFO_DATA dev_data;
    dev_data.cbSize = sizeof(dev_data);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(dev_info, i, &dev_data); i++) {
        char hw_id[0x400];
        if (!SetupDiGetDeviceRegistryPropertyA(dev_info, &dev_data,
                SPDRP_HARDWAREID, NULL, (PBYTE)hw_id, sizeof(hw_id), NULL))
            continue;
        if (strstr(hw_id, vid) == NULL || strstr(hw_id, pid) == NULL)
            continue;

        HKEY key = SetupDiOpenDevRegKey(dev_info, &dev_data,
                                        DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (key == INVALID_HANDLE_VALUE)
            continue;

        DWORD size = sizeof(device_cache.port_name);
        DWORD type = 0;
        LONG rc = RegQueryValueExA(key, "PortName", NULL, &type,
                                   (LPBYTE)device_cache.port_name, &size);
        RegCloseKey(key);
        if (rc == ERROR_SUCCESS && device_cache.port_name[0] != '\0') {
            SetupDiDestroyDeviceInfoList(dev_info);
            return device_cache.port_name;
        }
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return empty;
}

/* ------------------------------------------------------------------ */
BOOL open_port(const char *port, DWORD baud_rate)
{
    if (WaitForSingleObject(serial_mutex, 100) != 0)
        return FALSE;

    if (hPort != INVALID_HANDLE_VALUE) {
        CloseHandle(hPort);
        hPort = INVALID_HANDLE_VALUE;
    }

    hPort = CreateFileA(port,
                        GENERIC_READ | GENERIC_WRITE,
                        0,                    /* exclusive, same as original */
                        NULL,
                        OPEN_EXISTING,
                        0,                    /* no FILE_FLAG_OVERLAPPED      */
                        NULL);
    if (hPort == INVALID_HANDLE_VALUE) {
        log_write("serial open FAILED: %s err=%lu", port, (unsigned long)GetLastError());
        ReleaseMutex(serial_mutex);
        return FALSE;
    }

    if (!GetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        hPort = INVALID_HANDLE_VALUE;
        ReleaseMutex(serial_mutex);
        return FALSE;
    }

    dcb.BaudRate = baud_rate;
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    if (!SetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        hPort = INVALID_HANDLE_VALUE;
        ReleaseMutex(serial_mutex);
        return FALSE;
    }

    GetCommTimeouts(hPort, &timeouts);
    timeouts.ReadIntervalTimeout       = MAXDWORD;
    timeouts.ReadTotalTimeoutMultiplier = 0;
    /* FIX-2: block until the first byte arrives, capped at 2 ms (was 0 =
     * fully non-blocking, which forced the input threads to busy-spin). */
    timeouts.ReadTotalTimeoutConstant  = 2;
    /* FIX-1: write timeout = 1 ms (was 2*N+1 ms, up to ~400 ms per LED
     * frame — the root cause of the "inputs permanently lag" death spiral
     * when the board stops draining its RX side). */
    timeouts.WriteTotalTimeoutMultiplier = 0;
    timeouts.WriteTotalTimeoutConstant   = 1;
    if (!SetCommTimeouts(hPort, &timeouts)) {
        CloseHandle(hPort);
        hPort = INVALID_HANDLE_VALUE;
        ReleaseMutex(serial_mutex);
        return FALSE;
    }

    SetupComm(hPort, 0x1000, 0x1000);
    PurgeComm(hPort, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
    EscapeCommFunction(hPort, SETDTR);

    log_write("serial port opened: %s @ %lu bps", port, (unsigned long)baud_rate);
    ReleaseMutex(serial_mutex);
    return TRUE;
}

void close_port(void)
{
    if (WaitForSingleObject(serial_mutex, 100) != 0)
        return;

    if (hPort != INVALID_HANDLE_VALUE) {
        PurgeComm(hPort, PURGE_TXABORT | PURGE_RXABORT | PURGE_TXCLEAR | PURGE_RXCLEAR);
        CloseHandle(hPort);
        hPort = INVALID_HANDLE_VALUE;
        log_write("serial port closed");
    }

    ReleaseMutex(serial_mutex);
}

BOOL IsSerialPortOpen(void)
{
    BOOL result = FALSE;

    if (WaitForSingleObject(serial_mutex, 50) != 0)
        return FALSE;

    if (hPort != INVALID_HANDLE_VALUE) {
        DWORD errors = 0;
        if (ClearCommError(hPort, &errors, NULL) && errors == 0)
            result = TRUE;                /* comm errors count as "closed" */
    }

    ReleaseMutex(serial_mutex);
    return result;
}

/* ------------------------------------------------------------------ */
BOOL connect_serial_port(const char *vid, const char *pid,
                         const wchar_t *default_port, DWORD baud_rate)
{
    if (WaitForSingleObject(serial_mutex, 100) != 0)
        return FALSE;

    char found[16];
    found[0] = '\0';

    if (vid != NULL && pid != NULL) {
        const char *p = GetSerialPortByVidPid(vid, pid);
        if (p != NULL && p[0] != '\0') {
            strncpy_s(found, sizeof(found), p, (size_t)-1);
        }
    }

    if (found[0] == '\0') {
        /* board-family fallbacks, tried in the original order */
        static const char * const candidates[][2] = { SERIAL_VID_PID_CANDIDATES };
        for (size_t c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++) {
            const char *p = GetSerialPortByVidPid(candidates[c][0], candidates[c][1]);
            if (p != NULL && p[0] != '\0') {
                strncpy_s(found, sizeof(found), p, (size_t)-1);
                break;
            }
        }
    }

    if (found[0] != '\0' && strlen(found) > 3) {
        int num = 0;
        if (sscanf(found + 3, "%d", &num) == 1 && num > 0) {
            snprintf(comPort, sizeof(comPort), "\\\\.\\COM%d", num);
        } else {
            goto use_default;
        }
    } else {
use_default:
        if (default_port != NULL) {
            char tmp[13];
            memset(tmp, 0, sizeof(tmp));
            wcstombs(tmp, default_port, 12);
            strncpy(comPort, tmp, sizeof(comPort) - 1);
            comPort[sizeof(comPort) - 1] = '\0';
        }
    }

    ReleaseMutex(serial_mutex);
    return open_port(comPort, baud_rate);
}
