/**
  * test_cmd.c - host-side unit tests for the frozen feature-report command
  * parser interface declared in app/cmd.h.
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
    return cmd_keycode(0) == 0x1E && cmd_keycode(1) == 0x1F &&
           cmd_keycode(2) == 0x20;
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
    {
        cmd_pedal_t p;

        cmd_get_pedal(0, &p);
        CHECK(p.mode == CMD_MODE_SINGLE && p.step_count == 0 &&
              p.press_ms == CMD_MACRO_PRESS_DEFAULT &&
              p.gap_ms == CMD_MACRO_GAP_DEFAULT,
              "init: pedal defaults (single, no steps, timing 30/30)");
        cmd_get_pedal(7, &p);
        CHECK(p.mode == CMD_MODE_SINGLE && p.code == 0 && p.step_count == 0,
              "init: cmd_get_pedal out-of-range index zeroed");
    }

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
    CHECK(contains(resp, "fw=0.0.2"), "INFO: firmware version fw=0.0.2");
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

    /* -------- 18. cmd_step_ok(): the shared MAC-step predicate ----------- */
    CHECK(cmd_step_ok(0x00, 0x1D) && cmd_step_ok(0x02, 0x00) &&
          cmd_step_ok(0xFF, 0x65) && cmd_step_ok(0x02, 0x04),
          "cmd_step_ok: accepts plain key / modifier-only / full chord");
    CHECK(!cmd_step_ok(0x00, 0x00), "cmd_step_ok: rejects the empty step");
    CHECK(!cmd_step_ok(0x02, 0xE1) && !cmd_step_ok(0x00, 0xE0),
          "cmd_step_ok: rejects modifier codes as step keys");
    CHECK(!cmd_step_ok(0x00, 0x03) && !cmd_step_ok(0x00, 0x66) &&
          !cmd_step_ok(0x02, 0xFF),
          "cmd_step_ok: rejects keys outside 0x04..0x65");

    /* -------- 19. MAC: three independent taps (shift, z, c) --------------- */
    cmd_init();
    (void)run("MAC 1 02:00,1D,06", resp);
    CHECK(strcmp(resp, "OK MAC 1 02:00,1D,06\n") == 0, "MAC: canonical echo");
    {
        cmd_pedal_t p;

        cmd_get_pedal(0, &p);
        CHECK(p.mode == CMD_MODE_MACRO && p.step_count == 3,
              "MAC: mode macro, 3 steps stored");
        CHECK(p.mod_mask[0] == 0x02 && p.keycode[0] == 0x00,
              "MAC: step 0 = left shift alone");
        CHECK(p.mod_mask[1] == 0x00 && p.keycode[1] == 0x1D,
              "MAC: step 1 = z");
        CHECK(p.mod_mask[2] == 0x00 && p.keycode[2] == 0x06,
              "MAC: step 2 = c");
        CHECK(p.press_ms == CMD_MACRO_PRESS_DEFAULT &&
              p.gap_ms == CMD_MACRO_GAP_DEFAULT,
              "MAC without timing: defaults 30/30 kept");
    }
    CHECK(cmd_dirty() == 1, "MAC: config marked dirty");

    /* -------- 20. GET/INFO show the macro marker -------------------------- */
    (void)run("GET", resp);
    CHECK(strcmp(resp, "OK MAP M 0x1F 0x20\n") == 0, "GET: macro pedal shown as M");
    (void)run("INFO", resp);
    CHECK(contains(resp, "keys=M,0x1F,0x20"), "INFO: M marker in keys=");

    /* -------- 21. MGET: stored steps, and '-' when none ------------------- */
    (void)run("MGET 1", resp);
    CHECK(strcmp(resp, "OK MAC 1 02:00,1D,06\n") == 0, "MGET: stored steps echoed");
    (void)run("MGET 2", resp);
    CHECK(strcmp(resp, "OK MAC 2 -\n") == 0, "MGET: '-' when no steps stored");

    /* -------- 22. MAC input forms: 0x prefixes, lowercase, chords --------- */
    (void)run("MAC 2 0x02:0x00,0x1d,6", resp);
    CHECK(strcmp(resp, "OK MAC 2 02:00,1D,06\n") == 0,
          "MAC: 0x prefixes and lowercase canonicalised");
    (void)run("mac 3 02:1d,06", resp);
    CHECK(strcmp(resp, "OK MAC 3 02:1D,06\n") == 0,
          "MAC: lowercase command, chord step canonicalised");

    /* -------- 23. MAC with timing + MTIM set/query ------------------------ */
    (void)run("MAC 3 02:1D,06 15 25", resp);
    CHECK(strcmp(resp, "OK MAC 3 02:1D,06\n") == 0,
          "MAC+timing: echo covers the steps only");
    (void)run("MTIM 3", resp);
    CHECK(strcmp(resp, "OK MTIM 3 15 25\n") == 0, "MTIM query: timing from MAC");
    (void)run("MTIM 1 40 60", resp);
    CHECK(strcmp(resp, "OK MTIM 1 40 60\n") == 0, "MTIM set: echo");
    (void)run("MTIM 1", resp);
    CHECK(strcmp(resp, "OK MTIM 1 40 60\n") == 0, "MTIM query: stored values");
    (void)run("MTIM 2", resp);
    CHECK(strcmp(resp, "OK MTIM 2 30 30\n") == 0, "MTIM query: defaults untouched");
    {
        cmd_pedal_t p;

        cmd_get_pedal(2, &p);
        CHECK(p.press_ms == 15 && p.gap_ms == 25, "MTIM/MAC: timing stored");
    }

    /* -------- 24. MAC/MTIM syntax errors ---------------------------------- */
    {
        static const char *const bad[] = {
            "MAC 1",              /* no steps                */
            "MAC",                /* no args                 */
            "MAC 1 1D 30",        /* press but no gap        */
            "MAC 1 1D 30 30 7",   /* too many args           */
            "MAC 1 ZZ",           /* bad hex                 */
            "MAC 1 02:",          /* empty key field         */
            "MAC 1 :1D",          /* empty mask field        */
            "MAC 1 1D,,06",       /* empty segment           */
            "MAC 1 1D,",          /* trailing comma          */
            "MAC 1 ,1D",          /* leading comma           */
            "MAC 1 1D:06:02",     /* two colons              */
            "MAC 1 12345",        /* over-long hex field     */
            "MAC 1 1D 3a 30",     /* non-decimal timing      */
            "MGET",               /* missing pedal           */
            "MGET 1 2",           /* extra arg               */
            "MGET 12",            /* multi-digit pedal       */
            "MTIM 1 30",          /* gap missing             */
            "MTIM 1 30 30 30"     /* extra arg               */
        };
        size_t i;

        for (i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            (void)run(bad[i], resp);
            CHECK(starts_with(resp, "ERR syntax"), bad[i]);
        }
    }

    /* -------- 25. MAC/MTIM pedal and range errors ------------------------- */
    (void)run("MAC 4 1D", resp);
    CHECK(starts_with(resp, "ERR pedal"), "MAC 4: pedal rejected");
    (void)run("MGET 0", resp);
    CHECK(starts_with(resp, "ERR pedal"), "MGET 0: pedal rejected");
    (void)run("MTIM 9", resp);
    CHECK(starts_with(resp, "ERR pedal"), "MTIM 9: pedal rejected");
    (void)run("MAC 1 1D,1D,1D,1D,1D,1D,1D", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: 7 steps rejected");
    (void)run("MAC 1 00:00", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: empty chord 00:00 rejected");
    (void)run("MAC 1 00", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: bare key 00 rejected");
    (void)run("MAC 1 E1", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: modifier code as key rejected");
    (void)run("MAC 1 02:E1", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: modifier code in chord rejected");
    (void)run("MAC 1 03", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: key 0x03 rejected");
    (void)run("MAC 1 66", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: key 0x66 rejected");
    (void)run("MAC 1 1D 0 30", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: press 0 rejected");
    (void)run("MAC 1 1D 30 10000", resp);
    CHECK(starts_with(resp, "ERR range"), "MAC: gap above 9999 rejected");
    (void)run("MTIM 1 0 30", resp);
    CHECK(starts_with(resp, "ERR range"), "MTIM: press 0 rejected");
    (void)run("MTIM 1 30 100000", resp);
    CHECK(starts_with(resp, "ERR range"), "MTIM: huge gap rejected");
    /* State after the error batch: pedal 1 still the section-19 macro. */
    (void)run("MGET 1", resp);
    CHECK(strcmp(resp, "OK MAC 1 02:00,1D,06\n") == 0,
          "error batch: pedal 1 macro untouched");
    (void)run("MTIM 1", resp);
    CHECK(strcmp(resp, "OK MTIM 1 40 60\n") == 0,
          "error batch: pedal 1 timing untouched");

    /* -------- 26. SET leaves macro mode, steps/timing kept idle ----------- */
    (void)run("SET 1 0x04", resp);
    CHECK(starts_with(resp, "OK SET 1=0x04"), "SET on macro pedal: OK echo");
    {
        cmd_pedal_t p;

        cmd_get_pedal(0, &p);
        CHECK(p.mode == CMD_MODE_SINGLE && p.code == 0x04,
              "SET: pedal back in single mode");
        CHECK(p.step_count == 3 && p.press_ms == 40 && p.gap_ms == 60,
              "SET: stored steps and timing kept (idle)");
    }
    (void)run("GET", resp);
    CHECK(strcmp(resp, "OK MAP 0x04 M M\n") == 0, "GET: single + two macros");
    (void)run("MGET 1", resp);
    CHECK(strcmp(resp, "OK MAC 1 02:00,1D,06\n") == 0,
          "MGET after SET: idle steps still shown");

    /* -------- 27. RESET clears every macro field -------------------------- */
    (void)run("RESET", resp);
    CHECK(strcmp(resp, "OK RESET\n") == 0, "RESET: response");
    (void)run("GET", resp);
    CHECK(strcmp(resp, "OK MAP 0x1E 0x1F 0x20\n") == 0, "RESET: single defaults");
    (void)run("MGET 2", resp);
    CHECK(strcmp(resp, "OK MAC 2 -\n") == 0, "RESET: steps cleared");
    (void)run("MTIM 1", resp);
    CHECK(strcmp(resp, "OK MTIM 1 30 30\n") == 0, "RESET: default timing");
    {
        cmd_pedal_t p;

        cmd_get_pedal(1, &p);
        CHECK(p.mode == CMD_MODE_SINGLE && p.step_count == 0 &&
              p.code == 0x1F,
              "RESET: pedal struct back to defaults");
    }

    /* -------- 28. six chord steps: request 41B, response 45B, both fit ---- */
    {
        char r2[CMD_FEATURE_LEN + 1];

        memset(out, 0xAA, sizeof out);
        req_str(in, "MAC 3 02:1D,02:1D,02:1D,02:1D,02:1D,02:1D");
        (void)cmd_handle(in, out);
        resp_str(out, r2);
        CHECK(strcmp(r2, "OK MAC 3 02:1D,02:1D,02:1D,02:1D,02:1D,02:1D\n") == 0,
              "MAC: six chord steps full echo");
        CHECK(nul_padded(out), "MAC: response NUL-padded to 48 bytes");
    }

    /* -------- 29. report-ID artefact tolerated for the new commands ------- */
    (void)run("MGET 3", ref);
    req_build(in, "MGET 3", 1, 1);
    (void)cmd_handle(in, out);
    resp_str(out, resp);
    CHECK(strcmp(resp, ref) == 0, "MGET: leading NUL + trailing newline tolerated");

    /* -------- 30. cmd_get_pedal / cmd_set_pedal round trip ---------------- */
    {
        cmd_pedal_t src;
        cmd_pedal_t dst;
        int same = 1;

        cmd_init();
        (void)run("MAC 2 02:1D,06 15 25", resp);
        cmd_get_pedal(1, &src);
        cmd_init();                       /* wipe the RAM state */
        CHECK(cmd_dirty() == 0, "round trip: re-init clean");
        cmd_set_pedal(1, &src);
        cmd_get_pedal(1, &dst);
        CHECK(dst.mode == src.mode && dst.code == src.code &&
              dst.press_ms == src.press_ms && dst.gap_ms == src.gap_ms &&
              dst.step_count == src.step_count,
              "round trip: scalar fields restored");
        for (uint8_t s = 0; s < CMD_MACRO_MAX_STEPS; s++) {
            if (dst.mod_mask[s] != src.mod_mask[s] ||
                dst.keycode[s] != src.keycode[s]) {
                same = 0;
            }
        }
        CHECK(same, "round trip: step arrays restored");
        CHECK(cmd_dirty() == 0, "cmd_set_pedal: does not touch dirty");
        cmd_set_pedal(9, &src);           /* out-of-range index ignored */
        cmd_get_pedal(0, &dst);
        CHECK(dst.mode == CMD_MODE_SINGLE && dst.step_count == 0,
              "cmd_set_pedal: out-of-range index ignored");
    }

    /* -------- 31. cmd_mark_dirty ------------------------------------------ */
    cmd_init();
    CHECK(cmd_dirty() == 0, "mark_dirty: clean after init");
    cmd_mark_dirty();
    CHECK(cmd_dirty() == 1, "cmd_mark_dirty: flag set");
    cmd_save_done();
    CHECK(cmd_dirty() == 0, "cmd_save_done: flag cleared again");

    /* -------- verdict ---------------------------------------------------- */
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("FAILED %d\n", g_failures);
    return 1;
}
