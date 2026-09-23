/* Mitad izquierda: bateria propia + la de la otra mitad (recibida del dongle). */
#include <stdio.h>
#include <string.h>
#include <lvgl.h>
#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);
#include <zmk/battery.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/split/bluetooth/peripheral.h>

/* El dongle nos manda los niveles de sus 2 slots (uno somos nosotros). */
ZMK_RELAY_EVENT_HANDLE(zmk_peripheral_battery_state_changed, pb, );

#define W 32
#define H 128
static lv_color_t cbuf[H * H];
static lv_color_t tmp[H * H];
static lv_obj_t *canvas;

static uint8_t self_lvl;
static int16_t slot_lvl[2] = {-1, -1};

#define BG (IS_ENABLED(CONFIG_G0N_OLED_INVERTED) ? lv_color_black() : lv_color_white())
#define FG (IS_ENABLED(CONFIG_G0N_OLED_INVERTED) ? lv_color_white() : lv_color_black())

static int other_level(void) {
    /* de los 2 slots, el que no coincide con nuestra bateria es la otra mitad */
    int a = slot_lvl[0], b = slot_lvl[1];
    if (a < 0) return b;
    if (b < 0) return a;
    if (abs(a - self_lvl) <= abs(b - self_lvl)) return b;
    return a;
}

static void draw_bat(int y, const char *tag, int lvl) {
    lv_draw_label_dsc_t l; lv_draw_label_dsc_init(&l);
    l.color = FG; l.font = &lv_font_montserrat_12; l.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(canvas, 0, y, W, &l, tag);

    lv_draw_rect_dsc_t box; lv_draw_rect_dsc_init(&box);
    box.bg_opa = LV_OPA_TRANSP; box.border_color = FG; box.border_width = 1;
    lv_canvas_draw_rect(canvas, 3, y + 16, 26, 10, &box);
    if (lvl > 0) {
        lv_draw_rect_dsc_t fill; lv_draw_rect_dsc_init(&fill);
        fill.bg_color = FG;
        lv_canvas_draw_rect(canvas, 5, y + 18, (22 * lvl) / 100 ? (22 * lvl) / 100 : 1, 6, &fill);
    }
    char txt[8];
    if (lvl < 0) snprintf(txt, sizeof(txt), "--");
    else snprintf(txt, sizeof(txt), "%d%%", lvl);
    lv_canvas_draw_text(canvas, 0, y + 28, W, &l, txt);
}

static void redraw(void) {
    lv_canvas_fill_bg(canvas, BG, LV_OPA_COVER);
    lv_draw_label_dsc_t l; lv_draw_label_dsc_init(&l);
    l.color = FG; l.font = &lv_font_montserrat_12; l.align = LV_TEXT_ALIGN_CENTER;
    lv_canvas_draw_text(canvas, 0, 0, W, &l, "g0n");
    lv_draw_line_dsc_t ln; lv_draw_line_dsc_init(&ln); ln.color = FG; ln.width = 1;
    lv_point_t p[2] = {{3, 16}, {28, 16}};
    lv_canvas_draw_line(canvas, p, 2, &ln);

    draw_bat(20, "L", self_lvl);
    draw_bat(66, "R", other_level());

    /* rotar 90 grados: la pantalla fisica es 128x32 montada vertical */
    memcpy(tmp, cbuf, sizeof(tmp));
    lv_img_dsc_t img = {.data = (void *)tmp};
    img.header.cf = LV_IMG_CF_TRUE_COLOR; img.header.w = H; img.header.h = H;
    lv_canvas_fill_bg(canvas, BG, LV_OPA_COVER);
    lv_canvas_transform(canvas, &img, 900, LV_IMG_ZOOM_NONE, -1, 0, H / 2, H / 2, false);
}

struct g0n_state { uint8_t self; int16_t s0, s1; };

static struct g0n_state get_state(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *p =
        eh ? as_zmk_peripheral_battery_state_changed(eh) : NULL;
    if (p && p->source < 2) slot_lvl[p->source] = p->state_of_charge;
    return (struct g0n_state){zmk_battery_state_of_charge(), slot_lvl[0], slot_lvl[1]};
}

static void update(struct g0n_state s) {
    self_lvl = s.self; slot_lvl[0] = s.s0; slot_lvl[1] = s.s1;
    if (canvas) redraw();
}

ZMK_DISPLAY_WIDGET_LISTENER(g0n_batt, struct g0n_state, update, get_state)
ZMK_SUBSCRIPTION(g0n_batt, zmk_battery_state_changed);
ZMK_SUBSCRIPTION(g0n_batt, zmk_peripheral_battery_state_changed);

lv_obj_t *zmk_display_status_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_set_size(box, H, W);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_align(box, LV_ALIGN_TOP_LEFT, 0, 0);
    canvas = lv_canvas_create(box);
    lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_canvas_set_buffer(canvas, cbuf, H, H, LV_IMG_CF_TRUE_COLOR);
    g0n_batt_init();
    return screen;
}
