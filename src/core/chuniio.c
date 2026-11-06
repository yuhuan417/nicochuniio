/*
 * core/chuniio.c - segatools chuniio entry points + worker threads
 */
#include "nicochuniio.h"
#include <stdio.h>
#include <stdlib.h>
#include <intrin.h>
#include <process.h>

#ifndef PF_XMMI2_INSTRUCTIONS_AVAILABLE
#define PF_XMMI2_INSTRUCTIONS_AVAILABLE 6
#endif

static unsigned __stdcall chuni_io_slider_thread_proc(void *param);
static unsigned __stdcall chuni_io_io4_thread_proc(void *param);

/* --- exported globals ------------------------------------------------ */
struct chuni_io_config chuni_io_cfg;          /* 0x6804aec0 */
uint8_t  AIR_status;                          /* 0x6804a080 */
uint8_t  LED_status;                          /* 0x6804a0c0 */
uint8_t  HardwareInit;                        /* 0x6804a040 */
HID_InputReport_TypeDef io4_received_data;    /* 0x6804ae00 */
CRITICAL_SECTION io4_cs;                      /* 0x6804af88 */

/* --- globals (layout mirrors the original image) -------------------- */

static bool   chuni_io_coin;              /* coin key edge latch        */
static uint16_t chuni_io_coins;           /* total coins inserted       */
static DWORD  chuni_io_coin_gate;         /* read_coin_counter 10 ms gate */
static uint16_t chuni_io_coin_cached;     /* cached counter value       */
static DWORD  chuni_io_jvs_gate;          /* jvs_poll 10 ms gate        */
static uint8_t chuni_io_opbtn_cached;     /* jvs_poll test/service latch */
static uint8_t chuni_io_hand_pos;         /* single-key air simulation  */
static HANDLE chuni_io_slider_thread;
static HANDLE chuni_io_io4_thread;
static bool   chuni_io_slider_stop_flag;
static DWORD  last_reconnect_attempt;     /* passed to process_serial_data */
static CRITICAL_SECTION led_frame_cs;
static HANDLE led_frame_event;
static HANDLE led_tx_thread;
static BOOL led_tx_ready;
static BOOL led_frame_pending;
static uint8_t led_frame[0x60];
CRITICAL_SECTION serial_control_cs;

static unsigned __stdcall chuni_io_led_tx_thread_proc(void *param);

/* mode selector written on the first jvs_poll call (0xFF = undecided) */
static uint8_t chuni_io_air_mode = 0xFF;

/* keyboard mode state */
static uint8_t chuni_io_single_key_vk;

/* io4_JVS_FSM state */
static DWORD chuni_io_io4_open_gate;      /* 500 ms HID open retry gate */
static bool  chuni_io_io4_cs_ready;

/* beam mapping tables (const bytes @0x68047114 / @0x6804711c) */
static const uint8_t beam_word_map[6] = { 0, 1, 0, 1, 0, 1 };
static const uint8_t beam_bit_map[6]  = { 13, 13, 12, 12, 11, 11 };

/* ------------------------------------------------------------------ */
static void keep_awake_tick(int *counter, int period)
{
    if (++(*counter) >= period) {
        SetThreadExecutionState(ES_SYSTEM_REQUIRED);
        *counter = 0;
    }
}

/* ------------------------------------------------------------------ */
uint16_t __cdecl chuni_io_get_api_version(void)
{
    return 0x0102;
}

HRESULT __cdecl chuni_io_jvs_init(void)
{
    chuni_io_config_load(&chuni_io_cfg, L".\\segatools.ini");
    log_write("config: port=%ls baud=%d vid=%s pid=%s autoReconnect=%d reconnectInterval=%lu "
              "test=0x%02X service=0x%02X coin=0x%02X ir=0x%02X windowed=%d",
              chuni_io_cfg.serial_port, chuni_io_cfg.serial_baud,
              chuni_io_cfg.vid, chuni_io_cfg.pid,
              (int)chuni_io_cfg.auto_reconnect,
              (unsigned long)chuni_io_cfg.reconnect_interval_ms,
              chuni_io_cfg.vk_test, chuni_io_cfg.vk_service,
              chuni_io_cfg.vk_coin, chuni_io_cfg.vk_ir_emu,
              (int)chuni_io_cfg.windowed);

    serial_mutex = CreateMutexA(NULL, FALSE, NULL);
    hid_mutex    = CreateMutexA(NULL, FALSE, NULL);
    if (serial_mutex == NULL || hid_mutex == NULL)
        return E_FAIL;

    InitializeCriticalSection(&serial_control_cs);
    InitializeCriticalSection(&led_frame_cs);
    led_frame_event = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (led_frame_event == NULL)
        return E_FAIL;
    led_tx_thread = (HANDLE)_beginthreadex(NULL, 0,
        chuni_io_led_tx_thread_proc, NULL, 0, NULL);
    if (led_tx_thread == NULL)
        return E_FAIL;

    timeBeginPeriod(1);
    return S_OK;
}

void __cdecl chuni_io_jvs_read_coin_counter(uint16_t *out)
{
    if (out == NULL)
        return;

    DWORD now = GetTickCount();
    if (now - chuni_io_coin_gate > 10) {
        chuni_io_coin_gate = now;

        SHORT s = GetAsyncKeyState(chuni_io_cfg.vk_coin);
        bool pressed = ((s >> 15) & 1) != 0;

        if (pressed && !chuni_io_coin) {
            chuni_io_coin = true;
            chuni_io_coins++;
            chuni_io_coin_cached = chuni_io_coins;
        } else if (!pressed && chuni_io_coin) {
            chuni_io_coin = false;
        }
    }

    *out = chuni_io_coin_cached;
}

void __cdecl chuni_io_jvs_poll(uint8_t *opbtn, uint8_t *beams)
{
    /* decide the air simulation mode once */
    if (chuni_io_air_mode == 0xFF) {
        if (chuni_io_cfg.vk_ir_emu == 0xAA) {
            chuni_io_air_mode = 2;
            puts("使用IO4模式模拟AIR");
        } else if (chuni_io_cfg.vk_ir_emu != 0) {
            chuni_io_air_mode = 1;
            chuni_io_single_key_vk = chuni_io_cfg.vk_ir_emu;
            printf("使用单击模式模拟AIR: 0x%02X\n", chuni_io_single_key_vk);
        } else {
            chuni_io_air_mode = 0;
            puts("使用键盘模式模拟AIR");
        }
    }

    /* test / service buttons, rate-limited to one poll per 10 ms */
    DWORD now = GetTickCount();
    if (now - chuni_io_jvs_gate > 10) {
        chuni_io_jvs_gate = now;
        chuni_io_opbtn_cached = 0;

        if (GetAsyncKeyState(chuni_io_cfg.vk_test) < 0)
            chuni_io_opbtn_cached |= 0x01;
        if (GetAsyncKeyState(chuni_io_cfg.vk_service) < 0)
            chuni_io_opbtn_cached |= 0x02;
    }
    *opbtn |= chuni_io_opbtn_cached;

    switch (chuni_io_air_mode) {
    case 1: {   /* single-key simulated air */
        if (GetAsyncKeyState(chuni_io_single_key_vk) < 0) {
            if (chuni_io_hand_pos <= 5)
                chuni_io_hand_pos++;
        } else {
            if (chuni_io_hand_pos != 0)
                chuni_io_hand_pos--;
        }
        for (int i = 0; i <= 5; i++) {
            if (chuni_io_hand_pos > i)
                *beams |= (uint8_t)(1 << i);
        }
        break;
    }
    case 2: {   /* io4 HID beams */
        io4_JVS_FSM();
        io4_switches_to_beams(io4_received_data.switches, beams);
        break;
    }
    default:    /* per-beam keyboard bindings */
        for (int i = 0; i <= 5; i++) {
            if (GetAsyncKeyState(chuni_io_cfg.vk_ir[i]) < 0)
                *beams |= (uint8_t)(1 << i);
            else
                *beams &= (uint8_t)~(1 << i);
        }
        break;
    }
}

HRESULT __cdecl chuni_io_slider_init(void)
{
    cursor_init();
    slider_led_tx_set_ready(FALSE);
    connect_serial_port(chuni_io_cfg.vid, chuni_io_cfg.pid,
                        chuni_io_cfg.serial_port, (DWORD)chuni_io_cfg.serial_baud);
    return S_OK;
}

void __cdecl chuni_io_slider_start(chuni_io_slider_callback_t callback)
{
    if (!IsSerialPortOpen()) {
        if (open_port(comPort, (DWORD)chuni_io_cfg.serial_baud))
            report_init();
    } else {
        report_init();
    }
    slider_led_tx_set_ready(IsSerialPortOpen());

    if (chuni_io_slider_thread != NULL)
        return;

    InitializeCriticalSection(&cs);

    Queue *queue = createQueue(0x100);

    typedef struct {
        chuni_io_slider_callback_t callback;
        Queue *queue;
    } slider_ctx_t;

    slider_ctx_t *ctx = (slider_ctx_t *)malloc(sizeof(*ctx));
    ctx->callback = callback;
    ctx->queue    = queue;

    chuni_io_slider_thread =
        (HANDLE)_beginthreadex(NULL, 0, chuni_io_slider_thread_proc, ctx, 0, NULL);
    if (chuni_io_slider_thread != NULL)
        SetThreadPriority(chuni_io_slider_thread, THREAD_PRIORITY_TIME_CRITICAL);
}

void __cdecl chuni_io_slider_stop(void)
{
    slider_led_tx_set_ready(FALSE);
    if (IsSerialPortOpen()) {
        EnterCriticalSection(&serial_control_cs);
        f_disable_slider_report();
        LeaveCriticalSection(&serial_control_cs);
    }

    if (chuni_io_slider_thread == NULL)
        return;

    chuni_io_slider_stop_flag = true;

    if (WaitForSingleObject(chuni_io_slider_thread, 1000) == WAIT_TIMEOUT) {
        /* thread refused to exit — same last-resort as the original */
        TerminateThread(chuni_io_slider_thread, 1);
    }
    CloseHandle(chuni_io_slider_thread);
    chuni_io_slider_thread   = NULL;
    chuni_io_slider_stop_flag = false;
    DeleteCriticalSection(&cs);           /* original does this */
}

void __cdecl chuni_io_slider_set_leds(const uint8_t *rgb)
{
    if (rgb == NULL)
        return;

    LED_status = 1;
    slider_led_tx_queue(rgb);
}

void slider_led_tx_queue(const uint8_t *rgb)
{
    if (rgb == NULL || led_frame_event == NULL)
        return;
    EnterCriticalSection(&led_frame_cs);
    memcpy(led_frame, rgb, sizeof(led_frame));
    led_frame_pending = TRUE;
    LeaveCriticalSection(&led_frame_cs);
    SetEvent(led_frame_event);
}

void slider_led_tx_set_ready(BOOL ready)
{
    EnterCriticalSection(&led_frame_cs);
    led_tx_ready = ready;
    LeaveCriticalSection(&led_frame_cs);
    if (ready && led_frame_event != NULL)
        SetEvent(led_frame_event);
}

static unsigned __stdcall chuni_io_led_tx_thread_proc(void *param)
{
    (void)param;
    for (;;) {
        WaitForSingleObject(led_frame_event, INFINITE);
        for (;;) {
            uint8_t frame[sizeof(led_frame)];
            EnterCriticalSection(&led_frame_cs);
            if (!led_tx_ready || !led_frame_pending) {
                LeaveCriticalSection(&led_frame_cs);
                break;
            }
            memcpy(frame, led_frame, sizeof(frame));
            led_frame_pending = FALSE;
            LeaveCriticalSection(&led_frame_cs);

            EnterCriticalSection(&serial_control_cs);
            if (IsSerialPortOpen())
                f_slider_led_report(frame);
            LeaveCriticalSection(&serial_control_cs);
        }
    }
    return 0;
}

HRESULT __cdecl chuni_io_led_init(void)
{
    return 0;
}

void __cdecl chuni_io_led_set_colors(uint8_t board, uint8_t *rgb)
{
    (void)board;
    (void)rgb;                                 /* no-op, same as original */
}

/* ------------------------------------------------------------------ */
/* io4 beam extraction                                                 */
/* ------------------------------------------------------------------ */
void __cdecl io4_switches_to_beams(const uint16_t *switches, uint8_t *beams)
{
    *beams = 0;
    for (int i = 0; i <= 5; i++) {
        uint16_t word = switches[beam_word_map[i]];
        if ((word >> beam_bit_map[i]) & 1)
            *beams |= (uint8_t)(1 << i);
    }
}

/* ------------------------------------------------------------------ */
BOOL io4_JVS_FSM(void)
{
    /* try to (re)open the HID device at most once every 500 ms */
    if (!IsHidDeviceOpen()) {
        DWORD now = GetTickCount();
        if (now - chuni_io_io4_open_gate >= 500) {
            chuni_io_io4_open_gate = now;
            const char *path = GetHidDevicePathByVidPid(IO4_HID_VID, IO4_HID_PID);
            if (path != NULL && path[0] != '\0') {
                if (open_hid_device(path)) {
                    puts("IO4设备连接成功");
                    log_write("io4 HID opened: %s", path);
                }
            }
        }
    }

    if (chuni_io_io4_thread != NULL) {
        if (WaitForSingleObject(chuni_io_io4_thread, 0) != 0)
            return TRUE;                       /* worker still alive */

        CloseHandle(chuni_io_io4_thread);
        chuni_io_io4_thread = NULL;
        puts("IO4线程已退出，准备重启");
        log_write("io4 worker thread exited, restarting");
    }

    if (!chuni_io_io4_cs_ready) {
        InitializeCriticalSectionAndSpinCount(&io4_cs, 0xfa0);
        chuni_io_io4_cs_ready = true;
    }

    Queue *queue = createQueue(0x100);
    Queue **ctx = (Queue **)malloc(sizeof(Queue *) * 2);
    ctx[1] = queue;

    chuni_io_io4_thread =
        (HANDLE)_beginthreadex(NULL, 0, chuni_io_io4_thread_proc, ctx, 0, NULL);
    if (chuni_io_io4_thread != NULL)
        SetThreadPriority(chuni_io_io4_thread, THREAD_PRIORITY_HIGHEST);

    return TRUE;
}

/* ------------------------------------------------------------------ */
/* worker: slider (serial) thread                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    chuni_io_slider_callback_t callback;
    Queue *queue;
} slider_ctx_t;

unsigned __stdcall chuni_io_slider_thread_proc(void *param)
{
    slider_ctx_t *ctx = (slider_ctx_t *)param;
    chuni_io_slider_callback_t callback = ctx->callback;

    uint8_t *packet = (uint8_t *)_aligned_malloc(0x20, 0x40);
    if (packet == NULL) {
        free(ctx->queue->items);
        free(ctx->queue);
        free(ctx);
        return 1;
    }
    memset(packet, 0, 0x20);

    /* snapshot taken once at thread start, same as the original */
    DWORD reconnect_interval = chuni_io_cfg.reconnect_interval_ms;

    (void)reconnect_interval;

    while (!chuni_io_slider_stop_flag) {
        static int frame;
        keep_awake_tick(&frame, 16);

        process_serial_data(packet,
                            &AIR_status,
                            LED_status,
                            chuni_io_cfg.auto_reconnect,
                            &last_reconnect_attempt,
                            reconnect_interval,
                            chuni_io_cfg.vid,
                            chuni_io_cfg.pid,
                            chuni_io_cfg.serial_port,
                            chuni_io_cfg.serial_baud);

        callback(packet);

    }

    _aligned_free(packet);
    free(ctx->queue->items);
    free(ctx->queue);
    free(ctx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* worker: io4 (HID) thread                                            */
/* ------------------------------------------------------------------ */
unsigned __stdcall chuni_io_io4_thread_proc(void *param)
{
    Queue **ctx = (Queue **)param;

    uint8_t *packet = (uint8_t *)_aligned_malloc(0x20, 0x40);
    if (packet == NULL) {
        free(ctx[0]);
        free(ctx);
        return 1;
    }
    memset(packet, 0, 0x20);

    DWORD reconnect_interval = chuni_io_cfg.reconnect_interval_ms;
    (void)reconnect_interval;

    for (;;) {                                 /* no stop flag, as original */
        static int frame;
        keep_awake_tick(&frame, 10);

        process_io4_data();
    }

    /* unreached (thread is terminated by the process on shutdown) */
    _aligned_free(packet);
    free(ctx[0]);
    free(ctx);
    return 0;
}
