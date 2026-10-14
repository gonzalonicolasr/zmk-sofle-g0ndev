#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HUD_W 128
#define HUD_H 64
#define HUD_FBW 129
#define HUD_STRIDE 17
#define HUD_FB_SIZE (HUD_STRIDE * HUD_H)

#define HUD_CORNER 9
#define HUD_CORNER_STRIDE 2

#define HUD_MAGIC 0x47
#define HUD_REPORT 32
#define HUD_CHUNK 28

enum hud_phase { HUD_PH_HOME, HUD_PH_PAGE, HUD_PH_OUT, HUD_PH_ALERT };

struct hud_anim_step {
    const uint8_t *frame;
    uint16_t ms;
};

struct hud_anim {
    const struct hud_anim_step *steps;
    uint8_t len;
    uint16_t total_ms;
};

struct hud_out {
    bool full;
    int16_t x;
    bool corner;
    bool alert;
    uint32_t next_ms;
    uint8_t phase;
    int8_t page;
    uint8_t alert_type;
};

extern const struct hud_anim hud_anim_done;
extern const struct hud_anim hud_anim_wait;
extern const struct hud_anim hud_anim_splash;
extern const uint8_t hud_font[96][8];
extern const uint8_t hud_corner_bits[HUD_CORNER * HUD_CORNER_STRIDE];

void hud_init(void);
void hud_rx(const uint8_t *report, int len, uint32_t now);
void hud_push_alert(uint8_t type, const char *payload, int len);
void hud_set_active(bool active, uint32_t now);
void hud_tick(uint32_t now, uint8_t *fb, struct hud_out *out);
