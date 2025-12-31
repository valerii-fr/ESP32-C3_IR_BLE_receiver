// main.c — IR Capture Bridge (ESP32-C3) using ESP-IDF + NimBLE + RMT RX
//
// Behavior:
//  - Always listens TSOP via RMT RX
//  - For each received "frame" (ended by idle gap), prints durations[] to UART
//  - If BLE connected AND client subscribed to notifications, sends the same frame via BLE notify (chunked)
//
// Assumptions:
//  - TSOP output is demodulated digital signal (active-low mark is typical).
//  - We do NOT decode protocol; we only ship RAW mark/space timings in microseconds.
//  - No START_CAPTURE command.
//  - BLE: no pairing/bonding/security.
//
// Build notes:
//  - Enable NimBLE in menuconfig: Component config -> Bluetooth -> Bluetooth -> NimBLE
//  - Disable Bluedroid if enabled.
//  - Set CPU freq / logging as you like.
//
// Tested API style matches ESP-IDF v5.x RMT new driver + NimBLE host.

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"

#include "driver/rmt_rx.h"
#include "driver/gpio.h"

// NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nimble/ble.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "IR_BLE";

// ----------------------- User config -----------------------
#define IR_GPIO                 GPIO_NUM_4     // <-- set your TSOP output pin
#define RMT_RESOLUTION_HZ       (1000000)      // 1 tick = 1 us
#define RMT_MEM_BLOCK_SYMBOLS   (64)          // symbols per receive batch (tune)
#define MAX_DURATIONS           (1024)         // max [mark,space,...] entries per frame

#define GAP_THRESHOLD_US        (10000)        // 10ms - end-of-frame gqap
#define MIN_PULSE_US            (20)           // ignore shorter pulses as noise (optional)

#define BLE_DEVICE_NAME         "IR-Capture-Bridge"

// Notify chunking
#define BLE_NOTIFY_MTU_SAFE_PAYLOAD (180)      // conservative payload bytes for notify
// Frame payload format (simple):
//  BEGIN:  [0xA0][frame_id][count_lo][count_hi][unit=1]
//  CHUNK:  [0xA1][frame_id][seq][len][data...]
//  END:    [0xA2][frame_id][chunks_total]
//
// durations are uint16 little-endian microseconds.

static uint8_t frame_id = 0;
static uint8_t g_addr_type;

// ----------------------- RMT RX -----------------------
typedef struct {
    size_t num_symbols;
    rmt_symbol_word_t *symbols;  // points to buffer we own
} rmt_frame_event_t;

static rmt_channel_handle_t rx_chan = NULL;
static QueueHandle_t rmt_evt_queue = NULL;

// RMT receive buffer (symbols)
static rmt_symbol_word_t rmt_rx_buffer[RMT_MEM_BLOCK_SYMBOLS];

static void init_nvs(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(err);
    }
}

// Convert RMT symbols -> durations[]
static size_t symbols_to_durations_us(const rmt_symbol_word_t *syms, size_t n,
                                      uint16_t *out, size_t out_cap) {
    // Each symbol has two halves: (level0,duration0) and (level1,duration1)
    // duration is in ticks of resolution_hz => here 1 tick = 1 us
    size_t k = 0;

    for (size_t i = 0; i < n; i++) {
        uint32_t d0 = syms[i].duration0;
        uint32_t d1 = syms[i].duration1;

        // Optional noise filter
        if (d0 >= MIN_PULSE_US) {
            if (k < out_cap) out[k++] = (d0 > 0xFFFF) ? 0xFFFF : (uint16_t)d0;
        }
        if (d1 >= MIN_PULSE_US) {
            if (k < out_cap) out[k++] = (d1 > 0xFFFF) ? 0xFFFF : (uint16_t)d1;
        }
        if (k >= out_cap) break;
    }
    return k;
}

static bool on_rmt_rx_done_cb(rmt_channel_handle_t channel,
                             const rmt_rx_done_event_data_t *edata,
                             void *user_data) {
    // Called in ISR context.
    // We push an event to a queue; main task will parse+log+BLE.
    BaseType_t hp_task_woken = pdFALSE;

    rmt_frame_event_t evt = {
        .num_symbols = edata->num_symbols,
        .symbols = (rmt_symbol_word_t *)edata->received_symbols, // points into our buffer
    };

    xQueueSendFromISR(rmt_evt_queue, &evt, &hp_task_woken);

    return (hp_task_woken == pdTRUE);
}

static esp_err_t rmt_rx_start_once(void) {
    // Configure receive: treat "very long pulse / idle" as end-of-frame using signal_range_max_ns
    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = (MIN_PULSE_US * 10ULL),
        // Setting max pulse range to GAP threshold => when idle/pulse exceeds this, reception completes
        .signal_range_max_ns = ((uint64_t)GAP_THRESHOLD_US * 1000ULL),
    };

    // (Re)start receive into our buffer
    return rmt_receive(rx_chan, rmt_rx_buffer, sizeof(rmt_rx_buffer), &rx_cfg);
}

// ----------------------- BLE (NimBLE) -----------------------
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_attr_handle_notify = 0;
static bool g_notify_enabled = false;

// 128-bit UUIDs (random example)
static const ble_uuid128_t UUID_SVC = BLE_UUID128_INIT(
    0x7b,0x21,0x8c,0x2a,0x9e,0x52,0x4f,0x1f,0x9b,0x0c,0x6a,0x33,0xd5,0x11,0x0e,0x90
);
static const ble_uuid128_t UUID_CHR_NOTIFY = BLE_UUID128_INIT(
    0x7b,0x21,0x8c,0x2a,0x9e,0x52,0x4f,0x1f,0x9b,0x0c,0x6a,0x33,0xd5,0x11,0x0e,0x91
);

static int gatt_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
    // Notify-only characteristic: no direct read/write needed.
    // But NimBLE requires an access callback; return not supported for reads.
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &UUID_CHR_NOTIFY.u,
                .access_cb = gatt_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &g_attr_handle_notify,
            },
            {0} // end
        },
    },
    {0} // end
};

static void ble_advertise(void);

static int ble_gap_event_cb(struct ble_gap_event *event, void *arg) {
    (void)arg;

    switch (event->type) {
        case BLE_GAP_EVENT_SUBSCRIBE: {
            ESP_LOGI(TAG,
                "SUBSCRIBE: conn=%u attr=%u reason=%u prev_n=%u cur_n=%u prev_i=%u cur_i=%u",
                event->subscribe.conn_handle,
                event->subscribe.attr_handle,
                event->subscribe.reason,
                event->subscribe.prev_notify,
                event->subscribe.cur_notify,
                event->subscribe.prev_indicate,
                event->subscribe.cur_indicate);

            if (event->subscribe.attr_handle == g_attr_handle_notify) {
                g_notify_enabled = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "Notify enabled=%d", (int)g_notify_enabled);
            }
            return 0;
        }

        case BLE_GAP_EVENT_CONNECT: {
            if (event->connect.status == 0) {
                g_conn_handle = event->connect.conn_handle;
                g_notify_enabled = false;
                ESP_LOGI(TAG, "BLE connected (handle=%u)", g_conn_handle);
            } else {
                ESP_LOGW(TAG, "BLE connect failed status=%d", event->connect.status);
                ble_advertise();
            }
            return 0;
        }
        case BLE_GAP_EVENT_DISCONNECT: {
            ESP_LOGI(TAG, "BLE disconnected (reason=%d)", event->disconnect.reason);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            g_notify_enabled = false;
            ble_advertise();
            return 0;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE: {
            ble_advertise();
            return 0;
        }
        default:
            return 0;
    }
}

static void ble_advertise(void) {
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields sr_fields;

    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    fields.uuids128 = (ble_uuid128_t *)&UUID_SVC;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields rc=%d", rc);
        return;
    }

    // --- Scan response: имя (и при желании tx power) ---
    memset(&sr_fields, 0, sizeof(sr_fields));
    const char *name = ble_svc_gap_device_name();
    sr_fields.name = (uint8_t *)name;
    sr_fields.name_len = (uint8_t)strlen(name);
    sr_fields.name_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&sr_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_rsp_set_fields rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(g_addr_type, NULL, BLE_HS_FOREVER, &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start rc=%d", rc);
        return;
    }

    ESP_LOGI(TAG, "BLE advertising started (addr_type=%u)", (unsigned)g_addr_type);
}

static void ble_on_sync(void) {
    int rc = ble_hs_id_infer_auto(0, &g_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto rc=%d", rc);
        return;
    }

    uint8_t addr_val[6] = {0};
    ble_hs_id_copy_addr(g_addr_type, addr_val, NULL);
    ESP_LOGI(TAG, "BLE addr: %02x:%02x:%02x:%02x:%02x:%02x (type=%u)",
             addr_val[5], addr_val[4], addr_val[3], addr_val[2], addr_val[1], addr_val[0],
             (unsigned)g_addr_type);

    ble_advertise();
}

static void ble_host_task(void *param) {
    (void)param;
    nimble_port_run(); // returns only when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

// ----------------------- BLE send frame -----------------------
static uint16_t le16(uint16_t v) { return v; } // little-endian on ESP32

static void ble_notify_bytes(const uint8_t *data, size_t len) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_enabled) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        ESP_LOGW(TAG, "ble_hs_mbuf_from_flat failed");
        return;
    }
    int rc = ble_gatts_notify_custom(g_conn_handle, g_attr_handle_notify, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify rc=%d", rc);
    }
}

static void ble_send_frame(uint8_t fid, const uint16_t *durations, uint16_t count) {
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || !g_notify_enabled) return;

    // BEGIN
    uint8_t begin[1 + 1 + 2 + 1];
    begin[0] = 0xA0;
    begin[1] = fid;
    begin[2] = (uint8_t)(count & 0xFF);
    begin[3] = (uint8_t)((count >> 8) & 0xFF);
    begin[4] = 1; // unit = microseconds
    ble_notify_bytes(begin, sizeof(begin));

    // CHUNKS (pack uint16 LE)
    const size_t max_data = BLE_NOTIFY_MTU_SAFE_PAYLOAD - 4; // type+fid+seq+len
    uint8_t seq = 0;

    size_t idx = 0;
    uint8_t chunk[BLE_NOTIFY_MTU_SAFE_PAYLOAD];

    while (idx < count) {
        chunk[0] = 0xA1;
        chunk[1] = fid;
        chunk[2] = seq++;

        size_t payload_bytes = 0;
        size_t out_pos = 4;

        while (idx < count) {
            if (payload_bytes + 2 > max_data) break;
            uint16_t v = le16(durations[idx++]);
            chunk[out_pos++] = (uint8_t)(v & 0xFF);
            chunk[out_pos++] = (uint8_t)((v >> 8) & 0xFF);
            payload_bytes += 2;
        }

        chunk[3] = (uint8_t)payload_bytes;
        ble_notify_bytes(chunk, out_pos);
    }

    // END
    uint8_t end[1 + 1 + 1];
    end[0] = 0xA2;
    end[1] = fid;
    end[2] = seq; // chunks_total
    ble_notify_bytes(end, sizeof(end));
}

// ----------------------- Main worker task -----------------------
static void print_frame_uart(uint8_t fid, const uint16_t *durations, size_t count) {
    ESP_LOGI(TAG, "IR frame #%u: count=%u", fid, (unsigned)count);

    // Print durations in a readable array form (avoid too much per-line spam if huge)
    // This runs in task context (not ISR), safe.
    printf("durations[%u] = [", (unsigned)count);
    for (size_t i = 0; i < count; i++) {
        printf("%u", (unsigned)durations[i]);
        if (i + 1 < count) printf(", ");
        // Optional: wrap lines
        if ((i % 32) == 31 && i + 1 < count) printf("\n  ");
    }
    printf("]\n");
}

static void ir_ble_worker_task(void *arg) {
    (void)arg;

    uint16_t durations[MAX_DURATIONS];

    // Start first receive
    ESP_ERROR_CHECK(rmt_rx_start_once());

    while (true) {
        rmt_frame_event_t evt;
        if (xQueueReceive(rmt_evt_queue, &evt, portMAX_DELAY) == pdTRUE) {
            // Convert symbols -> durations
            size_t count = symbols_to_durations_us(evt.symbols, evt.num_symbols,
                                                  durations, MAX_DURATIONS);
            if (count == 0) {
                // Restart RX and continue
                ESP_ERROR_CHECK(rmt_rx_start_once());
                continue;
            }

            // UART log (always)
            uint8_t fid = frame_id++;
            ESP_LOGI(TAG, "IR activity detected; symbols=%u => durations=%u",
                     (unsigned)evt.num_symbols, (unsigned)count);
            print_frame_uart(fid, durations, count);

            // BLE notify if connected+subscribed
            if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE && g_notify_enabled) {
                ESP_LOGI(TAG, "Sending frame #%u via BLE (conn=%u)", fid, g_conn_handle);
                ble_send_frame(fid, durations, (uint16_t)count);
            } else {
                ESP_LOGI(TAG, "BLE not ready (connected=%d notify=%d) => skip sending",
                         (g_conn_handle != BLE_HS_CONN_HANDLE_NONE), (int)g_notify_enabled);
            }

            // Restart RX for next frame
            esp_err_t err = rmt_rx_start_once();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "rmt_rx_start_once failed: %s", esp_err_to_name(err));
                vTaskDelay(pdMS_TO_TICKS(200));
                // try again
                ESP_ERROR_CHECK(rmt_rx_start_once());
            }
        }
    }
}

// ----------------------- app_main -----------------------
void app_main(void) {
    ESP_LOGI(TAG, "IR Capture Bridge starting...");
    init_nvs();

    // Queue for RMT events
    rmt_evt_queue = xQueueCreate(8, sizeof(rmt_frame_event_t));
    if (!rmt_evt_queue) {
        ESP_LOGE(TAG, "Failed to create queue");
        return;
    }

    // Configure RMT RX channel
    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = IR_GPIO,
        .mem_block_symbols = RMT_MEM_BLOCK_SYMBOLS,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .flags = {
            .invert_in = 0, // set 1 if your TSOP polarity requires it
            .with_dma = 0,
        },
    };

    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_cfg, &rx_chan));

    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = on_rmt_rx_done_cb,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_chan, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(rx_chan));

    // NimBLE init
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_our_key_dist = 0;
    ble_hs_cfg.sm_their_key_dist = 0;
    ble_svc_gap_init();
    ble_svc_gatt_init();

    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg rc=%d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    ESP_LOGI(TAG, "Notify val_handle=%u", g_attr_handle_notify);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs rc=%d", rc);
        return;
    }

    // Start NimBLE host
    nimble_port_freertos_init(ble_host_task);

    // Start worker that logs and pushes frames to BLE
    xTaskCreate(ir_ble_worker_task, "ir_ble_worker", 1024*32, NULL, 10, NULL);

    ESP_LOGI(TAG, "Init done");
}
