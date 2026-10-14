#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/reboot.h>
#include <lvgl.h>

#include <raw_hid/events.h>
#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#if IS_ENABLED(CONFIG_G0N_HUD_LED_FALLBACK)
#include <zmk/endpoints.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
#endif

#include "hud_fw.h"

#define PAL 8
#define IND_COMPOSE 0x08
#define IND_KANA 0x10

#ifndef G0N_HUD_COMMIT
#define G0N_HUD_COMMIT "local"
#endif

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static struct k_spinlock lock;
static uint8_t img_buf[PAL + HUD_FB_SIZE] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff};
static uint8_t corner_buf[PAL + HUD_CORNER * HUD_CORNER_STRIDE] = {0xff, 0xff, 0xff, 0xff,
                                                                  0x00, 0x00, 0x00, 0xff};
static lv_img_dsc_t img_dsc;
static lv_img_dsc_t corner_dsc;
static lv_obj_t *img;
static lv_obj_t *corner_img;
static bool img_shown;
static bool corner_shown;
static bool woke;
static bool started;
static atomic_t flags;
static uint8_t boot_type;

#define F_VERSION 0
#define F_REWAKE 1

static bool may_be_blanked(void) {
    return IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE) &&
           zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE;
}

static void tick_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_cb);

static void reboot_cb(struct k_work *work) { sys_reboot(boot_type); }
static K_WORK_DELAYABLE_DEFINE(reboot_work, reboot_cb);

static void make_objects(void) {
    img_dsc.header.cf = LV_IMG_CF_INDEXED_1BIT;
    img_dsc.header.w = HUD_FBW;
    img_dsc.header.h = HUD_H;
    img_dsc.data_size = sizeof(img_buf);
    img_dsc.data = img_buf;
    corner_dsc.header.cf = LV_IMG_CF_INDEXED_1BIT;
    corner_dsc.header.w = HUD_CORNER;
    corner_dsc.header.h = HUD_CORNER;
    corner_dsc.data_size = sizeof(corner_buf);
    corner_dsc.data = corner_buf;
    memcpy(corner_buf + PAL, hud_fw_corner_bits, sizeof(hud_fw_corner_bits));

    img = lv_img_create(lv_layer_top());
    lv_img_set_src(img, &img_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    corner_img = lv_img_create(lv_layer_top());
    lv_img_set_src(corner_img, &corner_dsc);
    lv_obj_set_pos(corner_img, HUD_W - HUD_CORNER, 0);
    lv_obj_add_flag(corner_img, LV_OBJ_FLAG_HIDDEN);
}

static bool load_diff(const uint8_t *src, lv_area_t *a) {
    uint8_t *fb = img_buf + PAL;
    int y0 = -1, y1 = -1, b0 = HUD_STRIDE, b1 = -1;
    for (int y = 0; y < HUD_H; y++) {
        const uint8_t *r = src + y * HUD_STRIDE;
        uint8_t *p = fb + y * HUD_STRIDE;
        if (memcmp(r, p, HUD_STRIDE) == 0) {
            continue;
        }
        if (y0 < 0) {
            y0 = y;
        }
        y1 = y;
        for (int b = 0; b < HUD_STRIDE; b++) {
            if (r[b] != p[b]) {
                if (b < b0) b0 = b;
                if (b > b1) b1 = b;
            }
        }
        memcpy(p, r, HUD_STRIDE);
    }
    if (y0 < 0) {
        return false;
    }
    a->x1 = b0 * 8;
    a->x2 = b1 * 8 + 7;
    a->y1 = y0;
    a->y2 = y1;
    return true;
}

static void send_version(void) {
    uint8_t rep[HUD_REPORT];
    hud_fw_version(rep, sizeof(rep), CONFIG_G0N_HUD_NAME, G0N_HUD_COMMIT);
    raise_raw_hid_sent_event((struct raw_hid_sent_event){.data = rep, .length = sizeof(rep)});
}

static void tick_cb(struct k_work *work) {
    if (img == NULL) {
        make_objects();
    }
    if (atomic_test_and_clear_bit(&flags, F_VERSION)) {
        send_version();
    }
    if (atomic_test_and_clear_bit(&flags, F_REWAKE)) {
        woke = false;
    }

    struct hud_fw_out o;
    lv_area_t a;
    bool changed = false;
    k_spinlock_key_t key = k_spin_lock(&lock);
    hud_fw_tick(k_uptime_get_32(), &o);
    if (o.full) {
        changed = load_diff(o.fb, &a);
    }
    k_spin_unlock(&lock, key);

    if (o.wake && !woke && may_be_blanked()) {
        display_blanking_off(display);
        woke = true;
    }

    bool dirty = false;
    if (o.full) {
        if (!img_shown) {
            lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
            img_shown = true;
            lv_obj_invalidate(img);
            dirty = true;
        } else if (changed) {
            lv_obj_invalidate_area(img, &a);
            dirty = true;
        }
    } else if (img_shown) {
        lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
        img_shown = false;
        dirty = true;
    }

    bool want_corner = !o.full && o.corner;
    if (want_corner != corner_shown) {
        if (want_corner) {
            lv_obj_clear_flag(corner_img, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(corner_img, LV_OBJ_FLAG_HIDDEN);
        }
        corner_shown = want_corner;
        dirty = true;
    }

    if (dirty) {
        lv_refr_now(NULL);
    }

    if (!o.wake && woke) {
        woke = false;
        if (may_be_blanked()) {
            display_blanking_on(display);
        }
    }

    k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_MSEC(o.next_ms));
}

static void kick(void) {
    if (started) {
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_NO_WAIT);
    }
}

static int hud_rx_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *ev = as_raw_hid_received_event(eh);
    if (ev == NULL || ev->length == 0) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    enum hud_fw_act act = hud_fw_rx(ev->data, ev->length, k_uptime_get_32());
    k_spin_unlock(&lock, key);
    switch (act) {
    case HUD_ACT_REDRAW:
        kick();
        break;
    case HUD_ACT_VERSION:
        atomic_set_bit(&flags, F_VERSION);
        kick();
        break;
    case HUD_ACT_BOOT_UF2:
    case HUD_ACT_BOOT_OTA:
        boot_type = act == HUD_ACT_BOOT_UF2 ? HUD_BOOT_UF2 : HUD_BOOT_OTA;
        k_work_reschedule(&reboot_work, K_MSEC(100));
        break;
    default:
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(g0n_hud_rx, hud_rx_listener);
ZMK_SUBSCRIPTION(g0n_hud_rx, raw_hid_received_event);

static int hud_activity_listener(const zmk_event_t *eh) {
    atomic_set_bit(&flags, F_REWAKE);
    kick();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(g0n_hud_activity, hud_activity_listener);
ZMK_SUBSCRIPTION(g0n_hud_activity, zmk_activity_state_changed);

#if IS_ENABLED(CONFIG_G0N_HUD_LED_FALLBACK)
static int last_idx = -1;
static zmk_hid_indicators_t prev_ind;

static int hud_led_listener(const zmk_event_t *eh) {
    int idx = zmk_endpoint_instance_to_index(zmk_endpoints_selected());
    const struct zmk_hid_indicators_changed *ev = as_zmk_hid_indicators_changed(eh);
    if (ev == NULL || idx != last_idx) {
        last_idx = idx;
        prev_ind = zmk_hid_indicators_get_current_profile();
        return ZMK_EV_EVENT_BUBBLE;
    }
    zmk_hid_indicators_t rise = ev->indicators & ~prev_ind;
    prev_ind = ev->indicators;
    uint8_t type;
    if (rise & IND_KANA) {
        type = HUD_LED_DONE;
    } else if (rise & IND_COMPOSE) {
        type = HUD_LED_WAIT;
    } else {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    hud_fw_led(type, k_uptime_get_32());
    k_spin_unlock(&lock, key);
    kick();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(g0n_hud_led, hud_led_listener);
ZMK_SUBSCRIPTION(g0n_hud_led, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(g0n_hud_led, zmk_endpoint_changed);
#endif

static int boot_tries;

static void boot_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(boot_work, boot_cb);

static void boot_cb(struct k_work *work) {
    if (zmk_display_is_initialized()) {
        k_spinlock_key_t key = k_spin_lock(&lock);
        hud_fw_init(k_uptime_get_32());
        k_spin_unlock(&lock, key);
        started = true;
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_NO_WAIT);
    } else if (++boot_tries < 40) {
        k_work_reschedule(&boot_work, K_MSEC(250));
    }
}

static int hud_init_sys(void) {
    k_work_reschedule(&boot_work, K_MSEC(300));
    return 0;
}

SYS_INIT(hud_init_sys, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
