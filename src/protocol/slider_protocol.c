/*
 * protocol/slider_protocol.c - host-to-board command builders
 */
#include "nicochuniio.h"
#include <string.h>

/* global outgoing scratch packet (exported) */
packets request;                          /* 0x6804ae40 */

/* ------------------------------------------------------------------ */
void output_packet(uint8_t cmd, uint8_t size, const uint8_t *data)
{
    EnterCriticalSection(&serial_control_cs);
    packet_init(&request);
    request.data[0] = 0xFF;
    request.data[1] = cmd;
    request.data[2] = size;
    if (size != 0 && data != NULL)
        memcpy(&request.data[3], data, size);
    pack_data(&request);

    /* the original sleeps after state-changing commands */
    if (cmd == 3 || cmd == 4 || cmd == 6)
        Sleep(1);
    else if (cmd == 0x10)
        Sleep(10);
    LeaveCriticalSection(&serial_control_cs);
}

void f_get_hardware_info(void)
{
    output_packet(0xF0, 0, NULL);
}

void f_reset(void)
{
    output_packet(0x10, 0, NULL);
    if (!HardwareInit)
        f_get_hardware_info();
}

void f_slider_led_report(const uint8_t *rgb)
{
    uint8_t buf[0x61];
    buf[0] = 0x64;                        /* 'd' prefix, same as original */
    memcpy(&buf[1], rgb, 0x60);
    output_packet(2, 0x61, buf);
}

void f_enable_slider_report(void)
{
    output_packet(3, 0, NULL);
}

void f_disable_slider_report(void)
{
    output_packet(4, 0, NULL);
}

void f_enable_air_report(void)
{
    output_packet(6, 0, NULL);
}

void f_air_led_report_L(const uint8_t *rgb)
{
    (void)rgb;                            /* stub, same as original */
}

void f_air_led_report_R(const uint8_t *rgb)
{
    (void)rgb;                            /* stub, same as original */
}

void report_init(void)
{
    EnterCriticalSection(&serial_control_cs);
    Sleep(5);
    f_reset();
    f_enable_slider_report();
    f_enable_air_report();
    LeaveCriticalSection(&serial_control_cs);
}
