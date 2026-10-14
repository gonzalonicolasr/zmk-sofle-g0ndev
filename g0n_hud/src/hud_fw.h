#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hud_core.h"

#define HUD_FW_WATCHDOG_MS 8000u
#define HUD_FW_WAKE_MS 30000u
#define HUD_FW_BLINK_MS 450u
#define HUD_FW_PATCH_MAX 25

#define HUD_T_PING 0x31
#define HUD_T_PATCH 0x40
#define HUD_T_SHOW 0x41
#define HUD_T_BOOT 0x50
#define HUD_T_VERSION 0x51

#define HUD_LED_DONE 0x20
#define HUD_LED_WAIT 0x21

#define HUD_BOOT_UF2 0x57
#define HUD_BOOT_OTA 0xA8

enum hud_fw_act {
    HUD_ACT_NONE = 0,
    HUD_ACT_REDRAW = 1,
    HUD_ACT_VERSION = 2,
    HUD_ACT_BOOT_UF2 = 3,
    HUD_ACT_BOOT_OTA = 4,
};

struct hud_fw_out {
    bool full;
    bool corner;
    bool wake;
    const uint8_t *fb;
    uint32_t next_ms;
};

extern const uint8_t hud_fw_corner_bits[HUD_CORNER * HUD_CORNER_STRIDE];

void hud_fw_init(uint32_t now);
enum hud_fw_act hud_fw_rx(const uint8_t *rep, int len, uint32_t now);
void hud_fw_led(uint8_t type, uint32_t now);
void hud_fw_tick(uint32_t now, struct hud_fw_out *o);
int hud_fw_version(uint8_t *out, int cap, const char *name, const char *commit);
