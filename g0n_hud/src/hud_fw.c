#include <string.h>

#include "hud_fw.h"

const uint8_t hud_fw_corner_bits[HUD_CORNER * HUD_CORNER_STRIDE] = {
    0xff, 0x80, 0xf3, 0x80, 0xf3, 0x80, 0xf3, 0x80, 0xf3,
    0x80, 0xf3, 0x80, 0xff, 0x80, 0xf3, 0x80, 0xff, 0x80,
};

static uint8_t back[HUD_FB_SIZE];
static uint8_t front[HUD_FB_SIZE];
static bool visible;
static bool bang;
static bool host;
static uint32_t last_rx;
static bool waking;
static uint32_t wake_until;
static const struct hud_anim *anim;
static uint32_t anim_t0;

static void wake_for(uint32_t now, uint32_t ms) {
    uint32_t until = now + ms;
    if (!waking || (int32_t)(until - wake_until) > 0) {
        wake_until = until;
    }
    waking = true;
}

static void play(const struct hud_anim *a, uint32_t now) {
    anim = a;
    anim_t0 = now;
    wake_for(now, a->total_ms);
}

void hud_fw_init(uint32_t now) {
    memset(back, 0, sizeof(back));
    memset(front, 0, sizeof(front));
    visible = bang = host = waking = false;
    play(&hud_anim_splash, now);
}

void hud_fw_led(uint8_t type, uint32_t now) {
    if (type == HUD_LED_DONE) {
        play(&hud_anim_done, now);
    } else if (type == HUD_LED_WAIT) {
        play(&hud_anim_wait, now);
    }
}

enum hud_fw_act hud_fw_rx(const uint8_t *rep, int len, uint32_t now) {
    if (len >= 2 && rep[0] == 0 && rep[1] == HUD_MAGIC) {
        rep++;
        len--;
    }
    if (len < 4 || rep[0] != HUD_MAGIC) {
        return HUD_ACT_NONE;
    }
    host = true;
    last_rx = now;
    const uint8_t *p = rep + 4;
    int n = len - 4;
    switch (rep[1]) {
    case HUD_T_PATCH: {
        if (n < 3) {
            return HUD_ACT_NONE;
        }
        uint32_t off = p[0] | ((uint32_t)p[1] << 8);
        uint32_t cnt = p[2];
        if (cnt > HUD_FW_PATCH_MAX || (int)cnt > n - 3 || off + cnt > HUD_FB_SIZE) {
            return HUD_ACT_NONE;
        }
        memcpy(back + off, p + 3, cnt);
        return HUD_ACT_NONE;
    }
    case HUD_T_SHOW: {
        if (n < 3) {
            return HUD_ACT_NONE;
        }
        uint8_t f = p[2];
        memcpy(front, back, HUD_FB_SIZE);
        visible = f & 1;
        bang = (f & 4) != 0;
        if (f & 2) {
            wake_for(now, HUD_FW_WAKE_MS);
        }
        return HUD_ACT_REDRAW;
    }
    case HUD_T_BOOT:
        if (n < 5 || memcmp(p, "BOOT", 4) != 0) {
            return HUD_ACT_NONE;
        }
        if (p[4] == 0) {
            return HUD_ACT_BOOT_UF2;
        }
        if (p[4] == 1) {
            return HUD_ACT_BOOT_OTA;
        }
        return HUD_ACT_NONE;
    case HUD_T_VERSION:
        return HUD_ACT_VERSION;
    default:
        return HUD_ACT_NONE;
    }
}

static uint32_t at_most(uint32_t a, uint32_t b) { return b < a ? b : a; }

void hud_fw_tick(uint32_t now, struct hud_fw_out *o) {
    memset(o, 0, sizeof(*o));
    o->next_ms = 1000;

    if (host && (uint32_t)(now - last_rx) >= HUD_FW_WATCHDOG_MS) {
        host = false;
        visible = false;
        bang = false;
    }
    if (waking && (int32_t)(now - wake_until) >= 0) {
        waking = false;
    }
    o->wake = waking;
    if (waking) {
        o->next_ms = at_most(o->next_ms, wake_until - now);
    }
    if (host) {
        o->next_ms = at_most(o->next_ms, HUD_FW_WATCHDOG_MS - (now - last_rx));
    }

    if (anim) {
        uint32_t e = now - anim_t0, acc = 0;
        for (int i = 0; i < anim->len; i++) {
            acc += anim->steps[i].ms;
            if (e < acc) {
                o->full = true;
                o->fb = anim->steps[i].frame;
                o->next_ms = at_most(o->next_ms, acc - e);
                return;
            }
        }
        anim = NULL;
    }

    if (visible) {
        o->full = true;
        o->fb = front;
        return;
    }
    if (bang) {
        o->corner = (now / HUD_FW_BLINK_MS) % 2 == 0;
        o->next_ms = at_most(o->next_ms, HUD_FW_BLINK_MS - now % HUD_FW_BLINK_MS);
    }
}

int hud_fw_version(uint8_t *out, int cap, const char *name, const char *commit) {
    memset(out, 0, cap);
    if (cap < 2) {
        return 0;
    }
    out[0] = HUD_MAGIC;
    out[1] = HUD_T_VERSION;
    int k = 2;
    const char *parts[3] = {name, "g0n-hud v2", commit};
    for (int i = 0; i < 3; i++) {
        const char *s = parts[i];
        if (s == NULL || s[0] == 0) {
            continue;
        }
        if (k > 2 && k < cap) {
            out[k++] = ' ';
        }
        while (*s && k < cap) {
            out[k++] = (uint8_t)*s++;
        }
    }
    return k;
}
