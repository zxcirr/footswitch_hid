/**
  * test_cmd.c - host-side unit tests for the frozen feature-report command
  * parser interface declared in app/cmd.h.
  *
  * TDD RED phase: app/cmd.c is not implemented yet, so the full build fails
  * (missing translation unit).  Once app/cmd.c exists:
  *
  *   gcc -std=c11 -Wall -Wextra -Wshadow -Iapp host_test/test_cmd.c app/cmd.c \
  *       -o /tmp/test_cmd && /tmp/test_cmd
  *
  * Self-contained: no test framework, deterministic, one output line per
  * check, exit 0 only when every check passed.
  */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "cmd.h"

/* ================================ harness ================================= */

static int g_checks;   /* number of CHECK() evaluations          */
static int g_failures; /* number of failed CHECK() evaluations   */

#define CHECK(cond, name)                                                \
    do {                                                                 \
        g_checks++;                                                      \
        if (cond) {                                                      \
            printf("PASS %s\n", (name));                                 \
        } else {                                                         \
            g_failures++;                                                \
            printf("FAIL %s (%s:%d)\n", (name), __FILE__, __LINE__);     \
        }                                                                \
    } while (0)

/* ============================= test helpers =============================== */

/* Build a NUL-padded CMD_FEATURE_LEN-byte request from s.  lead_nul
 * prepends the host report-ID 0x00 byte, trail_nl appends '\n' after the
 * command; both are optional protocol tolerances. */
static void req_build(uint8_t in[CMD_FEATURE_LEN], const char *s,
                      int lead_nul, int trail_nl)
{
    size_t n = strlen(s);
    size_t off = 0;

    memset(in, 0, CMD_FEATURE_LEN);
    if (lead_nul) {
        in[off++] = 0x00;
    }
    if (n > (size_t)CMD_FEATURE_LEN - off) {
        n = (size_t)CMD_FEATURE_LEN - off;
    }
    memcpy(in + off, s, n);
    off += n;
    if (trail_nl && off < (size_t)CMD_FEATURE_LEN) {
        in[off] = '\n';
    }
}

/* Plain NUL-padded request from a string. */
static void req_str(uint8_t in[CMD_FEATURE_LEN], const char *s)
{
    req_build(in, s, 0, 0);
}

/* Response bytes as a C string.  cmd_handle() NUL-pads out[]; the extra
 * terminating byte is belt-and-braces against a non-conforming reply. */
static void resp_str(const uint8_t out[CMD_FEATURE_LEN], char *dst)
{
    memcpy(dst, out, CMD_FEATURE_LEN);
    dst[CMD_FEATURE_LEN] = '\0';
}

/* True when every byte after the first NUL in out[] is also NUL. */
static int nul_padded(const uint8_t out[CMD_FEATURE_LEN])
{
    size_t i;

    for (i = 0; i < (size_t)CMD_FEATURE_LEN; i++) {
        if (out[i] == 0x00) {
            for (i++; i < (size_t)CMD_FEATURE_LEN; i++) {
                if (out[i] != 0x00) {
                    return 0;
                }
            }
            return 1;
        }
    }
    return 0; /* no NUL at all: not a padded report */
}

static int starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int contains(const char *s, const char *needle)
{
    return strstr(s, needle) != NULL;
}

/* One command round trip; returns cmd_handle()'s value.  out[] is poisoned
 * first so stale-report leakage would be visible. */
static uint8_t run(const char *request, char *resp)
{
    uint8_t in[CMD_FEATURE_LEN];
    uint8_t out[CMD_FEATURE_LEN];
    uint8_t ret;

    req_str(in, request);
    memset(out, 0xAA, sizeof out);
    ret = cmd_handle(in, out);
    resp_str(out, resp);
    return ret;
}

static int map_default(void)
{
    return cmd_keymap[0] == 0x1E && cmd_keymap[1] == 0x1F &&
           cmd_keymap[2] == 0x20;
}

static int map_is(uint8_t k0, uint8_t k1, uint8_t k2)
{
    return cmd_keycode(0) == k0 && cmd_keycode(1) == k1 &&
           cmd_keycode(2) == k2;
}

/* ================================== main ================================== */

int main(void)
{
    uint8_t  in[CMD_FEATURE_LEN];
    uint8_t  out[CMD_FEATURE_LEN];
    char     resp[CMD_FEATURE_LEN + 1];
    char     ref[CMD_FEATURE_LEN + 1];
    uint32_t before;

    /* -------- 1. boot state ---------------------------------------------- */
    cmd_init();
    CHECK(map_default(), "init: keymap is 0x1E 0x1F 0x20");
    CHECK(map_is(0x1E, 0x1F, 0x20), "init: cmd_keycode mirrors keymap");
    CHECK(cmd_dirty() == 0, "init: dirty flag clear");
    CHECK(cmd_count == 0, "init: cmd_count reset to 0");

    /* -------- 2. SET 1 0x04 ---------------------------------------------- */
    (void)run("SET 1 0x04", resp);
    CHECK(starts_with(resp, "OK SET 1=0x04"), "SET 1 0x04: OK echo");
    CHECK(cmd_keycode(0) == 0x04, "SET 1 0x04: binding stored");
    CHECK(cmd_dirty() == 1, "SET 1 0x04: map marked dirty");

    /* -------- 3. SET 2 0x2c: lowercase command and hex ------------------- */
    (void)run("SET 2 0x2c", resp);
    CHECK(starts_with(resp, "OK SET 2=0x2C"),
          "SET 2 0x2c: case-insensitive in, canonical upper hex out");
    CHECK(cmd_keycode(1) == 0x2C, "SET 2 0x2c: binding stored");

    /* -------- 4. SET 1 0x66: above Usage Maximum 0x65 -------------------- */
    (void)run("SET 1 0x66", resp);
    CHECK(starts_with(resp, "ERR range"), "SET 1 0x66: rejected as out of range");
    CHECK(cmd_keycode(0) == 0x04 && cmd_keycode(1) == 0x2C,
          "SET 1 0x66: keymap untouched");

    /* -------- 5. SET 1 0x70: F13-F24 hole (0x68..0x73) ------------------- */
    (void)run("SET 1 0x70", resp);
    CHECK(starts_with(resp, "ERR range"), "SET 1 0x70: F13-F24 hole rejected");
    CHECK(cmd_keycode(0) == 0x04, "SET 1 0x70: keymap untouched");

    /* -------- 6. SET 1 0x00: disabled ------------------------------------ */
    (void)run("SET 1 0x00", resp);
    CHECK(starts_with(resp, "OK SET 1=0x00"), "SET 1 0x00: disable accepted");
    CHECK(cmd_keycode(0) == 0x00, "SET 1 0x00: pedal disabled");
    CHECK(cmd_dirty() == 1, "SET 1 0x00: still dirty");

    /* -------- 7. SET 1 0xE0: modifier ------------------------------------ */
    (void)run("SET 1 0xE0", resp);
    CHECK(starts_with(resp, "OK SET 1=0xE0"), "SET 1 0xE0: modifier accepted");
    CHECK(cmd_keycode(0) == 0xE0, "SET 1 0xE0: modifier stored");

    /* -------- 8. pedal index outside 1..3 -------------------------------- */
    (void)run("SET 4 0x04", resp);
    CHECK(starts_with(resp, "ERR pedal"), "SET 4 0x04: pedal 4 rejected");
    (void)run("SET 0 0x04", resp);
    CHECK(starts_with(resp, "ERR pedal"), "SET 0 0x04: pedal 0 rejected");
    CHECK(cmd_keycode(0) == 0xE0, "bad pedal: keymap untouched");

    /* -------- 9. malformed / unknown requests ---------------------------- */
    {
        static const char *const bad[] = {
            "SET 1",      /* missing code     */
            "SET",        /* missing args     */
            "SET 1 0xZZ", /* invalid hex      */
            "",           /* empty / blank    */
            "FOO"         /* unknown command  */
        };
        static const char *const bad_name[] = {
            "SET 1: ERR syntax or unknown",
            "SET: ERR syntax or unknown",
            "SET 1 0xZZ: ERR syntax or unknown",
            "empty: ERR syntax or unknown",
            "FOO: ERR syntax or unknown"
        };
        size_t i;

        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            (void)run(bad[i], resp);
            CHECK(starts_with(resp, "ERR syntax") ||
                  starts_with(resp, "ERR unknown"),
                  bad_name[i]);
        }
        CHECK(map_is(0xE0, 0x2C, 0x20), "malformed batch: keymap untouched");
    }

    /* -------- 9b. key-code boundaries: 0xFF / 0x68 / 0x73 are ERR range -- */
    (void)run("SET 1 0xFF", resp);
    CHECK(starts_with(resp, "ERR range"), "SET 1 0xFF: rejected as out of range");
    (void)run("SET 1 0x68", resp);
    CHECK(starts_with(resp, "ERR range"), "SET 1 0x68: F13-F24 hole lower edge rejected");
    (void)run("SET 1 0x73", resp);
    CHECK(starts_with(resp, "ERR range"), "SET 1 0x73: F13-F24 hole upper edge rejected");
    CHECK(map_is(0xE0, 0x2C, 0x20), "range boundaries: keymap untouched");

    /* -------- 9c. oversized request: all 48 bytes non-NUL garbage --------- */
    {
        uint8_t bad_in[CMD_FEATURE_LEN];

        memset(bad_in, 'A', sizeof bad_in);
        memset(out, 0xAA, sizeof out);
        (void)cmd_handle(bad_in, out);
        resp_str(out, resp);
        CHECK(starts_with(resp, "ERR unknown") || starts_with(resp, "ERR syntax"),
              "48x'A': garbage request rejected");
        CHECK(map_is(0xE0, 0x2C, 0x20), "48x'A': keymap untouched");
    }

    /* -------- 10. GET ---------------------------------------------------- */
    memset(out, 0xAA, sizeof out);
    req_str(in, "GET");
    (void)cmd_handle(in, out);
    resp_str(out, resp);
    CHECK(strcmp(resp, "OK MAP 0xE0 0x2C 0x20\n") == 0,
          "GET: canonical map echo");
    CHECK(nul_padded(out), "GET: out[] NUL-padded to 48 bytes");

    /* -------- 11. SAVE: flash write owned by the caller ------------------ */
    {
        uint8_t ret = run("SAVE", resp);

        CHECK(ret == 1, "SAVE: cmd_handle returns 1");
        CHECK(contains(resp, "OK saving"), "SAVE: placeholder response");
        CHECK(cmd_dirty() == 1, "SAVE: still dirty before cmd_save_done()");
        cmd_save_done();
        CHECK(cmd_dirty() == 0, "cmd_save_done(): dirty cleared");
    }

    /* -------- 12. RESET -------------------------------------------------- */
    (void)run("RESET", resp);
    CHECK(strcmp(resp, "OK RESET\n") == 0, "RESET: response");
    CHECK(map_default(), "RESET: defaults restored in keymap");
    CHECK(map_is(0x1E, 0x1F, 0x20), "RESET: cmd_keycode matches defaults");
    CHECK(cmd_dirty() == 1, "RESET: marked dirty");

    /* -------- 13. INFO --------------------------------------------------- */
    (void)run("INFO", resp);
    CHECK(starts_with(resp, "OK INFO"), "INFO: response prefix");
    CHECK(contains(resp, "keys="), "INFO: contains keys=");
    CHECK(contains(resp, "dirty="), "INFO: contains dirty=");
    CHECK(contains(resp, "keys=0x1E,0x1F,0x20"), "INFO: keys reflect map");
    CHECK(contains(resp, "dirty=1"), "INFO: dirty=1");

    /* -------- 14. leading 0x00 report-ID byte tolerated ------------------ */
    (void)run("GET", ref);
    req_build(in, "GET", 1, 0);
    CHECK(in[0] == 0x00 && in[1] == 'G' && in[2] == 'E' && in[3] == 'T' &&
          in[4] == 0x00,
          "leading NUL: request is {00,'G','E','T',00,...}");
    (void)cmd_handle(in, out);
    resp_str(out, resp);
    CHECK(strcmp(resp, ref) == 0, "leading NUL: same reply as GET");

    /* -------- 15. trailing newline tolerated ----------------------------- */
    req_build(in, "GET", 0, 1);
    CHECK(in[0] == 'G' && in[1] == 'E' && in[2] == 'T' && in[3] == '\n' &&
          in[4] == 0x00,
          "trailing newline: request is \"GET\\n\"");
    (void)cmd_handle(in, out);
    resp_str(out, resp);
    CHECK(strcmp(resp, ref) == 0, "trailing newline: same reply as GET");

    /* -------- 16. cmd_count increments once per cmd_handle() ------------- */
    before = cmd_count;
    req_str(in, "GET");
    (void)cmd_handle(in, out);
    CHECK(cmd_count == before + 1, "cmd_count: +1 for a valid GET");

    before = cmd_count;
    req_str(in, "GARBAGE");
    (void)cmd_handle(in, out);
    CHECK(cmd_count == before + 1, "cmd_count: +1 for an unknown request");

    before = cmd_count;
    (void)run("SET 3 0x04", resp);
    (void)run("GET", resp);
    CHECK(cmd_count == before + 2, "cmd_count: +1 per call over two calls");

    /* -------- 17. cmd_code_ok(): the shared SET predicate ---------------- */
    CHECK(cmd_code_ok(0x00) && cmd_code_ok(0x04) && cmd_code_ok(0x65) &&
          cmd_code_ok(0xE0) && cmd_code_ok(0xE7),
          "cmd_code_ok: accepts 0x00/0x04/0x65/0xE0/0xE7");
    CHECK(!cmd_code_ok(0x03), "cmd_code_ok: rejects 0x03 below 0x04");
    CHECK(!cmd_code_ok(0x66) && !cmd_code_ok(0x68) && !cmd_code_ok(0x73),
          "cmd_code_ok: rejects 0x66 and the 0x68..0x73 F13-F24 hole");
    CHECK(!cmd_code_ok(0xDF) && !cmd_code_ok(0xE8) && !cmd_code_ok(0xFF),
          "cmd_code_ok: rejects 0xDF/0xE8/0xFF boundaries");

    /* -------- verdict ---------------------------------------------------- */
    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("FAILED %d\n", g_failures);
    return 1;
}
