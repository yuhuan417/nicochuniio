/*
 * nicochuniio - shared header
 *
 * Faithful reconstruction of the Fitouch2 "nicochuniio" chuniio DLL
 * (originally built with MinGW-w64 GCC 8.1.0, i686, DWARF).
 *
 * Reverse-engineered from the original fitouch2.dll (SHA-256 verified copy
 * kept as fitouch2.dll.orig).  All logic, structures, defaults, quirks and
 * global layout were recovered from its COFF symbols + DWARF debug info.
 *
 * The following intentional behavior changes were made relative to the
 * original binary (see FIXES.md for rationale):
 *   FIX-1  open_port(): WriteTotalTimeoutMultiplier 2 -> 0
 *          (write timeout 2*N+1 ms -> 1 ms; a wedged device can no longer
 *           stall the game's slider IO thread for ~200-400 ms per LED frame)
 *   FIX-2  open_port(): ReadTotalTimeoutConstant 0 -> 2
 *          (ReadFile blocks until the first byte arrives or 2 ms elapse;
 *           the input threads sleep in the kernel instead of busy-spinning)
 *   FIX-3  data_reader(): no longer takes serial_mutex around the refill
 *          (serial read/write are full-duplex; LED writes from the game
 *           thread are never blocked by input reads and vice versa)
 *   FIX-5  serial_read_cmd(): frame-header watchdog budget 2000 ms -> 20000 ms
 *          (with FIX-2 the port no longer needs a 2 s close/reopen cycle
 *           while merely idle; a genuinely dead port still recovers)
 */
#ifndef NICOCHUNIIO_H
#define NICOCHUNIIO_H

#include <windows.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* segatools chuniio API (v0x0102)                                     */
/* ------------------------------------------------------------------ */
typedef void (*chuni_io_slider_callback_t)(const uint8_t *state);

/* ------------------------------------------------------------------ */
/* configuration (read from .\segatools.ini, section [nico] / [io3])   */
/* ------------------------------------------------------------------ */
struct chuni_io_config {
    uint8_t  vk_test;                 /* +0  [io3] test            */
    uint8_t  vk_service;              /* +1  [io3] service         */
    uint8_t  vk_coin;                 /* +2  [io3] coin            */
    uint8_t  vk_ir_emu;               /* +3  [io3] ir (0xAA=io4 HID mode) */
    uint8_t  vk_ir[6];                /* +4  [io3] ir1..ir6        */
    uint8_t  vk_cell[32];             /* +10 [io3] cell1..cell32   */
    wchar_t  serial_port[12];         /* +42 [nico] port           */
    int32_t  serial_baud;             /* +68 [nico] baudRate       */
    char     vid[10];                 /* +72 [nico] vid            */
    char     pid[10];                 /* +82 [nico] pid            */
    bool     auto_reconnect;          /* +92 [nico] autoReconnect  */
    uint32_t reconnect_interval_ms;   /* +96 [nico] reconnectInterval */
    bool     windowed;                /* +100 [gfx] windowed       */
};

/* ------------------------------------------------------------------ */
/* wire protocol                                                       */
/* ------------------------------------------------------------------ */
typedef struct packets {
    union {
        struct {
            uint8_t sync;             /* 0xFF header               */
            uint8_t cmd;              /* command / response id     */
            uint8_t size;             /* payload length            */
        };
        uint8_t data[128];
    };
} packets;

/* io4 JVS-style input report carried over the 64-byte HID interface */
#pragma pack(push, 1)
typedef struct HID_InputReport_TypeDef {
    uint8_t  report_id;               /* +0                        */
    uint16_t analog[8];               /* +1                        */
    uint16_t rotary[4];               /* +17                       */
    uint8_t  coin[4];                 /* +25                       */
    uint16_t switches[2];             /* +29 (beams live here)     */
    uint8_t  system_status;           /* +33                       */
    uint8_t  usb_status;              /* +34                       */
    uint8_t  _unused[29];             /* +35                       */
} HID_InputReport_TypeDef;
#pragma pack(pop)

_Static_assert(sizeof(HID_InputReport_TypeDef) == 0x40,
               "IO4 HID report must match the original 64-byte layout");
_Static_assert(offsetof(HID_InputReport_TypeDef, switches) == 29,
               "IO4 switches must begin at byte 29");

/* ------------------------------------------------------------------ */
/* queue (utils/queue.c) — allocated but unused by the serial path;    */
/* kept for export compatibility                                       */
/* ------------------------------------------------------------------ */
typedef struct Queue {
    char *items;
    int   front;
    int   size;
    int   capacity;
} Queue;

/* ------------------------------------------------------------------ */
/* exported globals (same set as the original DLL)                     */
/* ------------------------------------------------------------------ */
extern HANDLE          hPort;             /* serial port handle        */
extern HANDLE          hHidDevice;        /* io4 HID handle            */
extern CRITICAL_SECTION cs;               /* slider_start init         */
extern CRITICAL_SECTION io4_cs;           /* io4_JVS_FSM init          */
extern HANDLE          serial_mutex;      /* jvs_init CreateMutexA     */
extern CRITICAL_SECTION serial_control_cs;
extern HANDLE          hid_mutex;         /* jvs_init CreateMutexA     */
extern DCB             dcb;
extern COMMTIMEOUTS    timeouts;
extern char            comPort[16];       /* "\\.\\COMn"               */
extern uint8_t         AIR_status;        /* byte packed w/ slider frame */
extern uint8_t         LED_status;        /* set once game sends LEDs  */
extern uint8_t         HardwareInit;      /* set when board answers 0xF0 */
extern struct chuni_io_config chuni_io_cfg;
extern packets         request;           /* outgoing packet scratch   */
extern HID_InputReport_TypeDef io4_received_data;

/* ------------------------------------------------------------------ */
/* core/chuniio.c                                                      */
/* ------------------------------------------------------------------ */
uint16_t __cdecl chuni_io_get_api_version(void);
HRESULT  __cdecl chuni_io_jvs_init(void);
void     __cdecl chuni_io_jvs_poll(uint8_t *opbtn, uint8_t *beams);
void     __cdecl chuni_io_jvs_read_coin_counter(uint16_t *out);
HRESULT  __cdecl chuni_io_slider_init(void);
void     __cdecl chuni_io_slider_start(chuni_io_slider_callback_t callback);
void     __cdecl chuni_io_slider_stop(void);
void     __cdecl chuni_io_slider_set_leds(const uint8_t *rgb);
void slider_led_tx_queue(const uint8_t *rgb);
void slider_led_tx_set_ready(BOOL ready);
HRESULT  __cdecl chuni_io_led_init(void);
void     __cdecl chuni_io_led_set_colors(uint8_t board, uint8_t *rgb);
void     __cdecl io4_switches_to_beams(const uint16_t *switches, uint8_t *beams);
BOOL     io4_JVS_FSM(void);
void     __cdecl cursor_init(void);

/* ------------------------------------------------------------------ */
/* core/config.c                                                       */
/* ------------------------------------------------------------------ */
void chuni_io_config_load(struct chuni_io_config *cfg, const wchar_t *filename);

/* ------------------------------------------------------------------ */
/* core/input_packet.c                                                 */
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
                         int baud_rate);
BOOL process_io4_data(void);

/* ------------------------------------------------------------------ */
/* hardware/serial_port.c                                              */
/* ------------------------------------------------------------------ */
BOOL open_port(const char *port, DWORD baud_rate);
void close_port(void);
BOOL IsSerialPortOpen(void);
BOOL connect_serial_port(const char *vid, const char *pid,
                         const wchar_t *default_port, DWORD baud_rate);
const char *GetSerialPortByVidPid(const char *vid, const char *pid);

/* ------------------------------------------------------------------ */
/* hardware/io4_port.c                                                 */
/* ------------------------------------------------------------------ */
BOOL open_hid_device(const char *path);
void close_hid_device(void);
BOOL IsHidDeviceOpen(void);
BOOL connect_hid_device(const char *vid, const char *pid);
const char *GetHidDevicePathByVidPid(const char *vid, const char *pid);

/* ------------------------------------------------------------------ */
/* protocol/packet_solver.c                                            */
/* ------------------------------------------------------------------ */
void    packet_init(packets *request);
BOOL    send_packet(int length, uint8_t *send_buffer);
void    pack_data(packets *request);
BOOL    data_reader(uint8_t *result);
uint8_t serial_read_cmd(packets *response);

/* ------------------------------------------------------------------ */
/* protocol/slider_protocol.c                                          */
/* ------------------------------------------------------------------ */
void output_packet(uint8_t cmd, uint8_t size, const uint8_t *data);
void f_get_hardware_info(void);
void f_reset(void);
void f_slider_led_report(const uint8_t *rgb);
void f_enable_slider_report(void);
void f_disable_slider_report(void);
void f_enable_air_report(void);
void f_air_led_report_L(const uint8_t *rgb);
void f_air_led_report_R(const uint8_t *rgb);
void report_init(void);

/* ------------------------------------------------------------------ */
/* utils/log.c (rebuild-only diagnostic facility)                       */
/* ------------------------------------------------------------------ */
void log_init(HINSTANCE self);
void log_write(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* utils/queue.c                                                       */
/* ------------------------------------------------------------------ */
Queue *createQueue(int capacity);
int    isFull(Queue *queue);
int    isEmpty(Queue *queue);
void   enqueue(Queue *queue, char item);
char   dequeue(Queue *queue);

/* board VID/PID fallbacks tried by connect_serial_port() */
#define SERIAL_VID_PID_CANDIDATES \
    { "VID_1B4F", "PID_8D21" },    \
    { "VID_1145", "PID_0003" },    \
    { "VID_1145", "PID_0005" }

/* io4 HID interface identity */
#define IO4_HID_VID "VID_0CA3"
#define IO4_HID_PID "PID_0021"

#endif /* NICOCHUNIIO_H */
