/**
  * cmd.c - host-configurable pedal keymap command parser (contract: cmd.h).
  *
  * Pure C11, <stdint.h> + "cmd.h": no libc, no dynamic memory, non-blocking.
  * clear out[] -> copy request to a local line (skip one leading report-ID
  * 0x00, stop at the first NUL) -> split on whitespace -> case-insensitive
  * command dispatch -> validate -> mutate state -> '\n'-terminated reply.
  */
#include <stdint.h>
#include "cmd.h"

volatile uint8_t  cmd_keymap[CMD_KEY_COUNT];
volatile uint32_t cmd_count;

static uint8_t s_dirty;                  /* RAM differs from the last save */

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

static void resp_map(uint8_t *out) {
    uint8_t pos = out_put(out, 0, "OK MAP");
    uint8_t i;

    for (i = 0; i < CMD_KEY_COUNT; i++) {
        pos = out_hex(out, out_put(out, pos, " "), cmd_keymap[i]);
    }
    (void)out_put(out, pos, "\n");
}

/* The documented INFO text "OK INFO fw=1.0 keys=0xNN,0xNN,0xNN dirty=N\n" is
 * 43 bytes and fits the CMD_FEATURE_LEN (48) byte transport; out_put() never
 * writes past the report size. */
static void resp_info(uint8_t *out) {
    uint8_t pos = out_put(out, 0, "OK INFO fw=1.0 keys=");
    uint8_t i;

    for (i = 0; i < CMD_KEY_COUNT; i++) {
        if (i > 0U) { pos = out_put(out, pos, ","); }
        pos = out_hex(out, pos, cmd_keymap[i]);
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

/* 1-2 hex digits with optional 0x/0X prefix, then end of token. */
static int parse_code(const char *t, uint8_t *code) {
    uint8_t hi;
    uint8_t lo;

    if (t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) { t += 2; }
    if (!hex_digit(t[0], &hi)) { return 0; }
    if (t[1] == '\0') { *code = hi; return 1; }
    if (!hex_digit(t[1], &lo) || t[2] != '\0') { return 0; }
    *code = (uint8_t)((uint8_t)(hi << 4) | lo);
    return 1;
}

/* 0x00 disabled; 0x04..0x65 keys; 0xE0..0xE7 modifiers; else out of range. */
uint8_t cmd_code_ok(uint8_t code) {
    return (uint8_t)(code == 0x00U || (code >= 0x04U && code <= 0x65U) ||
                     (code >= 0xE0U && code <= 0xE7U));
}

/* -------------------------------------------------------------- public API */

void cmd_init(void) {
    cmd_keymap[0] = 0x1EU;
    cmd_keymap[1] = 0x1FU;
    cmd_keymap[2] = 0x20U;
    cmd_count = 0U;
    s_dirty = 0U;
}

uint8_t cmd_handle(const uint8_t in[CMD_FEATURE_LEN], uint8_t out[CMD_FEATURE_LEN]) {
    char line[CMD_FEATURE_LEN + 1];
    char *tok[4];
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
    argc = split(line, tok, 4U);

    if (argc == 0U) {                        /* empty or blank request */
        put_err(out, "unknown");
    } else if (word_eq(tok[0], "SET")) {
        uint8_t n;
        uint8_t code;

        if (argc != 3U) {
            put_err(out, "syntax");
        } else if (!parse_pedal(tok[1], &n) || !parse_code(tok[2], &code)) {
            put_err(out, "syntax");
        } else if (n < 1U || n > CMD_KEY_COUNT) {
            put_err(out, "pedal");
        } else if (cmd_code_ok(code) == 0U) {
            put_err(out, "range");
        } else {
            cmd_keymap[n - 1U] = code;
            s_dirty = 1U;
            resp_set(out, n, code);
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
            cmd_keymap[0] = 0x1EU;
            cmd_keymap[1] = 0x1FU;
            cmd_keymap[2] = 0x20U;
            s_dirty = 1U;
            (void)out_put(out, 0, "OK RESET\n");
        }
    } else {
        put_err(out, "unknown");
    }
    return 0U;
}

uint8_t cmd_keycode(uint8_t index) {
    return (index < CMD_KEY_COUNT) ? cmd_keymap[index] : 0U;
}

void cmd_set_keymap(uint8_t index, uint8_t code) {
    if (index < CMD_KEY_COUNT) { cmd_keymap[index] = code; }
}

uint8_t cmd_dirty(void) { return s_dirty; }

void cmd_save_done(void) { s_dirty = 0U; }
