#include <string.h>
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "keyboard_indev.h"

/*
 * I2C register map / Ext.Port1 pins: see memory note project_keyboard_protocol.md
 * and the Phase 2 keyboard_spike.c this file supersedes -- unchanged here.
 *
 * HID keymap tables below are ported directly from m5stack/M5Unit-KEYBOARD's
 * unit_Tab5Keyboard.cpp (KEY_MATRIX_HID_BASE / KEY_MATRIX_HID_SYM), which the
 * library states were "captured from device HID-mode logs on real hardware
 * (US ANSI)". We use them to translate Normal mode's press/release + row/col
 * events (which we already read correctly) into standard USB HID Usage Page
 * 0x07 keycodes, then into LV_KEY_x or ASCII for LVGL -- rather than using the
 * keyboard's own HID mode, whose I2C mirror (HID_EVENT, reg 0x30/0x31) turned
 * out not to carry an explicit press/release signal.
 */
#define KBD_I2C_ADDR       0x6D
#define KBD_SDA_GPIO       GPIO_NUM_0
#define KBD_SCL_GPIO       GPIO_NUM_1
#define KBD_INT_GPIO       GPIO_NUM_50

#define REG_INT_STAT       0x01
#define REG_EVENT_NUM      0x02
#define REG_KEY_EVENT      0x20
#define KEY_EVENT_EMPTY    0xFF

#define KBD_ROWS 5
#define KBD_COLS 14

static const char *TAG = "keyboard_indev";

typedef struct {
    uint8_t keycode;  /* USB HID Usage Page 0x07 code; 0 = unmapped (modifier keys) */
    uint8_t modifier; /* firmware-forced shift bit (0x02) for this position */
} hid_mapping_t;

/* Row-major (row*KBD_COLS + col). Row 3 col 0/1 = Sym/Aa, row 4 col 0/1 = Ctrl/Alt
 * -- all four map to {0,0} here and are handled as modifiers below instead. */
static const hid_mapping_t KEY_MATRIX_HID_BASE[KBD_ROWS * KBD_COLS] = {
    /* Row 0: Esc 1 2 3 4 5 6 7 8 9 0 - + Del */
    {0x29, 0x00}, {0x1E, 0x00}, {0x1F, 0x00}, {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00}, {0x23, 0x00},
    {0x24, 0x00}, {0x25, 0x00}, {0x26, 0x00}, {0x27, 0x00}, {0x2D, 0x00}, {0x2E, 0x02}, {0x4C, 0x00},
    /* Row 1: ` ! @ # $ % ^ & * ( ) [ ] \ */
    {0x35, 0x00}, {0x1E, 0x02}, {0x1F, 0x02}, {0x20, 0x02}, {0x21, 0x02}, {0x22, 0x02}, {0x23, 0x02},
    {0x24, 0x02}, {0x25, 0x02}, {0x26, 0x02}, {0x27, 0x02}, {0x2F, 0x00}, {0x30, 0x00}, {0x31, 0x00},
    /* Row 2: Tab q w e r t y u i o p ; ' Backspace */
    {0x2B, 0x00}, {0x14, 0x00}, {0x1A, 0x00}, {0x08, 0x00}, {0x15, 0x00}, {0x17, 0x00}, {0x1C, 0x00},
    {0x18, 0x00}, {0x0C, 0x00}, {0x12, 0x00}, {0x13, 0x00}, {0x33, 0x00}, {0x34, 0x00}, {0x2A, 0x00},
    /* Row 3: Sym Aa a s d f g h j k l Up _ Enter */
    {0x00, 0x00}, {0x00, 0x00}, {0x04, 0x00}, {0x16, 0x00}, {0x07, 0x00}, {0x09, 0x00}, {0x0A, 0x00},
    {0x0B, 0x00}, {0x0D, 0x00}, {0x0E, 0x00}, {0x0F, 0x00}, {0x52, 0x00}, {0x2D, 0x02}, {0x28, 0x00},
    /* Row 4: Ctrl Alt z x c v b n m . Left Down Right Space */
    {0x00, 0x00}, {0x00, 0x00}, {0x1D, 0x00}, {0x1B, 0x00}, {0x06, 0x00}, {0x19, 0x00}, {0x05, 0x00},
    {0x11, 0x00}, {0x10, 0x00}, {0x37, 0x00}, {0x50, 0x00}, {0x51, 0x00}, {0x4F, 0x00}, {0x2C, 0x00},
};

/* Sym-held layer; only entries that differ from base are commented. */
static const hid_mapping_t KEY_MATRIX_HID_SYM[KBD_ROWS * KBD_COLS] = {
    {0x29, 0x00}, {0x1E, 0x00}, {0x1F, 0x00}, {0x20, 0x00}, {0x21, 0x00}, {0x22, 0x00}, {0x23, 0x00},
    {0x24, 0x00}, {0x25, 0x00}, {0x26, 0x00}, {0x27, 0x00}, {0x2D, 0x00}, {0x2E, 0x02}, {0x4C, 0x00},
    {0x35, 0x02} /* `->~ */, {0x38, 0x02} /* !->? */, {0x1F, 0x02}, {0x20, 0x02}, {0x21, 0x02}, {0x22, 0x02},
    {0x23, 0x02}, {0x24, 0x02}, {0x38, 0x00} /* *->/ */, {0x36, 0x02} /* (-> < */, {0x37, 0x02} /* )-> > */,
    {0x2F, 0x02} /* [->{ */, {0x30, 0x02} /* ]->} */, {0x31, 0x02} /* \->| */,
    {0x2B, 0x00}, {0x14, 0x00}, {0x1A, 0x00}, {0x08, 0x00}, {0x15, 0x00}, {0x17, 0x00}, {0x1C, 0x00},
    {0x18, 0x00}, {0x0C, 0x00}, {0x12, 0x00}, {0x13, 0x00}, {0x33, 0x02} /* ;->: */, {0x34, 0x02} /* '->" */,
    {0x2A, 0x00},
    {0x00, 0x00}, {0x00, 0x00}, {0x04, 0x00}, {0x16, 0x00}, {0x07, 0x00}, {0x09, 0x00}, {0x0A, 0x00},
    {0x0B, 0x00}, {0x0D, 0x00}, {0x0E, 0x00}, {0x0F, 0x00}, {0x52, 0x00}, {0x2E, 0x00} /* _->= */, {0x28, 0x00},
    {0x00, 0x00}, {0x00, 0x00}, {0x1D, 0x00}, {0x1B, 0x00}, {0x06, 0x00}, {0x19, 0x00}, {0x05, 0x00},
    {0x11, 0x00}, {0x10, 0x00}, {0x36, 0x00} /* .->, */, {0x50, 0x00}, {0x51, 0x00}, {0x4F, 0x00}, {0x2C, 0x00},
};

/* HID Usage Page 0x07 -> LV_KEY_x or ASCII, US layout. Returns 0 for unmapped. */
static uint32_t hid_keycode_to_lv_key(uint8_t keycode, bool shift)
{
    static const char lower[] = "abcdefghijklmnopqrstuvwxyz";
    static const char upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    if (keycode >= 0x04 && keycode <= 0x1D) {
        return (uint32_t)(shift ? upper[keycode - 0x04] : lower[keycode - 0x04]);
    }
    if (keycode >= 0x1E && keycode <= 0x27) {
        static const char digits[]  = "1234567890";
        static const char shifted[] = "!@#$%^&*()";
        return (uint32_t)(shift ? shifted[keycode - 0x1E] : digits[keycode - 0x1E]);
    }
    switch (keycode) {
    case 0x28: return LV_KEY_ENTER;
    case 0x29: return LV_KEY_ESC;
    case 0x2A: return LV_KEY_BACKSPACE;
    case 0x2B: return LV_KEY_NEXT;
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4C: return LV_KEY_DEL;
    case 0x4F: return LV_KEY_RIGHT;
    case 0x50: return LV_KEY_LEFT;
    case 0x51: return LV_KEY_DOWN;
    case 0x52: return LV_KEY_UP;
    default:   return 0;
    }
}

typedef struct {
    uint32_t key;
    bool pressed;
} kbd_lv_event_t;

static i2c_master_dev_handle_t s_kbd_dev;
static TaskHandle_t s_kbd_task;
static QueueHandle_t s_lv_event_queue;
static bool s_sym_held, s_aa_held, s_ctrl_held, s_alt_held;
static uint32_t s_last_key;
static keyboard_raw_key_cb_t s_raw_cb;

void keyboard_indev_set_raw_cb(keyboard_raw_key_cb_t cb)
{
    ESP_LOGI(TAG, "raw key callback %s", cb != NULL ? "enabled" : "disabled");
    s_raw_cb = cb;
}

static esp_err_t kbd_read_reg(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_kbd_dev, &reg, 1, data, len, 1000);
}

static esp_err_t kbd_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2] = { reg, value };
    return i2c_master_transmit(s_kbd_dev, buf, sizeof(buf), 1000);
}

static void IRAM_ATTR kbd_int_isr(void *arg)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_kbd_task, &higher_priority_task_woken);
    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

static void handle_key_event(uint8_t row, uint8_t col, bool pressed)
{
    /* Modifier keys: track state, don't forward to LVGL themselves. */
    if (row == 3 && col == 0) { s_sym_held = pressed; return; }
    if (row == 3 && col == 1) { s_aa_held = pressed; return; }
    if (row == 4 && col == 0) { s_ctrl_held = pressed; return; }
    if (row == 4 && col == 1) { s_alt_held = pressed; return; }

    if (row >= KBD_ROWS || col >= KBD_COLS) {
        return;
    }

    const hid_mapping_t *map = s_sym_held ? KEY_MATRIX_HID_SYM : KEY_MATRIX_HID_BASE;
    hid_mapping_t m = map[row * KBD_COLS + col];
    if (m.keycode == 0) {
        return; /* shouldn't happen outside the modifier positions already handled above */
    }

    bool shift = (m.modifier & 0x02) != 0 || s_aa_held;
    uint32_t key = hid_keycode_to_lv_key(m.keycode, shift);
    if (key == 0) {
        return;
    }

    /* Raw (terminal) mode wants a literal Tab byte, not LVGL's "move focus
     * to the next widget" meaning for that same physical key. */
    if (s_raw_cb != NULL && key == LV_KEY_NEXT && m.keycode == 0x2B) {
        key = '\t';
    }

    /* Ctrl+letter -> control character (matches terminal convention), since
     * LVGL widgets have no separate "ctrl held" concept for plain keys. */
    if (s_ctrl_held && key >= 'a' && key <= 'z') {
        key = key - 'a' + 1;
    } else if (s_ctrl_held && key >= 'A' && key <= 'Z') {
        key = key - 'A' + 1;
    }

    if (pressed) {
        bool printable = key >= 0x20 && key < 0x7F;
        ESP_LOGI(TAG, "key: 0x%02lX%s%c%s", (unsigned long)key,
                  printable ? " ('" : "", printable ? (char)key : ' ', printable ? "')" : "");
    }

    if (s_raw_cb != NULL) {
        s_raw_cb(key, pressed);
        return;
    }

    kbd_lv_event_t evt = { .key = key, .pressed = pressed };
    xQueueSend(s_lv_event_queue, &evt, 0);
}

static void kbd_drain_events(void)
{
    uint8_t event_num = 0;
    if (kbd_read_reg(REG_EVENT_NUM, &event_num, 1) != ESP_OK) {
        ESP_LOGW(TAG, "failed to read EVENT_NUM");
        return;
    }

    while (event_num > 0) {
        uint8_t key_event = KEY_EVENT_EMPTY;
        if (kbd_read_reg(REG_KEY_EVENT, &key_event, 1) != ESP_OK) {
            ESP_LOGW(TAG, "failed to read KEY_EVENT");
            break;
        }
        if (key_event == KEY_EVENT_EMPTY) {
            break;
        }

        bool pressed = (key_event & 0x80) != 0;
        uint8_t row = (key_event >> 4) & 0x07;
        uint8_t col = key_event & 0x0F;
        handle_key_event(row, col, pressed);

        if (kbd_read_reg(REG_EVENT_NUM, &event_num, 1) != ESP_OK) {
            break;
        }
    }

    /* Explicitly release/re-arm the INT line -- see project_keyboard_protocol.md. */
    kbd_write_reg(REG_INT_STAT, 0x00);
}

static void kbd_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        kbd_drain_events();
    }
}

static void lvgl_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    kbd_lv_event_t evt;
    if (xQueueReceive(s_lv_event_queue, &evt, 0) == pdTRUE) {
        s_last_key = evt.key;
        data->key = evt.key;
        data->state = evt.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
        data->continue_reading = uxQueueMessagesWaiting(s_lv_event_queue) > 0;
    } else {
        data->key = s_last_key;
        data->state = LV_INDEV_STATE_RELEASED;
        data->continue_reading = false;
    }
}

lv_indev_t *keyboard_indev_start(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = KBD_SDA_GPIO,
        .scl_io_num = KBD_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus_handle;
    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus() failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = KBD_I2C_ADDR,
        .scl_speed_hz = 100000,
    };
    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &s_kbd_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device() failed: %s", esp_err_to_name(ret));
        return NULL;
    }

    ret = i2c_master_probe(bus_handle, KBD_I2C_ADDR, 100);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "no response from keyboard at 0x%02X -- is it plugged into Ext.Port1? (%s)",
                  KBD_I2C_ADDR, esp_err_to_name(ret));
        return NULL;
    }
    ESP_LOGI(TAG, "keyboard detected at 0x%02X", KBD_I2C_ADDR);

    s_lv_event_queue = xQueueCreate(32, sizeof(kbd_lv_event_t));
    xTaskCreate(kbd_task, "kbd_task", 4096, NULL, 10, &s_kbd_task);

    gpio_config_t int_cfg = {
        .pin_bit_mask = 1ULL << KBD_INT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    gpio_config(&int_cfg);

    esp_err_t isr_ret = gpio_install_isr_service(0);
    if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "gpio_install_isr_service() failed: %s", esp_err_to_name(isr_ret));
        return NULL;
    }
    gpio_isr_handler_add(KBD_INT_GPIO, kbd_int_isr, NULL);

    /* INT is level-triggered and may already be asserted at startup -- see
     * project_keyboard_protocol.md. */
    if (gpio_get_level(KBD_INT_GPIO) == 0) {
        xTaskNotifyGive(s_kbd_task);
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, lvgl_read_cb);

    ESP_LOGI(TAG, "keyboard indev ready");
    return indev;
}
