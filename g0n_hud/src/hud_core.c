#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hud_core.h"

#define W HUD_W
#define H HUD_H

#define REC_MAX 6
#define FLD_MAX 4
#define TXT_MAX 424
#define PAGES 6
#define ALERTS 4
#define EXPIRE_MS 120000u
#define WIPE_MS 260u
#define TITLE_MS 1800u
#define FRAME_MS 70u
#define LINE 100

enum { F_S, F_B, F_L };
enum { A_L, A_C, A_R };

#define T_SPLASH 0x10
#define T_DONE 0x20
#define T_WAIT 0x21
#define T_MEET 0x22
#define T_UMB 0x23
#define T_CONT 0x24

enum { P_WORK, P_DONE, P_WAIT, P_TODO, P_MEET, P_DAY };

struct recs {
    char txt[TXT_MAX];
    uint8_t n;
    uint8_t nf[REC_MAX];
    uint16_t off[REC_MAX][FLD_MAX];
    uint32_t at;
};

struct alert {
    uint8_t type;
    struct recs d;
};

static uint32_t page_ms[PAGES] = {15000, 13000, 15000, 14000, 12000, 12000};
static const char *page_title[PAGES] = {"TRABAJANDO", "LISTOS",  "TE ESPERAN",
                                              "TUS PENDIENTES", "REUNION", "TU DIA"};
static const char *page_sub[PAGES] = {"trabajando", "sin ver", "te necesitan", "pendientes", "proxima reunion", "resumen de hoy"};

static struct recs pages[PAGES];
static struct alert alerts[ALERTS];
static uint8_t alert_n;
static bool alert_started;
static uint32_t alert_t0;

static struct {
    bool on;
    uint8_t id, type, total;
    uint16_t got;
    char buf[15 * HUD_CHUNK];
} asm_;

static bool rotate = true;
static uint32_t home_ms = 8000;
static bool active = true;
static uint8_t phase = HUD_PH_HOME;
static uint32_t ph_t0;
static int8_t cur_page = -1;
static int8_t last_page = -1;

static uint8_t *g;
static bool xor_mode;
static int cx0, cy0, cx1 = W, cy1 = H;

const uint8_t hud_corner_bits[HUD_CORNER * HUD_CORNER_STRIDE] = {
    0xff, 0x80, 0xf3, 0x80, 0xf3, 0x80, 0xf3, 0x80, 0xf3,
    0x80, 0xf3, 0x80, 0xff, 0x80, 0xf3, 0x80, 0xff, 0x80,
};

static void parse(struct recs *r, const char *p, int len, uint32_t now) {
    if (len > TXT_MAX - 1) {
        len = TXT_MAX - 1;
    }
    int n = 0;
    while (n < len && p[n] != 0) {
        n++;
    }
    memcpy(r->txt, p, n);
    r->txt[n] = 0;
    r->n = 0;
    r->at = now;
    if (n == 0) {
        return;
    }
    int rec = 0, fld = 0;
    r->off[0][0] = 0;
    for (int i = 0; i <= n; i++) {
        char c = r->txt[i];
        if (c != '\t' && c != '\n' && c != 0) {
            continue;
        }
        r->txt[i] = 0;
        if (c == '\t') {
            if (fld + 1 < FLD_MAX) {
                fld++;
                r->off[rec][fld] = i + 1;
            }
            continue;
        }
        r->nf[rec] = fld + 1;
        rec++;
        if (c == 0 || rec >= REC_MAX) {
            break;
        }
        if (i + 1 >= n) {
            break;
        }
        fld = 0;
        r->off[rec][0] = i + 1;
    }
    r->n = rec;
}

static const char *fld(const struct recs *r, int rec, int f) {
    if (rec >= r->n || f >= r->nf[rec]) {
        return "";
    }
    return r->txt + r->off[rec][f];
}

static int num(const char *s) { return atoi(s); }

static bool page_ok(int p, uint32_t now) {
    return pages[p].n > 0 && (uint32_t)(now - pages[p].at) <= EXPIRE_MS;
}

void hud_push_alert(uint8_t type, const char *payload, int len) {
    if (alert_n == ALERTS) {
        alert_n--;
    }
    struct alert *a = &alerts[alert_n++];
    a->type = type;
    parse(&a->d, payload, len, 0);
    if (alert_n == 1) {
        alert_started = false;
        phase = HUD_PH_ALERT;
    }
}

static void dispatch(uint8_t type, const char *p, int len, uint32_t now) {
    if (type >= 0x01 && type <= 0x06) {
        parse(&pages[type - 1], p, len, now);
    } else if (type >= T_DONE && type <= T_CONT) {
        hud_push_alert(type, p, len);
    } else if (type == 0x30) {
        static struct recs c;
        parse(&c, p, len, now);
        if (c.n > 0) {
            rotate = num(fld(&c, 0, 0)) != 0;
            int s = num(fld(&c, 0, 1));
            if (s >= 2 && s <= 120) {
                home_ms = (uint32_t)s * 1000u;
            }
        }
    }
}

void hud_rx(const uint8_t *rep, int len, uint32_t now) {
    if (len > HUD_REPORT && rep[0] == 0 && rep[1] == HUD_MAGIC) {
        rep++;
        len--;
    }
    if (len < 4 || rep[0] != HUD_MAGIC) {
        return;
    }
    uint8_t type = rep[1], id = rep[2], idx = rep[3] >> 4, total = rep[3] & 0x0f;
    if (total == 0 || idx >= total) {
        return;
    }
    if (!asm_.on || asm_.id != id || asm_.type != type || asm_.total != total) {
        asm_.on = true;
        asm_.id = id;
        asm_.type = type;
        asm_.total = total;
        asm_.got = 0;
        memset(asm_.buf, 0, sizeof(asm_.buf));
    }
    int n = len - 4;
    if (n > HUD_CHUNK) {
        n = HUD_CHUNK;
    }
    memcpy(asm_.buf + idx * HUD_CHUNK, rep + 4, n);
    asm_.got |= 1u << idx;
    if (asm_.got == (1u << total) - 1u) {
        asm_.on = false;
        dispatch(type, asm_.buf, total * HUD_CHUNK, now);
    }
}

void hud_set_active(bool a, uint32_t now) {
    if (a == active) {
        return;
    }
    active = a;
    if (phase != HUD_PH_ALERT) {
        phase = HUD_PH_HOME;
        ph_t0 = now;
    }
}

void hud_init(void) {
    memset(pages, 0, sizeof(pages));
    alert_n = 0;
    phase = HUD_PH_HOME;
    hud_push_alert(T_SPLASH, "", 0);
}

static void px(int x, int y) {
    if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) {
        return;
    }
    uint8_t m = 0x80 >> (x & 7);
    uint8_t *b = &g[y * HUD_STRIDE + (x >> 3)];
    if (xor_mode) {
        *b ^= m;
    } else {
        *b |= m;
    }
}

static void fill(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            px(i, j);
        }
    }
}

static void clear_area(int x, int y, int w, int h) {
    for (int j = y; j < y + h; j++) {
        for (int i = x; i < x + w; i++) {
            if (i >= 0 && j >= 0 && i < HUD_FBW && j < H) {
                g[j * HUD_STRIDE + (i >> 3)] &= ~(0x80 >> (i & 7));
            }
        }
    }
}

static void rect(int x, int y, int w, int h) {
    fill(x, y, w, 1);
    fill(x, y + h - 1, w, 1);
    fill(x, y + 1, 1, h - 2);
    fill(x + w - 1, y + 1, 1, h - 2);
}

static void invert(int x, int y, int w, int h) {
    bool o = xor_mode;
    xor_mode = true;
    fill(x, y, w, h);
    xor_mode = o;
}

static void dots(int y, int x0, int x1) {
    for (int x = x0; x < x1; x += 2) {
        px(x, y);
    }
}

static void line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        px(x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y0 += sy;
        }
    }
}

static void poly_fill(const int *xy, int n) {
    int ymin = H, ymax = 0;
    for (int i = 0; i < n; i++) {
        if (xy[2 * i + 1] < ymin) ymin = xy[2 * i + 1];
        if (xy[2 * i + 1] > ymax) ymax = xy[2 * i + 1];
    }
    for (int y = ymin; y <= ymax; y++) {
        int xs[8], k = 0;
        float sy = y + 0.5f;
        for (int i = 0; i < n && k < 8; i++) {
            int ax = xy[2 * i], ay = xy[2 * i + 1];
            int bx = xy[2 * ((i + 1) % n)], by = xy[2 * ((i + 1) % n) + 1];
            if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                xs[k++] = (int)(ax + (sy - ay) * (bx - ax) / (float)(by - ay) + 0.5f);
            }
        }
        for (int a = 0; a < k; a++) {
            for (int b = a + 1; b < k; b++) {
                if (xs[b] < xs[a]) {
                    int t = xs[a];
                    xs[a] = xs[b];
                    xs[b] = t;
                }
            }
        }
        for (int a = 0; a + 1 < k; a += 2) {
            for (int x = xs[a]; x < xs[a + 1]; x++) {
                px(x, y);
            }
        }
    }
}

static void hex(int cx, int cy, int r, bool filled) {
    static const float c6[6] = {1, 0.5f, -0.5f, -1, -0.5f, 0.5f};
    static const float s6[6] = {0, 0.8660254f, 0.8660254f, 0, -0.8660254f, -0.8660254f};
    int xy[12];
    for (int i = 0; i < 6; i++) {
        float x = cx + r * c6[i], y = cy + r * s6[i];
        xy[2 * i] = (int)(x + (x >= 0 ? 0.5f : -0.5f));
        xy[2 * i + 1] = (int)(y + (y >= 0 ? 0.5f : -0.5f));
    }
    if (filled) {
        poly_fill(xy, 6);
    }
    for (int i = 0; i < 6; i++) {
        line(xy[2 * i], xy[2 * i + 1], xy[2 * ((i + 1) % 6)], xy[2 * ((i + 1) % 6) + 1]);
    }
}

static void stripes(int x0, int y0, int w, int h, int off) {
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            if (((x + y + off) >> 2) & 1) {
                px(x, y);
            }
        }
    }
}

static void check(int x, int y) {
    static const int8_t c[7][2] = {{0, 3}, {1, 4}, {2, 5}, {3, 4}, {4, 3}, {5, 2}, {6, 1}};
    for (int i = 0; i < 7; i++) {
        fill(x + c[i][0], y + c[i][1], 1, 2);
    }
}

static void bang(int x, int y) {
    fill(x + 2, y, 2, 5);
    fill(x + 2, y + 6, 2, 1);
}

static int iround(float v) { return v >= 0 ? (int)(v + 0.5f) : -(int)(-v + 0.5f); }

static int adv(int f) { return f == F_L ? 12 : f == F_B ? 7 : 6; }

static int twidth(const char *s, int f) {
    int n = strlen(s);
    if (n == 0) {
        return 0;
    }
    return n * adv(f) - (f == F_L ? 2 : 1);
}

static void glyph(int gi, int x, int y, int f) {
    const uint8_t *rows = hud_font[gi];
    for (int r = 0; r < 8; r++) {
        uint8_t b = rows[r];
        if (!b) {
            continue;
        }
        if (f == F_L) {
            for (int c = 0; c < 5; c++) {
                if (b & (0x10 >> c)) {
                    fill(x + c * 2, y + r * 2, 2, 2);
                }
            }
        } else {
            uint8_t o = (uint8_t)(b << 1);
            uint8_t m = f == F_B ? (uint8_t)(o | ((o >> 1) & ~(o << 1))) : o;
            for (int c = 0; c < 6; c++) {
                if (m & (0x20 >> c)) {
                    px(x + c, y + r);
                }
            }
        }
    }
}

static void text(const char *s, int x, int y, int f, int align) {
    int w = twidth(s, f);
    if (align == A_C) {
        x = x - w / 2;
    } else if (align == A_R) {
        x = x - w + 1;
    }
    const char *slash = strstr(s, "g0ndev");
    for (int i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        int gi = (c >= 32 && c < 127) ? c - 32 : '?' - 32;
        if (slash && s + i == slash + 1) {
            gi = 95;
            slash = strstr(s + i, "g0ndev");
        }
        glyph(gi, x + i * adv(f), y, f);
    }
}

static const char *fit(char *dst, const char *s, int max, int f) {
    if (twidth(s, f) <= max) {
        return s;
    }
    int gap = f == F_L ? 2 : 1;
    int n = (max + gap) / adv(f) - 2;
    if (n < 0) {
        n = 0;
    }
    if (n > LINE - 3) {
        n = LINE - 3;
    }
    memcpy(dst, s, n);
    while (n > 0 && dst[n - 1] == ' ') {
        n--;
    }
    dst[n] = '.';
    dst[n + 1] = '.';
    dst[n + 2] = 0;
    return dst;
}

static int wrap(char out[][LINE], const char *s, int max, int f, int lines) {
    int per = (max + 1) / adv(f);
    if (per > LINE - 1) {
        per = LINE - 1;
    }
    int k = 0;
    const char *p = s;
    while (*p == ' ') {
        p++;
    }
    while (*p && k < lines) {
        int len = strlen(p);
        int take = len;
        if (len > per) {
            take = per;
            while (take > 0 && p[take] != ' ') {
                take--;
            }
            if (take == 0) {
                take = per;
            }
        }
        memcpy(out[k], p, take);
        out[k][take] = 0;
        p += take;
        while (*p == ' ') {
            p++;
        }
        k++;
    }
    if (*p && k == lines) {
        char tmp[LINE * 2];
        snprintf(tmp, sizeof(tmp), "%s %s", out[k - 1], p);
        char f2[LINE];
        const char *r = fit(f2, tmp, max, f);
        snprintf(out[k - 1], LINE, "%s", r);
    }
    return k;
}

static const char *mins(char *dst, const char *s) {
    if (s[0] == 0 || s[0] == '?') {
        return "";
    }
    snprintf(dst, 16, "%sm", s);
    return dst;
}

static void where_line(const char *s, int x, int y, int maxw, int f) {
    char ws[8], b1[LINE];
    const char *sep = strstr(s, " >");
    if (!sep) {
        text(fit(b1, s, maxw, f), x, y, f, A_L);
        return;
    }
    int n = sep - s;
    if (n > 6) {
        n = 6;
    }
    for (int i = 0; i < n; i++) {
        ws[i] = (s[i] >= 'a' && s[i] <= 'z') ? s[i] - 32 : s[i];
    }
    ws[n] = 0;
    const char *tb = sep + 2;
    while (*tb == ' ') {
        tb++;
    }
    int bw = 0;
    if (n > 0) {
        bw = twidth(ws, F_S) + 3;
        fill(x, y - 1, bw, 9);
        xor_mode = !xor_mode;
        text(ws, x + 2, y, F_S, A_L);
        xor_mode = !xor_mode;
        bw += 3;
    }
    if (*tb && maxw - bw > 0) {
        text(fit(b1, tb, maxw - bw, f), x + bw, y, f, A_L);
    }
}

static void marquee(const char *s, int x, int y, int avail, int local) {
    int full = twidth(s, F_S), shift = 0;
    if (full > avail) {
        shift = (local - 600) * 6 / 100;
        if (shift < 0) shift = 0;
        if (shift > full - avail) shift = full - avail;
    }
    cx0 = x;
    cy0 = y;
    cx1 = x + avail;
    cy1 = y + 8;
    text(s, x - shift, y, F_S, A_L);
    cx0 = cy0 = 0;
    cx1 = W;
    cy1 = H;
}

static int todo_ms(const char *s, int avail) {
    int over = twidth(s, F_S) - avail;
    return 2400 + (over > 0 ? over * 1000 / 60 : 0);
}

static void tab(const char *label, const char *right) {
    int w = twidth(label, F_B) + 6;
    fill(0, 0, w, 9);
    for (int i = 0; i < 9; i++) {
        fill(w, i, 9 - i, 1);
    }
    xor_mode = true;
    text(label, 3, 1, F_B, A_L);
    xor_mode = false;
    if (right && right[0]) {
        text(right, W - 1, 1, F_S, A_R);
    }
    dots(10, 0, W);
}

static void draw_work(uint32_t t) {
    const struct recs *r = &pages[P_WORK];
    int n = r->n;
    int np = (n + 2) / 3;
    int pg = (t / 7000) % np;
    char right[24], b2[LINE], m[16];
    if (np > 1) {
        snprintf(right, sizeof(right), "%d  %d/%d", n, pg + 1, np);
    } else {
        snprintf(right, sizeof(right), "%d", n);
    }
    tab("EN CURSO", right);
    static const int8_t sp[8][2] = {{1, 0}, {2, 0}, {2, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}, {0, 0}};
    for (int i = 0; i < 3 && pg * 3 + i < n; i++) {
        int k = pg * 3 + i;
        int y = 13 + i * 17;
        int spin = (t / 120 + i * 2) % 8;
        for (int j = 0; j < 8; j++) {
            if ((j - spin + 8) % 8 < 3) {
                fill(sp[j][0] * 2, y + 1 + sp[j][1] * 2, 2, 2);
            }
        }
        const char *mm = mins(m, fld(r, k, 2));
        int mw = twidth(mm, F_S);
        where_line(fld(r, k, 0), 9, y, W - 13 - mw, F_S);
        text(mm, W - 1, y, F_S, A_R);
        text(fit(b2, fld(r, k, 1), W - 9, F_S), 9, y + 8, F_S, A_L);
    }
}

static void draw_done(uint32_t t) {
    const struct recs *r = &pages[P_DONE];
    char right[24], b2[LINE], m[16];
    snprintf(right, sizeof(right), "%d SIN VER", r->n);
    tab("LISTOS", right);
    for (int i = 0; i < 3 && i < r->n; i++) {
        int t0 = 250 + i * 380;
        int e = (int)t - t0;
        if (e < 0) {
            continue;
        }
        float k = e / 260.0f;
        if (k > 1) {
            k = 1;
        }
        float ease = 1 - (1 - k) * (1 - k) * (1 - k);
        int dx = iround((1 - ease) * W);
        int y = 13 + i * 17;
        const char *mm = mins(m, fld(r, i, 2));
        check(dx + 1, y + 1);
        where_line(fld(r, i, 0), dx + 11, y, W - 16 - twidth(mm, F_S), F_S);
        text(mm, dx + W - 1, y, F_S, A_R);
        text(fit(b2, fld(r, i, 1), W - 11, F_S), dx + 11, y + 8, F_S, A_L);
        if (e > 260 && e < 420) {
            invert(0, y - 1, W, 17);
        }
    }
}

static void draw_wait_page(uint32_t t) {
    const struct recs *r = &pages[P_WAIT];
    char m[16], idx[8], ln[3][LINE];
    int off = t / 40;
    stripes(0, 0, W, 5, off);
    stripes(0, H - 5, W, 5, -off);
    int i = (t / 6000) % r->n;
    int beat = (t / 500) % 2;
    hex(14, 30, beat ? 12 : 11, beat);
    if (beat) {
        xor_mode = true;
    }
    bang(12, 26);
    xor_mode = false;
    fill(30, 7, W - 30, 10);
    xor_mode = true;
    text("TE ESPERAN", 33, 8, F_B, A_L);
    if (r->n > 1) {
        snprintf(idx, sizeof(idx), "%d/%d", i + 1, r->n);
        text(idx, W - 2, 8, F_S, A_R);
    }
    xor_mode = false;
    text(mins(m, fld(r, i, 3)), 14, 47, F_S, A_C);
    where_line(fld(r, i, 0), 31, 20, W - 31, F_S);
    int k = wrap(ln, fld(r, i, 2), W - 31, F_S, 3);
    for (int j = 0; j < k; j++) {
        text(ln[j], 31, 30 + j * 8, F_S, A_L);
    }
}

static void draw_todo_where(uint32_t t, const struct recs *r, int n) {
    char right[32], b1[LINE];
    const int per = 3;
    int avail = W - 10, cyc = 0;
    for (int i = 0; i < n; i++) {
        cyc += todo_ms(fld(r, i, 1), avail);
    }
    int lt = (int)(t % (uint32_t)cyc), cur = 0;
    while (cur < n - 1 && lt >= todo_ms(fld(r, cur, 1), avail)) {
        lt -= todo_ms(fld(r, cur, 1), avail);
        cur++;
    }
    int g0 = cur / per * per, ng = (n + per - 1) / per;
    if (ng > 1) {
        snprintf(right, sizeof(right), "%d  %d/%d", r->n, g0 / per + 1, ng);
    } else {
        snprintf(right, sizeof(right), "%d", r->n);
    }
    tab("PENDIENTES", right);
    for (int i = g0; i < g0 + per && i < n; i++) {
        int y = 13 + (i - g0) * 17;
        const char *s = fld(r, i, 1);
        if (i == cur) {
            fill(1, y + 1, 6, 6);
            where_line(fld(r, i, 0), 10, y, avail, F_S);
            marquee(s, 10, y + 8, avail, lt);
        } else {
            rect(1, y + 1, 6, 6);
            where_line(fld(r, i, 0), 10, y, avail, F_S);
            text(fit(b1, s, avail, F_S), 10, y + 8, F_S, A_L);
        }
    }
}

static void draw_todo(uint32_t t) {
    const struct recs *r = &pages[P_TODO];
    char right[16], b1[LINE];
    int n = r->n < 5 ? r->n : 5;
    if (n > 0 && r->nf[0] >= 2) {
        draw_todo_where(t, r, n);
        return;
    }
    snprintf(right, sizeof(right), "%d PEND.", r->n);
    tab("TUS ORDENES", right);
    int cur = (t / 3000) % n;
    for (int i = 0; i < n; i++) {
        int y = 13 + i * 10;
        const char *s = fld(r, i, 0);
        if (i == cur) {
            fill(0, y - 1, W, 10);
            xor_mode = true;
            rect(1, y + 1, 6, 6);
            int full = twidth(s, F_S), avail = W - 11, local = t % 3000;
            int shift = 0;
            if (full > avail) {
                shift = (local - 300) * 6 / 100;
                if (shift < 0) shift = 0;
                if (shift > full - avail) shift = full - avail;
            }
            cx0 = 10;
            cy0 = y - 1;
            cx1 = 10 + avail;
            cy1 = y + 9;
            text(s, 10 - shift, y, F_S, A_L);
            cx0 = cy0 = 0;
            cx1 = W;
            cy1 = H;
            xor_mode = false;
        } else {
            rect(1, y + 1, 6, 6);
            text(fit(b1, s, W - 11, F_S), 10, y, F_S, A_L);
        }
    }
}

static const char *const SEG[10] = {"abcdef", "bc",    "abdeg", "abcdg",   "bcfg",
                                    "acdfg",  "acdefg", "abc",  "abcdefg", "abcdfg"};

static void seg(int d, int x, int y, int w, int h, int th) {
    const char *on = (d >= 0 && d <= 9) ? SEG[d] : "";
    int m = h / 2;
    if (strchr(on, 'a')) fill(x + th, y, w - 2 * th, th);
    if (strchr(on, 'g')) fill(x + th, y + m - (th >> 1), w - 2 * th, th);
    if (strchr(on, 'd')) fill(x + th, y + h - th, w - 2 * th, th);
    if (strchr(on, 'f')) fill(x, y + th, th, m - th);
    if (strchr(on, 'b')) fill(x + w - th, y + th, th, m - th);
    if (strchr(on, 'e')) fill(x, y + m + 1, th, m - th - 1);
    if (strchr(on, 'c')) fill(x + w - th, y + m + 1, th, m - th - 1);
}

static void draw_meet_page(uint32_t t, uint32_t now) {
    const struct recs *r = &pages[P_MEET];
    char b1[LINE], b2[LINE];
    int32_t left = num(fld(r, 0, 3)) * 1000 - (int32_t)(now - r->at);
    if (left < 0) {
        left = 0;
    }
    int big1, big2, small;
    const char *l1 = "MIN", *l2 = "SEG";
    if (left >= 3600000) {
        big1 = left / 3600000;
        big2 = (left / 60000) % 60;
        small = (left / 1000) % 60;
        l1 = "HRS";
        l2 = "MIN";
    } else {
        big1 = left / 60000;
        big2 = (left / 1000) % 60;
        small = (left / 10) % 100;
    }
    if (big1 > 99) {
        big1 = 99;
    }
    tab("REUNION EN", "T-MINUS");
    int ds[4] = {big1 / 10, big1 % 10, big2 / 10, big2 % 10};
    static const int xs[4] = {0, 18, 41, 59};
    for (int i = 0; i < 4; i++) {
        seg(ds[i], xs[i], 13, 15, 27, 3);
    }
    if ((t / 500) % 2 == 0) {
        fill(36, 19, 3, 3);
        fill(36, 31, 3, 3);
    }
    seg(small / 10, 80, 25, 9, 15, 2);
    seg(small % 10, 92, 25, 9, 15, 2);
    text(l1, 104, 14, F_S, A_L);
    text(l2, 104, 22, F_S, A_L);
    dots(43, 0, W);
    const char *where = fld(r, 0, 2);
    text(fld(r, 0, 0), 0, 46, F_B, A_L);
    text(fit(b1, fld(r, 0, 1), W - 42 - twidth(where, F_S), F_B), 37, 46, F_B, A_L);
    text(where, W - 1, 46, F_S, A_R);
    if (r->n > 1) {
        char nx[LINE * 2];
        snprintf(nx, sizeof(nx), "luego %s %s", fld(r, 1, 0), fld(r, 1, 1));
        text(fit(b2, nx, W, F_S), 0, 55, F_S, A_L);
    }
}

static void draw_day(uint32_t t) {
    const struct recs *r = &pages[P_DAY];
    char b1[LINE], v[12];
    tab("HOY", "MAGI LOG");
    int total = num(fld(r, 0, 0));
    float grow = t / 900.0f;
    if (grow > 1) {
        grow = 1;
    }
    snprintf(v, sizeof(v), "%02d", iround(total * grow));
    text(v, 0, 13, F_L, A_L);
    text("LISTAS", 0, 29, F_S, A_L);
    text("AGENTES", 0, 43, F_S, A_L);
    text(fld(r, 0, 1), 0, 52, F_B, A_L);
    fill(42, 12, 1, 49);
    int mx = 1;
    for (int i = 1; i < r->n && i <= 4; i++) {
        int n = num(fld(r, i, 1));
        if (n > mx) mx = n;
    }
    for (int i = 1; i < r->n && i <= 4; i++) {
        int y = 13 + (i - 1) * 12;
        int n = num(fld(r, i, 1));
        text(fit(b1, fld(r, i, 0), 40, F_S), 46, y, F_S, A_L);
        float k = ((int)t - (i - 1) * 150) / 700.0f;
        if (k < 0) k = 0;
        if (k > 1) k = 1;
        int bw = iround(30.0f * n / mx * (1 - (1 - k) * (1 - k) * (1 - k)));
        for (int x = 0; x < bw; x++) {
            if (x % 3 != 2) {
                fill(87 + x, y + 1, 1, 6);
            }
        }
        snprintf(v, sizeof(v), "%d", n);
        text(v, W - 1, y, F_S, A_R);
    }
}

static void card_done(uint32_t t, const struct recs *d) {
    char ln[2][LINE], tk[32];
    fill(0, 0, W, 10);
    xor_mode = true;
    check(2, 1);
    text("LISTO", 12, 1, F_B, A_L);
    text(fld(d, 0, 0), W - 2, 1, F_S, A_R);
    xor_mode = false;
    where_line(fld(d, 0, 1), 0, 13, W, F_B);
    int k = wrap(ln, fld(d, 0, 2), W, F_S, 2);
    for (int i = 0; i < k; i++) {
        text(ln[i], 0, 23 + i * 9, F_S, A_L);
    }
    dots(43, 0, W);
    const char *tm = fld(d, 0, 3);
    if (tm[0] && tm[0] != '?') {
        snprintf(tk, sizeof(tk), "tardo %sm", tm);
        text(tk, 0, 47, F_S, A_L);
    }
    fill(0, 56, W, 8);
    xor_mode = true;
    text("g0ndev // MAGI", W / 2, 56, F_S, A_C);
    xor_mode = false;
}

static void card_wait(uint32_t t, uint32_t since, const struct recs *d) {
    char ln[3][LINE], ago[24];
    fill(0, 0, W, 10);
    xor_mode = true;
    bang(2, 1);
    text("TE NECESITA", 11, 1, F_B, A_L);
    text(fld(d, 0, 0), W - 2, 1, F_S, A_R);
    xor_mode = false;
    if ((t / 300) % 2 == 0 && t < 2400) {
        invert(0, 0, W, 10);
    }
    where_line(fld(d, 0, 1), 0, 13, W, F_B);
    rect(0, 23, W, 31);
    int k = wrap(ln, fld(d, 0, 2), W - 6, F_S, 3);
    for (int i = 0; i < k; i++) {
        text(ln[i], 3, 26 + i * 9, F_S, A_L);
    }
    uint32_t s = since / 1000;
    if (s < 60) {
        snprintf(ago, sizeof(ago), "hace %us", (unsigned)s);
    } else {
        snprintf(ago, sizeof(ago), "hace %um", (unsigned)(s / 60));
    }
    text(ago, 0, 56, F_S, A_L);
    text("g0ndev", W - 1, 56, F_S, A_R);
}

static void card_meet(uint32_t t, const struct recs *d) {
    char b1[LINE], b2[LINE * 2], b3[LINE];
    hex(15, 30, 13, false);
    text("5'", 15, 22, F_L, A_C);
    text("REUNION", 33, 6, F_L, A_L);
    text(fit(b1, fld(d, 0, 1), 92, F_B), 34, 28, F_B, A_L);
    const char *where = fld(d, 0, 2);
    snprintf(b2, sizeof(b2), where[0] ? "%s - %s" : "%s", fld(d, 0, 0), where);
    text(fit(b3, b2, W - 34, F_S), 34, 38, F_S, A_L);
    dots(50, 0, W);
    text("g0ndev // MAGI", W / 2, 54, F_S, A_C);
    if ((t / 400) % 2 == 0 && t < 3200) {
        invert(0, 0, W, H);
    }
}

static int ease_i(int e, int dur, int span) {
    if (e <= 0) return 0;
    if (e >= dur) return span;
    float k = (float)e / dur;
    return iround(span * (1 - (1 - k) * (1 - k) * (1 - k)));
}

static uint32_t rnd_state;
static int rnd(int n) {
    rnd_state = rnd_state * 1103515245u + 12345u;
    return (int)((rnd_state >> 16) % (uint32_t)n);
}

static void flash(uint32_t t) {
    if (t < 60 || t >= 100) {
        fill(0, 0, W, H);
    }
}

static void bezier(int x0, int y0, int xc, int yc, int x1, int y1, int th) {
    int px0 = x0, py0 = y0;
    for (int i = 1; i <= 24; i++) {
        float k = i / 24.0f, a = (1 - k) * (1 - k), b = 2 * k * (1 - k), c = k * k;
        int x = iround(a * x0 + b * xc + c * x1), y = iround(a * y0 + b * yc + c * y1);
        for (int o = -(th / 2); o <= th / 2; o++) {
            line(px0, py0 + o, x, y + o);
        }
        px0 = x;
        py0 = y;
    }
}

static void umbilical(uint32_t t, const struct recs *d) {
    if (t < 160) {
        flash(t);
        return;
    }
    int pct = num(fld(d, 0, 0));
    if (pct < 0) pct = 0;
    if (pct > 999) pct = 999;
    if (t < 1700) {
        int e = t - 160;
        text("UMBILICAL CABLE", W / 2, 1, F_B, A_C);
        dots(10, 0, W);
        rect(100, 21, 24, 28);
        rect(102, 23, 20, 24);
        fill(104, 29, 8, 3);
        fill(104, 38, 8, 3);
        stripes(114, 24, 7, 22, e / 40);
        int plug = 86, drop = 0, sag = 3;
        if (e < 500) {
            if (e > 200) plug += ((e / 50) % 2) ? 1 : -1;
        } else {
            plug -= ease_i(e - 500, 380, 44);
            drop = ease_i(e - 500, 500, 12);
            sag = 3 + ease_i(e - 500, 500, 14);
        }
        bezier(0, 35, plug / 2, 35 + sag, plug, 35 + drop, 3);
        fill(plug, 28 + drop, 12, 15);
        clear_area(plug + 2, 31 + drop, 8, 2);
        clear_area(plug + 2, 38 + drop, 8, 2);
        fill(plug + 12, 29 + drop, 6, 3);
        fill(plug + 12, 38 + drop, 6, 3);
        if (e >= 500 && e < 900) {
            rnd_state = e / 40 + 7;
            for (int i = 0; i < 26; i++) {
                int ang = rnd(9) - 4;
                int len = rnd(9);
                px(99 - len, 35 + ang * len / 3);
            }
        }
        if (e < 500) {
            text("CONNECTED", W / 2, 54, F_S, A_C);
        } else if ((e / 250) % 2 == 0) {
            fill(0, 52, W, 11);
            xor_mode = true;
            text("DISCONNECTED", W / 2, 54, F_B, A_C);
            xor_mode = false;
        } else {
            text("DISCONNECTED", W / 2, 54, F_B, A_C);
        }
        if (e >= 500 && e < 570) {
            invert(0, 0, W, H);
        }
        return;
    }
    int e = t - 1700;
    char v[8], b[24];
    fill(0, 0, W, 10);
    xor_mode = true;
    text("INTERNAL BATTERY", W / 2, 1, F_B, A_C);
    xor_mode = false;
    if (e < 900 && (e / 150) % 2) {
        invert(0, 0, W, 10);
    }
    int val = ease_i(e, 700, pct);
    bool show = pct < 90 || e < 700 || (e / 300) % 2 == 0;
    int nd = val >= 100 ? 3 : 2;
    int x = 0;
    if (show) {
        int dg[3] = {val / 100, (val / 10) % 10, val % 10};
        for (int i = 3 - nd; i < 3; i++) {
            seg(dg[i], x, 14, 15, 27, 3);
            x += 18;
        }
        text("%", x + 1, 27, F_L, A_L);
    }
    text("CUOTA", 78, 14, F_S, A_L);
    text("CLAUDE", 78, 23, F_B, A_L);
    snprintf(b, sizeof(b), "VENT %s", fld(d, 0, 1));
    text(b, 78, 33, F_S, A_L);
    rect(0, 45, W, 9);
    for (int k = 1; k < 4; k++) {
        px(k * W / 4, 44);
        px(k * W / 4, 54);
    }
    int bw = (W - 4) * (val > 100 ? 100 : val) / 100;
    stripes(2, 47, bw, 5, e / 30);
    fill(2, 47, bw, 1);
    fill(2, 51, bw, 1);
    snprintf(v, sizeof(v), "%d", pct > 100 ? 0 : 100 - pct);
    snprintf(b, sizeof(b), "RESTA %s%%", v);
    text(b, 0, 56, F_S, A_L);
    text("g0ndev", W - 1, 56, F_S, A_R);
}

static void contamination(uint32_t t, const struct recs *d) {
    if (t < 160) {
        flash(t);
        return;
    }
    int e = t - 160;
    int ram = num(fld(d, 0, 0)), swp = num(fld(d, 0, 1));
    const char *psi = fld(d, 0, 2);
    if (ram < 0) ram = 0;
    if (ram > 100) ram = 100;
    if (swp < 0) swp = 0;
    if (swp > 100) swp = 100;
    int worst = ram > swp ? ram : swp;
    int lv = 1 + worst / 25;
    if (lv > 5) lv = 5;
    bool crit = worst >= 85;
    char b[24];
    stripes(0, 0, W, 5, e / 40);
    fill(0, 6, W, 10);
    xor_mode = true;
    text("CONTAMINATION", 3, 7, F_B, A_L);
    snprintf(b, sizeof(b), "LV %d", lv);
    text(b, W - 2, 7, F_S, A_R);
    xor_mode = false;
    if (crit && (e / 250) % 2) {
        invert(0, 6, W, 10);
    }
    const int vals[2] = {ram, swp};
    const char *labs[2] = {"RAM", "SWP"};
    for (int i = 0; i < 2; i++) {
        int x = 2 + i * 20;
        rect(x, 18, 14, 38);
        int h = ease_i(e - 150 - i * 220, 1200, 36 * vals[i] / 100);
        for (int k = 0; k < h; k++) {
            if (k % 3 != 2) {
                fill(x + 2, 54 - k, 10, 1);
            }
        }
        text(labs[i], x - 1, 57, F_S, A_L);
    }
    for (int k = 0; k <= 4; k++) {
        fill(38, 19 + k * 9, 2, 1);
    }
    int vr = ease_i(e - 150, 1200, ram), vs = ease_i(e - 370, 1200, swp);
    text("RAM", 46, 19, F_S, A_L);
    snprintf(b, sizeof(b), "%d%%", vr);
    text(b, W - 1, 19, F_B, A_R);
    text("SWAP", 46, 29, F_S, A_L);
    snprintf(b, sizeof(b), "%d%%", vs);
    text(b, W - 1, 29, F_B, A_R);
    text("PSI", 46, 39, F_S, A_L);
    text(psi[0] ? psi : "-", W - 1, 39, F_B, A_R);
    dots(48, 44, W);
    const char *st = worst >= 85 ? "CRITICO" : worst >= 70 ? "ALERTA" : "NORMAL";
    if (crit) {
        fill(44, 51, W - 44, 11);
        xor_mode = true;
        text(st, (44 + W) / 2, 53, F_B, A_C);
        xor_mode = false;
        if ((e / 250) % 2) {
            invert(44, 51, W - 44, 11);
        }
    } else {
        text(st, (44 + W) / 2, 53, F_B, A_C);
    }
}

static void corner(uint32_t now) {
    fill(W - 9, 0, 9, 9);
    xor_mode = true;
    bang(W - 7, 1);
    xor_mode = false;
    (void)now;
}

static int avail_pages(uint32_t now, int8_t *list) {
    int n = 0;
    for (int p = 0; p < PAGES; p++) {
        if (page_ok(p, now)) {
            list[n++] = p;
        }
    }
    return n;
}

static void pager(uint32_t now) {
    int8_t list[PAGES];
    int n = avail_pages(now, list);
    for (int k = 0; k < n; k++) {
        int x = W - 4 - (n - 1 - k) * 4;
        if (list[k] == cur_page) {
            fill(x, H - 2, 3, 2);
        } else {
            px(x + 1, H - 1);
        }
    }
}

static int next_page(uint32_t now) {
    for (int k = 1; k <= PAGES; k++) {
        int p = (last_page + k + PAGES) % PAGES;
        if (page_ok(p, now)) {
            return p;
        }
    }
    return -1;
}

static void draw_page(int p, uint32_t t, uint32_t now) {
    switch (p) {
    case P_WORK: draw_work(t); break;
    case P_DONE: draw_done(t); break;
    case P_WAIT: draw_wait_page(t); break;
    case P_TODO: draw_todo(t); break;
    case P_MEET: draw_meet_page(t, now); break;
    case P_DAY: draw_day(t); break;
    }
}

static int wipe_x(uint32_t t) { return (int)(t * (W + 24) / WIPE_MS) - 24; }

static void wipe_title(int x, uint32_t t, const char *title) {
    char b1[LINE];
    clear_area(x, 0, 24, H);
    int sx = x < 0 ? 0 : x;
    int sw = 24 + (x < 0 ? x : 0);
    if (sw > W - sx) sw = W - sx;
    if (sw > 0) stripes(sx, 0, sw, H, t / 10);
    if (x + 24 < W) {
        clear_area(x + 24, 0, HUD_FBW - x - 24, H);
        int mw = W - x - 30;
        if (mw > 0) text(fit(b1, title, mw, F_B), x + 28, 28, F_B, A_L);
    }
}

static void title_card(int p, uint32_t t) {
    char b1[LINE];
    stripes(0, 0, W, 7, (int)(t / 25));
    stripes(0, H - 7, W, 7, -(int)(t / 25));
    const char *ti = page_title[p];
    text(ti, W / 2, 16, twidth(ti, F_L) <= W ? F_L : F_B, A_C);
    if (p == P_MEET || p == P_DAY) {
        text(page_sub[p], W / 2, 41, F_S, A_C);
    } else {
        snprintf(b1, sizeof(b1), "%d %s", pages[p].n, page_sub[p]);
        text(b1, W / 2, 41, F_S, A_C);
    }
    if (t < 210) {
        invert(0, 0, W, H);
    }
}

static uint32_t alert_parts(const struct alert *a, const struct hud_anim **anim, uint32_t *card) {
    *anim = NULL;
    *card = 0;
    bool data = a->d.n > 0;
    switch (a->type) {
    case T_SPLASH: *anim = &hud_anim_splash; break;
    case T_DONE: *anim = &hud_anim_done; *card = data ? 15000 : 0; break;
    case T_WAIT: *anim = &hud_anim_wait; *card = data ? 20000 : 0; break;
    case T_MEET: *card = 12000; break;
    case T_UMB: *card = 9000; break;
    case T_CONT: *card = 9000; break;
    }
    return (*anim ? (*anim)->total_ms : 0) + *card;
}

static void pop_alert(void) {
    memmove(&alerts[0], &alerts[1], sizeof(alerts[0]) * (alert_n - 1));
    alert_n--;
    alert_started = false;
}

static uint32_t draw_alert(uint32_t now, uint8_t *fb) {
    const struct alert *a = &alerts[0];
    const struct hud_anim *anim;
    uint32_t card;
    alert_parts(a, &anim, &card);
    uint32_t e = now - alert_t0;
    uint32_t am = anim ? anim->total_ms : 0;
    if (e < am) {
        uint32_t acc = 0;
        for (int i = 0; i < anim->len; i++) {
            acc += anim->steps[i].ms;
            if (e < acc) {
                memcpy(fb, anim->steps[i].frame, HUD_FB_SIZE);
                return acc - e;
            }
        }
    }
    uint32_t t = e - am;
    switch (a->type) {
    case T_DONE: card_done(t, &a->d); break;
    case T_WAIT: card_wait(t, e, &a->d); break;
    case T_MEET: card_meet(t, &a->d); break;
    case T_UMB: umbilical(t, &a->d); break;
    case T_CONT: contamination(t, &a->d); break;
    }
    return FRAME_MS;
}

void hud_tick(uint32_t now, uint8_t *fb, struct hud_out *o) {
    g = fb;
    xor_mode = false;
    memset(fb, 0, HUD_FB_SIZE);
    memset(o, 0, sizeof(*o));
    o->page = -1;
    bool waiting = page_ok(P_WAIT, now);

    for (;;) {
        if (phase == HUD_PH_ALERT) {
            if (alert_n == 0) {
                phase = HUD_PH_HOME;
                ph_t0 = now;
                continue;
            }
            if (!alert_started) {
                alert_started = true;
                alert_t0 = now;
            }
            const struct hud_anim *an;
            uint32_t card;
            uint32_t total = alert_parts(&alerts[0], &an, &card);
            if (now - alert_t0 >= total) {
                pop_alert();
                continue;
            }
            o->alert_type = alerts[0].type;
            o->next_ms = draw_alert(now, fb);
            o->full = true;
            o->alert = true;
            break;
        }
        uint32_t e = now - ph_t0;
        if (phase == HUD_PH_HOME) {
            if (active && rotate && e >= home_ms) {
                int p = next_page(now);
                if (p >= 0) {
                    phase = HUD_PH_PAGE;
                    cur_page = p;
                    last_page = p;
                    ph_t0 = now;
                    continue;
                }
            }
            if (waiting) {
                o->corner = (now / 450) % 2 == 0;
                o->next_ms = 450 - now % 450;
            } else {
                o->next_ms = 1000;
            }
            if (active && rotate && e < home_ms && home_ms - e < o->next_ms) {
                o->next_ms = home_ms - e;
            }
            break;
        }
        if (phase == HUD_PH_PAGE) {
            if (!active || !rotate) {
                phase = HUD_PH_HOME;
                ph_t0 = now;
                continue;
            }
            if (!page_ok(cur_page, now) || e >= page_ms[cur_page] + TITLE_MS) {
                phase = HUD_PH_OUT;
                ph_t0 = now;
                continue;
            }
            if (e < TITLE_MS) {
                title_card(cur_page, e);
                if (e < WIPE_MS) {
                    wipe_title(wipe_x(e), e, page_title[cur_page]);
                }
                o->full = true;
                o->next_ms = FRAME_MS;
                break;
            }
            draw_page(cur_page, e - TITLE_MS, now);
            if (cur_page != P_WAIT && waiting && (now / 450) % 2 == 0) {
                corner(now);
            }
            if (cur_page != P_WAIT && cur_page != P_TODO && cur_page != P_MEET) {
                pager(now);
            }
            o->full = true;
            o->next_ms = FRAME_MS;
            break;
        }
        if (phase == HUD_PH_OUT) {
            if (e >= WIPE_MS || !active) {
                phase = HUD_PH_HOME;
                ph_t0 = now;
                continue;
            }
            o->x = wipe_x(e);
            wipe_title(0, e, "INICIO");
            o->full = true;
            o->next_ms = 40;
            break;
        }
        phase = HUD_PH_HOME;
    }
    o->phase = phase;
    o->page = phase == HUD_PH_PAGE || phase == HUD_PH_OUT ? cur_page : -1;
}
