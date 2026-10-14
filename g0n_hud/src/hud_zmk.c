#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
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

#include "hud_core.h"

#define PAL 8
#define IND_COMPOSE 0x08
#define IND_KANA 0x10

struct rx_item {
    uint8_t len;
    uint8_t data[HUD_REPORT + 3];
};

K_MSGQ_DEFINE(hud_rxq, sizeof(struct rx_item), 24, 4);

static const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));

static uint8_t img_buf[PAL + HUD_FB_SIZE] = {0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0xff};
static uint8_t prev_fb[HUD_FB_SIZE];
static uint8_t corner_buf[PAL + HUD_CORNER * HUD_CORNER_STRIDE] = {0xff, 0xff, 0xff, 0xff,
                                                                  0x00, 0x00, 0x00, 0xff};
static lv_img_dsc_t img_dsc;
static lv_img_dsc_t corner_dsc;
static lv_obj_t *img;
static lv_obj_t *corner_img;
static bool img_shown;
static bool corner_shown;
static int16_t img_x;
static bool woke;
static bool started;

static bool may_be_blanked(void) {
    return IS_ENABLED(CONFIG_ZMK_DISPLAY_BLANK_ON_IDLE) &&
           zmk_activity_get_state() != ZMK_ACTIVITY_ACTIVE;
}

static void tick_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(tick_work, tick_cb);

static void make_objects(void) {
    img_dsc.header.cf = LV_IMG_CF_INDEXED_1BIT;
    img_dsc.header.always_zero = 0;
    img_dsc.header.reserved = 0;
    img_dsc.header.w = HUD_FBW;
    img_dsc.header.h = HUD_H;
    img_dsc.data_size = sizeof(img_buf);
    img_dsc.data = img_buf;
    corner_dsc.header.cf = LV_IMG_CF_INDEXED_1BIT;
    corner_dsc.header.always_zero = 0;
    corner_dsc.header.reserved = 0;
    corner_dsc.header.w = HUD_CORNER;
    corner_dsc.header.h = HUD_CORNER;
    corner_dsc.data_size = sizeof(corner_buf);
    corner_dsc.data = corner_buf;
    memcpy(corner_buf + PAL, hud_corner_bits, sizeof(hud_corner_bits));

    img = lv_img_create(lv_layer_top());
    lv_img_set_src(img, &img_dsc);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);

    corner_img = lv_img_create(lv_layer_top());
    lv_img_set_src(corner_img, &corner_dsc);
    lv_obj_set_pos(corner_img, HUD_W - HUD_CORNER, 0);
    lv_obj_add_flag(corner_img, LV_OBJ_FLAG_HIDDEN);
}

static bool diff_area(lv_area_t *a) {
    const uint8_t *fb = img_buf + PAL;
    int y0 = -1, y1 = -1, b0 = HUD_STRIDE, b1 = -1;
    for (int y = 0; y < HUD_H; y++) {
        const uint8_t *r = fb + y * HUD_STRIDE, *p = prev_fb + y * HUD_STRIDE;
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
    }
    if (y0 < 0) {
        return false;
    }
    a->x1 = img_x + b0 * 8;
    a->x2 = img_x + b1 * 8 + 7;
    a->y1 = y0;
    a->y2 = y1;
    return true;
}

static void tick_cb(struct k_work *work) {
    if (img == NULL) {
        make_objects();
    }
    uint32_t now = k_uptime_get_32();
    struct rx_item it;
    while (k_msgq_get(&hud_rxq, &it, K_NO_WAIT) == 0) {
        if (it.len == 0) {
            hud_push_alert(it.data[0], "", 0);
        } else {
            hud_rx(it.data, it.len, now);
        }
    }
    hud_set_active(!may_be_blanked(), now);

    struct hud_out o;
    hud_tick(now, img_buf + PAL, &o);

    if (o.alert && !woke && may_be_blanked()) {
        display_blanking_off(display);
        woke = true;
    }

    bool dirty = false;
    if (o.full) {
        if (!img_shown) {
            lv_obj_clear_flag(img, LV_OBJ_FLAG_HIDDEN);
            img_shown = true;
            dirty = true;
        }
        if (o.x != img_x) {
            img_x = o.x;
            lv_obj_set_x(img, img_x);
            dirty = true;
        }
        lv_area_t a;
        if (dirty) {
            lv_obj_invalidate(img);
        } else if (diff_area(&a)) {
            lv_obj_invalidate_area(img, &a);
            dirty = true;
        }
        memcpy(prev_fb, img_buf + PAL, HUD_FB_SIZE);
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

    if (!o.alert && woke) {
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
    struct rx_item it = {0};
    it.len = MIN(ev->length, sizeof(it.data));
    memcpy(it.data, ev->data, it.len);
    k_msgq_put(&hud_rxq, &it, K_NO_WAIT);
    kick();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(g0n_hud_rx, hud_rx_listener);
ZMK_SUBSCRIPTION(g0n_hud_rx, raw_hid_received_event);

static int hud_activity_listener(const zmk_event_t *eh) {
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
    struct rx_item it = {0};
    if (rise & IND_KANA) {
        it.data[0] = 0x20;
    } else if (rise & IND_COMPOSE) {
        it.data[0] = 0x21;
    } else {
        return ZMK_EV_EVENT_BUBBLE;
    }
    k_msgq_put(&hud_rxq, &it, K_NO_WAIT);
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
        started = true;
        k_work_reschedule_for_queue(zmk_display_work_q(), &tick_work, K_NO_WAIT);
    } else if (++boot_tries < 40) {
        k_work_reschedule(&boot_work, K_MSEC(250));
    }
}

static int hud_init_sys(void) {
    hud_init();
    k_work_reschedule(&boot_work, K_MSEC(300));
    return 0;
}

SYS_INIT(hud_init_sys, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
