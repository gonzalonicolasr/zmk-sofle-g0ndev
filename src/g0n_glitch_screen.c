/* Mitad derecha: logo g0ndev limpio, con rafagas de glitch cada tanto (estilo Arasaka). */
#include <stdlib.h>
#include <lvgl.h>
#include <zephyr/kernel.h>

extern const lv_img_dsc_t *g0n_frames[];
extern const int g0n_frame_count;

static lv_obj_t *img;
static int burst_left;

static void tick(lv_timer_t *t) {
    if (burst_left > 0) {
        burst_left--;
        int i = burst_left == 0 ? 0 : 1 + (rand() % (g0n_frame_count - 1));
        lv_img_set_src(img, g0n_frames[i]);
        lv_timer_set_period(t, 80);
        return;
    }
    lv_img_set_src(img, g0n_frames[0]);
    /* limpio entre 2 y 6 segundos, despues una rafaga de 3 a 7 frames */
    lv_timer_set_period(t, 2000 + (rand() % 4000));
    burst_left = 3 + (rand() % 5);
}

lv_obj_t *zmk_display_status_screen(void) {
    srand(k_cycle_get_32());
    lv_obj_t *screen = lv_obj_create(NULL);
    img = lv_img_create(screen);
    lv_img_set_src(img, g0n_frames[0]);
    lv_obj_align(img, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_timer_create(tick, 1500, NULL);
    return screen;
}
