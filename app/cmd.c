/**
  * cmd.c - host-configurable pedal binding parser (contract: cmd.h).
  *
  * Pure C11, <stdint.h> + "cmd.h": no libc, no dynamic memory, non-blocking.
  * clear out[] -> copy request to a local line (skip one leading report-ID
  * 0x00, stop at the first NUL) -> split on whitespace -> case-insensitive
  * command dispatch -> validate -> mutate state -> '\n'-terminated reply.
  */
#include <stdint.h>
#include "cmd.h"

static cmd_pedal_t s_ped[CMD_KEY_COUNT];
static uint8_t s_dirty;                  /* RAM differs from the last save */

volatile uint32_t cmd_count;

/* ------------------------------------------------------------- words/split */

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static char to_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c; }

/* Case-insensitive equality of two NUL-terminated words. */
static int word_eq(const char *a, const char *b) {
    while (*a != '\0' && to_upper(*a) == to_upper(*b)) { a++; b++; }
    return *a == '\0' && *b == '\0';
}

/* Split line on whitespace in place; store up to max tokens and return the
 * (possibly larger) token count so callers can reject extras. */
static uint8_t split(char *line, char *tok[], uint8_t max) {
    uint8_t n = 0;
    char *p = line;

    while (*p != '\0') {
        while (is_space(*p)) { p++; }
        if (*p == '\0') { break; }
        if (n < max) { tok[n] = p; }
        n++;
        while (*p != '\0' && !is_space(*p)) { p++; }
        if (*p != '\0') { *p++ = '\0'; }
    }
    return n;
}

/* --------------------------------------------------------------- responses */

/* Append s at out[pos]; never writes past CMD_FEATURE_LEN. */
static uint8_t out_put(uint8_t *out, uint8_t pos, const char *s) {
    while (*s != '\0' && pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)(*s++); }
    return pos;
}

/* Append v as canonical upper-case two-digit hex "0xNN". */
static uint8_t out_hex(uint8_t *out, uint8_t pos, uint8_t v) {
    static const char digits[] = "0123456789ABCDEF";

    pos = out_put(out, pos, "0x");
    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)digits[(v >> 4) & 0x0FU]; }
    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)digits[v & 0x0FU]; }
    return pos;
}

/* Append v as decimal digits (no padding). */
static uint8_t out_dec(uint8_t *out, uint8_t pos, uint16_t v) {
    char buf[6];
    uint8_t i = 0;

    do {
        buf[i++] = (char)('0' + (char)(v % 10u));
        v = (uint16_t)(v / 10u);
    } while (v != 0u);
    while (i > 0u && pos < CMD_FEATURE_LEN) { i--; out[pos++] = (uint8_t)buf[i]; }
    return pos;
}

static void put_err(uint8_t *out, const char *msg) {
    uint8_t pos = out_put(out, 0, "ERR ");

    pos = out_put(out, pos, msg);
    (void)out_put(out, pos, "\n");
}

static void resp_set(uint8_t *out, uint8_t n, uint8_t code) {
    uint8_t pos = out_put(out, 0, "OK SET ");

    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)('0' + n); }
    pos = out_hex(out, out_put(out, pos, "="), code);
    (void)out_put(out, pos, "\n");
}

/* One macro step in canonical wire form: bare "KK" when the mask is zero,
 * else "MM:KK" (upper-case hex). */
static uint8_t out_step(uint8_t *out, uint8_t pos, uint8_t mask, uint8_t key) {
    static const char digits[] = "0123456789ABCDEF";

    if (mask != 0u) {
        if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)digits[(mask >> 4) & 0x0FU]; }
        if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)digits[mask & 0x0FU]; }
        pos = out_put(out, pos, ":");
    }
    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)digits[(key >> 4) & 0x0FU]; }
    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)digits[key & 0x0FU]; }
    return pos;
}

/* "OK MAP <p> <p> <p>\n", <p> = single binding 0xNN or "M" (macro mode). */
static void resp_map(uint8_t *out) {
    uint8_t pos = out_put(out, 0, "OK MAP");
    uint8_t i;

    for (i = 0; i < CMD_KEY_COUNT; i++) {
        pos = out_put(out, pos, " ");
        if (s_ped[i].mode == CMD_MODE_MACRO) {
            pos = out_put(out, pos, "M");
        } else {
            pos = out_hex(out, pos, s_ped[i].code);
        }
    }
    (void)out_put(out, pos, "\n");
}

/* "OK MAC <n> <steps>\n" - the stored step list, or "-" when none is
 * stored.  Independent of the active mode (see cmd.h).  Worst case:
 * 9 + 6*5 + 1 = 45 bytes, fits the report. */
static void resp_mac(uint8_t *out, uint8_t n) {
    const cmd_pedal_t *p = &s_ped[n - 1u];
    uint8_t pos = out_put(out, 0, "OK MAC ");
    uint8_t s;

    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)('0' + n); }
    pos = out_put(out, pos, " ");
    if (p->step_count == 0u) {
        pos = out_put(out, pos, "-");
    } else {
        for (s = 0; s < p->step_count; s++) {
            if (s > 0u) { pos = out_put(out, pos, ","); }
            pos = out_step(out, pos, p->mod_mask[s], p->keycode[s]);
        }
    }
    (void)out_put(out, pos, "\n");
}

/* "OK MTIM <n> <press> <gap>\n" - worst case 9 + 4 + 1 + 4 + 1 = 21 bytes. */
static void resp_mtim(uint8_t *out, uint8_t n) {
    const cmd_pedal_t *p = &s_ped[n - 1u];
    uint8_t pos = out_put(out, 0, "OK MTIM ");

    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)('0' + n); }
    pos = out_put(out, pos, " ");
    pos = out_dec(out, pos, p->press_ms);
    pos = out_put(out, pos, " ");
    pos = out_dec(out, pos, p->gap_ms);
    (void)out_put(out, pos, "\n");
}

/* The documented INFO text "OK INFO fw=0.0.2 keys=<p>,<p>,<p> dirty=N\n" is
 * at most 45 bytes and fits the CMD_FEATURE_LEN (48) byte transport;
 * out_put() never writes past the report size. */
static void resp_info(uint8_t *out) {
    uint8_t pos = out_put(out, 0, "OK INFO fw=0.0.2 keys=");
    uint8_t i;

    for (i = 0; i < CMD_KEY_COUNT; i++) {
        if (i > 0U) { pos = out_put(out, pos, ","); }
        if (s_ped[i].mode == CMD_MODE_MACRO) {
            pos = out_put(out, pos, "M");
        } else {
            pos = out_hex(out, pos, s_ped[i].code);
        }
    }
    pos = out_put(out, pos, " dirty=");
    if (pos < CMD_FEATURE_LEN) { out[pos++] = (uint8_t)(s_dirty ? '1' : '0'); }
    (void)out_put(out, pos, "\n");
}

/* --------------------------------------------------------------- arguments */

/* Exactly one decimal digit 0..9. */
static int parse_pedal(const char *t, uint8_t *n) {
    if (t[0] < '0' || t[0] > '9' || t[1] != '\0') { return 0; }
    *n = (uint8_t)(t[0] - '0');
    return 1;
}

static int hex_digit(char c, uint8_t *v) {
    if (c >= '0' && c <= '9') { *v = (uint8_t)(c - '0'); return 1; }
    if (c >= 'a' && c <= 'f') { *v = (uint8_t)(c - 'a' + 10); return 1; }
    if (c >= 'A' && c <= 'F') { *v = (uint8_t)(c - 'A' + 10); return 1; }
    return 0;
}

/* Parse 1-2 hex digits with an optional "0x"/"0X" prefix from the segment
 * [s, e).  Rejects empty segments, non-hex characters and trailing garbage. */
static int parse_hex_seg(const char *s, const char *e, uint8_t *v) {
    uint8_t nd = 0;
    uint8_t val = 0;

    if ((e - s) >= 3 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { s += 2; }
    while (s < e && nd < 2u) {
        uint8_t d;

        if (hex_digit(*s, &d) == 0) { return 0; }
        val = (uint8_t)((uint8_t)(val << 4) | d);
        nd++;
        s++;
    }
    if (nd == 0u || s != e) { return 0; }
    *v = val;
    return 1;
}

/* End of the NUL-terminated segment at s, capped at 8 chars: longer tails
 * can never parse as a hex field anyway (parse_hex_seg requires s == e). */
static const char *seg_end(const char *s) {
    uint8_t n = 0;

    while (s[n] != '\0' && n < 8u) { n++; }
    return &s[n];
}

/* Parse one step token: "KK" (bare key, mask 00) or "MM:KK" (chord).
 * Each field is 1-2 hex digits with an optional 0x/0X prefix. */
static int parse_step(const char *t, uint8_t *mask, uint8_t *key) {
    uint8_t n = 0;

    while (t[n] != '\0' && t[n] != ':') {
        n++;
        if (n > 4u) { return 0; }   /* mask field holds at most "0xFF" */
    }
    if (n == 0u) { return 0; }      /* empty first field (":KK") */

    if (t[n] == '\0') {
        *mask = 0u;
        return parse_hex_seg(t, &t[n], key);
    }
    if (parse_hex_seg(t, &t[n], mask) == 0) { return 0; }
    return parse_hex_seg(&t[n + 1u], seg_end(&t[n + 1u]), key);
}

/* 0x00 disabled; 0x04..0x65 keys; 0xE0..0xE7 modifiers; else out of range. */
uint8_t cmd_code_ok(uint8_t code) {
    return (uint8_t)(code == 0x00U || (code >= 0x04U && code <= 0x65U) ||
                     (code >= 0xE0U && code <= 0xE7U));
}

/* Mask: any byte (bit b = modifier 0xE0+b).  Key: 0x00 (modifier-only step)
 * or 0x04..0x65; modifier codes are expressed via the mask, never as a key.
 * Both zero is not a step. */
uint8_t cmd_step_ok(uint8_t mask, uint8_t key) {
    if (key != 0x00U && (key < 0x04U || key > 0x65U)) { return 0u; }
    return (uint8_t)(mask != 0x00U || key != 0x00U);
}

/* Parse the comma-separated <steps> token in place (commas become NULs) into
 * mask[]/key[]; *count receives 1..CMD_MACRO_MAX_STEPS.
 * Returns 0 = OK, 1 = syntax error (malformed token), 2 = range error (too
 * many steps, or a step rejected by cmd_step_ok()). */
static int parse_steps(char *steps, uint8_t *mask, uint8_t *key, uint8_t *count) {
    char *p = steps;
    uint8_t n = 0;
    uint8_t more = 1;

    while (more != 0u) {
        char *seg = p;
        uint8_t m = 0;
        uint8_t k = 0;

        while (*p != '\0' && *p != ',') { p++; }
        more = (uint8_t)(*p == ',');
        if (more != 0u) { *p = '\0'; p++; }

        if (seg[0] == '\0') { return 1; }            /* empty segment */
        if (parse_step(seg, &m, &k) == 0) { return 1; }
        if (cmd_step_ok(m, k) == 0u) { return 2; }
        if (n >= CMD_MACRO_MAX_STEPS) { return 2; }  /* 7th step and beyond */
        mask[n] = m;
        key[n] = k;
        n++;
    }

    *count = n;
    return 0;
}

/* Decimal milliseconds into *v.  Returns 0 = OK, 1 = not a decimal number
 * (syntax), 2 = out of range (0, or above CMD_MACRO_TIME_MAX). */
static int parse_time(const char *t, uint16_t *v) {
    uint32_t val = 0;
    uint8_t nd = 0;

    while (*t != '\0') {
        if (*t < '0' || *t > '9') { return 1; }
        if (val <= CMD_MACRO_TIME_MAX) {
            val = val * 10u + (uint32_t)(*t - '0');
        }
        nd++;
        t++;
        if (nd > 5u) { return 2; }   /* obviously above 9999 */
    }
    if (nd == 0u) { return 1; }
    if (val == 0u || val > CMD_MACRO_TIME_MAX) { return 2; }
    *v = (uint16_t)val;
    return 0;
}

/* -------------------------------------------------------------- public API */

/* One pedal back to factory defaults: single mode, default code, default
 * timing, no steps. */
static void pedal_defaults(uint8_t i) {
    static const uint8_t def_code[CMD_KEY_COUNT] = { 0x1EU, 0x1FU, 0x20U };
    uint8_t s;

    s_ped[i].mode = CMD_MODE_SINGLE;
    s_ped[i].code = def_code[i];
    s_ped[i].press_ms = CMD_MACRO_PRESS_DEFAULT;
    s_ped[i].gap_ms = CMD_MACRO_GAP_DEFAULT;
    s_ped[i].step_count = 0u;
    for (s = 0u; s < CMD_MACRO_MAX_STEPS; s++) {
        s_ped[i].mod_mask[s] = 0u;
        s_ped[i].keycode[s] = 0u;
    }
}

void cmd_init(void) {
    uint8_t i;

    for (i = 0u; i < CMD_KEY_COUNT; i++) { pedal_defaults(i); }
    cmd_count = 0U;
    s_dirty = 0U;
}

/* Error codes used by the MAC/MTIM argument checks below. */
#define ERR_NONE   0u
#define ERR_SYNTAX 1u
#define ERR_PEDAL  2u
#define ERR_RANGE  3u

static void put_err_code(uint8_t *out, uint8_t err) {
    switch (err) {
    case ERR_SYNTAX: put_err(out, "syntax"); break;
    case ERR_PEDAL:  put_err(out, "pedal");  break;
    default:         put_err(out, "range");  break;
    }
}

uint8_t cmd_handle(const uint8_t in[CMD_FEATURE_LEN], uint8_t out[CMD_FEATURE_LEN]) {
    char line[CMD_FEATURE_LEN + 1];
    char *tok[5];
    uint8_t argc;
    uint8_t i;
    uint8_t k;

    /* Never leak stale report data, and count every report, valid or not. */
    for (i = 0; i < CMD_FEATURE_LEN; i++) { out[i] = 0U; }
    cmd_count++;

    i = (in[0] == 0x00U) ? 1U : 0U;          /* skip report-ID artefact */
    k = 0U;
    while (i < CMD_FEATURE_LEN && in[i] != 0x00U && k < CMD_FEATURE_LEN) {
        line[k++] = (char)in[i++];
    }
    line[k] = '\0';
    argc = split(line, tok, 5U);

    if (argc == 0U) {                        /* empty or blank request */
        put_err(out, "unknown");
    } else if (word_eq(tok[0], "SET")) {
        uint8_t n = 0;
        uint8_t code = 0;
        uint8_t err = ERR_NONE;

        if (argc != 3U) {
            err = ERR_SYNTAX;
        } else if (parse_pedal(tok[1], &n) == 0) {
            err = ERR_SYNTAX;
        } else if (n < 1U || n > CMD_KEY_COUNT) {
            err = ERR_PEDAL;
        } else if (parse_hex_seg(tok[2], seg_end(tok[2]), &code) == 0) {
            err = ERR_SYNTAX;
        } else if (cmd_code_ok(code) == 0U) {
            err = ERR_RANGE;
        }
        if (err != ERR_NONE) {
            put_err_code(out, err);
        } else {
            s_ped[n - 1U].code = code;
            s_ped[n - 1U].mode = CMD_MODE_SINGLE;  /* leaves macro mode */
            s_dirty = 1U;
            resp_set(out, n, code);
        }
    } else if (word_eq(tok[0], "MAC")) {
        uint8_t n = 0;
        uint8_t mask[CMD_MACRO_MAX_STEPS];
        uint8_t key[CMD_MACRO_MAX_STEPS];
        uint8_t count = 0;
        uint8_t err = ERR_NONE;
        uint16_t press = 0;
        uint16_t gap = 0;
        int r;

        if (argc != 3U && argc != 5U) {
            err = ERR_SYNTAX;
        } else if (parse_pedal(tok[1], &n) == 0) {
            err = ERR_SYNTAX;
        } else if (n < 1U || n > CMD_KEY_COUNT) {
            err = ERR_PEDAL;
        } else if ((r = parse_steps(tok[2], mask, key, &count)) != 0) {
            err = (uint8_t)((r == 2) ? ERR_RANGE : ERR_SYNTAX);
        } else if (argc == 5U) {
            int rp = parse_time(tok[3], &press);
            int rg = parse_time(tok[4], &gap);

            if (rp == 1 || rg == 1) { err = ERR_SYNTAX; }
            else if (rp == 2 || rg == 2) { err = ERR_RANGE; }
        }
        if (err != ERR_NONE) {
            put_err_code(out, err);
        } else {
            cmd_pedal_t *p = &s_ped[n - 1U];
            uint8_t s;

            if (argc == 3U) {                /* keep the stored timing */
                press = p->press_ms;
                gap = p->gap_ms;
            }
            p->mode = CMD_MODE_MACRO;
            p->step_count = count;
            p->press_ms = press;
            p->gap_ms = gap;
            for (s = 0u; s < CMD_MACRO_MAX_STEPS; s++) {
                p->mod_mask[s] = (s < count) ? mask[s] : 0u;
                p->keycode[s] = (s < count) ? key[s] : 0u;
            }
            s_dirty = 1U;
            resp_mac(out, n);
        }
    } else if (word_eq(tok[0], "MGET")) {
        uint8_t n = 0;

        if (argc != 2U) {
            put_err(out, "syntax");
        } else if (parse_pedal(tok[1], &n) == 0) {
            put_err(out, "syntax");
        } else if (n < 1U || n > CMD_KEY_COUNT) {
            put_err(out, "pedal");
        } else {
            resp_mac(out, n);
        }
    } else if (word_eq(tok[0], "MTIM")) {
        uint8_t n = 0;
        uint8_t err = ERR_NONE;
        uint16_t press = 0;
        uint16_t gap = 0;

        if (argc != 2U && argc != 4U) {
            err = ERR_SYNTAX;
        } else if (parse_pedal(tok[1], &n) == 0) {
            err = ERR_SYNTAX;
        } else if (n < 1U || n > CMD_KEY_COUNT) {
            err = ERR_PEDAL;
        } else if (argc == 4U) {
            int rp = parse_time(tok[2], &press);
            int rg = parse_time(tok[3], &gap);

            if (rp == 1 || rg == 1) { err = ERR_SYNTAX; }
            else if (rp == 2 || rg == 2) { err = ERR_RANGE; }
        }
        if (err != ERR_NONE) {
            put_err_code(out, err);
        } else if (argc == 4U) {
            s_ped[n - 1U].press_ms = press;
            s_ped[n - 1U].gap_ms = gap;
            s_dirty = 1U;
            resp_mtim(out, n);
        } else {
            resp_mtim(out, n);               /* query */
        }
    } else if (word_eq(tok[0], "GET") || word_eq(tok[0], "SAVE") ||
               word_eq(tok[0], "RESET") || word_eq(tok[0], "INFO")) {
        if (argc != 1U) {
            put_err(out, "syntax");
        } else if (word_eq(tok[0], "GET")) {
            resp_map(out);
        } else if (word_eq(tok[0], "INFO")) {
            resp_info(out);
        } else if (word_eq(tok[0], "SAVE")) {
            (void)out_put(out, 0, "OK saving\n");
            return 1U;                       /* caller owns the flash write */
        } else {                             /* RESET */
            for (i = 0u; i < CMD_KEY_COUNT; i++) { pedal_defaults(i); }
            s_dirty = 1U;
            (void)out_put(out, 0, "OK RESET\n");
        }
    } else {
        put_err(out, "unknown");
    }
    return 0U;
}

void cmd_get_pedal(uint8_t index, cmd_pedal_t *out) {
    uint8_t s;

    out->mode = CMD_MODE_SINGLE;
    out->code = 0u;
    out->press_ms = 0u;
    out->gap_ms = 0u;
    out->step_count = 0u;
    for (s = 0u; s < CMD_MACRO_MAX_STEPS; s++) {
        out->mod_mask[s] = 0u;
        out->keycode[s] = 0u;
    }
    if (index < CMD_KEY_COUNT) { *out = s_ped[index]; }
}

void cmd_set_pedal(uint8_t index, const cmd_pedal_t *in) {
    if (index < CMD_KEY_COUNT) { s_ped[index] = *in; }
}

uint8_t cmd_keycode(uint8_t index) {
    return (index < CMD_KEY_COUNT) ? s_ped[index].code : 0U;
}

uint8_t cmd_dirty(void) { return s_dirty; }

void cmd_save_done(void) { s_dirty = 0U; }

void cmd_mark_dirty(void) { s_dirty = 1U; }
