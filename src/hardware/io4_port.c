/*
 * hardware/io4_port.c - HID transport for the io4 (beams / switches)
 */
#include "nicochuniio.h"
#include <initguid.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <stdio.h>
#include <string.h>

HANDLE hHidDevice = INVALID_HANDLE_VALUE;   /* 0x6804600c */

typedef struct HidDeviceCache {
    char vid[10];
    char pid[10];
    char path[256];
    DWORD last_check;
} HidDeviceCache;

static HidDeviceCache hid_device_cache;     /* 0x6804a500 */

/* ------------------------------------------------------------------ */
const char *GetHidDevicePathByVidPid(const char *vid, const char *pid)
{
    static const char empty[] = "";
    DWORD now = GetTickCount();

    if (vid != NULL && pid != NULL &&
        strcmp(vid, hid_device_cache.vid) == 0 &&
        strcmp(pid, hid_device_cache.pid) == 0 &&
        hid_device_cache.path[0] != '\0' &&
        now - hid_device_cache.last_check <= 2999)
        return hid_device_cache.path;

    memset(&hid_device_cache, 0, sizeof(hid_device_cache));
    if (vid != NULL)
        strncpy_s(hid_device_cache.vid, sizeof(hid_device_cache.vid), vid, (size_t)-1);
    if (pid != NULL)
        strncpy_s(hid_device_cache.pid, sizeof(hid_device_cache.pid), pid, (size_t)-1);
    hid_device_cache.last_check = now;

    if (vid == NULL || pid == NULL)
        return empty;

    GUID hid_guid;
    HidD_GetHidGuid(&hid_guid);

    HDEVINFO dev_info =
        SetupDiGetClassDevsA(&hid_guid, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (dev_info == INVALID_HANDLE_VALUE)
        return empty;

    unsigned int vid_num = 0, pid_num = 0;
    sscanf(vid, "VID_%4x", &vid_num);
    sscanf(pid, "PID_%4x", &pid_num);

    SP_DEVICE_INTERFACE_DATA ifc_data;
    ifc_data.cbSize = sizeof(ifc_data);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(dev_info, NULL, &hid_guid, i, &ifc_data); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailA(dev_info, &ifc_data, NULL, 0, &needed, NULL);
        if (needed == 0 || needed > 2048)
            continue;

        uint8_t detail_buf[2048];
        PSP_DEVICE_INTERFACE_DETAIL_DATA_A detail =
            (PSP_DEVICE_INTERFACE_DETAIL_DATA_A)detail_buf;
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        if (!SetupDiGetDeviceInterfaceDetailA(dev_info, &ifc_data, detail,
                                              needed, NULL, NULL))
            continue;

        HANDLE probe = CreateFileA(detail->DevicePath,
                                   GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (probe == INVALID_HANDLE_VALUE)
            continue;

        HIDD_ATTRIBUTES attr;
        attr.Size = sizeof(attr);
        BOOL ok = HidD_GetAttributes(probe, &attr);
        CloseHandle(probe);

        if (!ok)
            continue;

        if ((attr.VendorID & 0xFFFF) != vid_num || (attr.ProductID & 0xFFFF) != pid_num)
            continue;

        printf("Found IO4 device: %s\n", detail->DevicePath);
        strncpy_s(hid_device_cache.path, sizeof(hid_device_cache.path),
                  detail->DevicePath, (size_t)-1);
        SetupDiDestroyDeviceInfoList(dev_info);
        return hid_device_cache.path;
    }

    SetupDiDestroyDeviceInfoList(dev_info);
    return empty;
}

/* ------------------------------------------------------------------ */
BOOL open_hid_device(const char *path)
{
    if (WaitForSingleObject(hid_mutex, 1000) != 0)
        return FALSE;

    if (hHidDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(hHidDevice);
        hHidDevice = INVALID_HANDLE_VALUE;
    }

    hHidDevice = CreateFileA(path,
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             NULL,
                             OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED,    /* process_io4_data needs it */
                             NULL);

    BOOL ok = (hHidDevice != INVALID_HANDLE_VALUE);
    if (ok) {
        log_write("io4 HID device opened");
        /* the original did this too; harmless on HID handles */
        COMMTIMEOUTS to;
        memset(&to, 0, sizeof(to));
        to.ReadIntervalTimeout = MAXDWORD;
        SetCommTimeouts(hHidDevice, &to);
    }

    ReleaseMutex(hid_mutex);
    return ok;
}

void close_hid_device(void)
{
    if (WaitForSingleObject(hid_mutex, 1000) != 0)
        return;

    if (hHidDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(hHidDevice);
        hHidDevice = INVALID_HANDLE_VALUE;
        log_write("io4 HID device closed");
    }

    ReleaseMutex(hid_mutex);
}

BOOL IsHidDeviceOpen(void)
{
    BOOL result = FALSE;

    if (WaitForSingleObject(hid_mutex, 1000) != 0)
        return FALSE;

    result = (hHidDevice != INVALID_HANDLE_VALUE);

    ReleaseMutex(hid_mutex);
    return result;
}

BOOL connect_hid_device(const char *vid, const char *pid)
{
    if (WaitForSingleObject(hid_mutex, 1000) != 0)
        return FALSE;

    const char *path = NULL;
    if (vid != NULL && pid != NULL)
        path = GetHidDevicePathByVidPid(vid, pid);

    ReleaseMutex(hid_mutex);

    if (path == NULL || path[0] == '\0')
        return FALSE;

    return open_hid_device(path);
}
