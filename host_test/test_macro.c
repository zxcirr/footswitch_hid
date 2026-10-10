/**
  * test_macro.c - host-side unit tests for the macro playback engine declared
  * in app/macro.h.
  *
  *   gcc -std=c11 -Wall -Wextra -Wshadow -Iapp host_test/test_macro.c \
  *       app/macro.c -o /tmp/test_macro && /tmp/test_macro
  *
  * Self-contained: no test framework, deterministic fake millisecond clock,
  * one output line per check, exit 0 only when every check passed.
  */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "macro.h"

/* ================================ harness ================================= */

static int g_checks;
static int g_failures;

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

/* ================================ fixture ================================= */

/* Step list + timing handed to macro_tick() each tick, like the real caller
 * (main.c) does from a cmd_get_pedal() snapshot.  Step i: mask 0x02 for
 * step 0 (a shift chord), 0x00 otherwise; key 0x1D + i ('z', '{', '|' ...
 * - the codes themselves are irrelevant to the engine). */
#define FIX_STEPS 6

typedef struct
{
    uint8_t  mask[FIX_STEPS];
    uint8_t  key[FIX_STEPS];
    uint8_t  count;
    uint16_t press;
    uint16_t gap;
} fx_t;

static void fx_init(fx_t *f, uint8_t count, uint16_t press, uint16_t gap)
{
    uint8_t i;

    memset(f, 0, sizeof *f);
    f->count = count;
    f->press = press;
    f->gap = gap;
    for (i = 0; i < FIX_STEPS; i++)
    {
        f->mask[i] = (i == 0u) ? 0x02u : 0x00u;
        f->key[i] = (uint8_t)(0x1Du + i);
    }
}

static void fx_tick(const fx_t *f, macro_state_t *st, uint8_t held, uint32_t now,
                    uint8_t *m, uint8_t *k)
{
    macro_tick(st, f->mask, f->key, f->count, f->press, f->gap, held, now, m, k);
}

/* Step signature of one tick: '0'..'5' = that step pressed, '.' = gap,
 * 'x' = idle/released. */
static char fx_sig(const fx_t *f, macro_state_t *st, uint8_t held, uint32_t now)
{
    uint8_t m = 0xAA;
    uint8_t k = 0xAA;

    fx_tick(f, st, held, now, &m, &k);
    if (m == 0u && k == 0u)
    {
        return held ? '.' : 'x';
    }
    return (char)('0' + (k - 0x1Du));
}

/* ================================== main ================================== */

int main(void)
{
    fx_t f;
    macro_state_t st;
    uint8_t m;
    uint8_t k;

    /* -------- 1. idle: released pedal outputs nothing --------------------- */
    fx_init(&f, 3, 30, 30);
    macro_reset(&st);
    for (uint32_t t = 0; t < 10; t++)
    {
        fx_tick(&f, &st, 0, t, &m, &k);
        if (m != 0u || k != 0u) { break; }
    }
    CHECK(m == 0u && k == 0u, "idle: released pedal outputs zero chord");
    CHECK(st.active == 0u, "idle: engine not started");

    /* -------- 2. rising edge: step 0 chord immediately -------------------- */
    fx_tick(&f, &st, 1, 100, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "edge: step 0 chord on the first held tick");
    CHECK(st.active == 1u && st.pressing == 1u && st.step == 0u,
          "edge: engine started at step 0, pressing");

    /* -------- 3. phase timing: press 30 ms, gap 30 ms --------------------- */
    fx_tick(&f, &st, 1, 129, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "press: still step 0 at now=deadline-1");
    fx_tick(&f, &st, 1, 130, &m, &k);
    CHECK(m == 0u && k == 0u, "gap: released exactly at the deadline");
    fx_tick(&f, &st, 1, 159, &m, &k);
    CHECK(m == 0u && k == 0u, "gap: still released at gap end - 1");
    fx_tick(&f, &st, 1, 160, &m, &k);
    CHECK(m == 0x00u && k == 0x1Eu, "advance: step 1 after the gap");

    /* -------- 4. wrap-around: step 2 -> step 0 ---------------------------- */
    fx_tick(&f, &st, 1, 189, &m, &k);
    CHECK(m == 0x00u && k == 0x1Eu, "wrap: step 1 held to its deadline - 1");
    fx_tick(&f, &st, 1, 190, &m, &k);
    CHECK(m == 0u && k == 0u, "wrap: gap after step 1");
    fx_tick(&f, &st, 1, 220, &m, &k);
    CHECK(m == 0x00u && k == 0x1Fu, "wrap: step 2 pressed");
    fx_tick(&f, &st, 1, 250, &m, &k);
    CHECK(m == 0u && k == 0u, "wrap: gap after step 2");
    fx_tick(&f, &st, 1, 280, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "wrap: back to step 0 (chord)");

    /* -------- 5. release mid-press: immediate zeros, restart at step 0 ---- */
    fx_tick(&f, &st, 0, 285, &m, &k);
    CHECK(m == 0u && k == 0u, "release: chord dropped on the same tick");
    CHECK(st.active == 0u, "release: engine idle");
    fx_tick(&f, &st, 1, 400, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "re-hold: restarts at step 0");

    /* -------- 6. release during the gap ----------------------------------- */
    fx_tick(&f, &st, 1, 429, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "gap release: step 0 held to deadline - 1");
    fx_tick(&f, &st, 1, 430, &m, &k);
    CHECK(m == 0u && k == 0u, "gap release: in the gap");
    fx_tick(&f, &st, 0, 440, &m, &k);
    CHECK(m == 0u && k == 0u && st.active == 0u, "gap release: idle again");
    fx_tick(&f, &st, 1, 500, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "gap release: re-hold restarts at step 0");

    /* -------- 7. one-step macro cycles forever ---------------------------- */
    fx_init(&f, 1, 30, 30);
    macro_reset(&st);
    fx_tick(&f, &st, 1, 0, &m, &k);
    CHECK(k == 0x1Du, "1-step: press at t=0");
    fx_tick(&f, &st, 1, 30, &m, &k);
    CHECK(k == 0u, "1-step: gap at t=30");
    fx_tick(&f, &st, 1, 60, &m, &k);
    CHECK(k == 0x1Du, "1-step: press again at t=60 (wrap to step 0)");
    fx_tick(&f, &st, 1, 90, &m, &k);
    CHECK(k == 0u, "1-step: gap at t=90");

    /* -------- 8. step list shrinks under a running cycle ------------------ */
    fx_init(&f, 3, 30, 30);
    macro_reset(&st);
    fx_tick(&f, &st, 1, 0, &m, &k);     /* step 0 press, deadline 30  */
    fx_tick(&f, &st, 1, 30, &m, &k);    /* gap, deadline 60           */
    fx_tick(&f, &st, 1, 60, &m, &k);    /* step 1 press, deadline 90  */
    fx_tick(&f, &st, 1, 90, &m, &k);    /* gap, deadline 120          */
    fx_tick(&f, &st, 1, 120, &m, &k);   /* step 2 press, deadline 150 */
    CHECK(k == 0x1Fu, "shrink: at step 2 with count 3");
    f.count = 2;                        /* MAC while held: list shrank */
    fx_tick(&f, &st, 1, 121, &m, &k);
    CHECK(st.step < f.count, "shrink: step index clamped below the new count");
    CHECK(m == 0x02u && k == 0x1Du, "shrink: resumes at step 0, no OOB read");

    /* -------- 9. zero timing is clamped to 1 ms --------------------------- */
    fx_init(&f, 1, 0, 0);
    macro_reset(&st);
    fx_tick(&f, &st, 1, 0, &m, &k);
    CHECK(k == 0x1Du, "clamp: press at t=0");
    fx_tick(&f, &st, 1, 1, &m, &k);
    CHECK(k == 0u, "clamp: gap at t=1 (press clamped to 1 ms)");
    fx_tick(&f, &st, 1, 2, &m, &k);
    CHECK(k == 0x1Du, "clamp: press at t=2 (gap clamped to 1 ms)");

    /* -------- 10. 32-bit ms wrap-around ----------------------------------- */
    fx_init(&f, 2, 30, 30);
    macro_reset(&st);
    {
        uint32_t start = 0xFFFFFFF0u;   /* deadline wraps past 2^32 */
        uint32_t t;
        int press_ok = 1;

        fx_tick(&f, &st, 1, start, &m, &k);
        if (k != 0x1Du) { press_ok = 0; }
        for (uint32_t i = 1; i <= 14u; i++)   /* t = start+1 .. 0xFFFFFFFF */
        {
            t = start + i;
            fx_tick(&f, &st, 1, t, &m, &k);
            if (k != 0x1Du) { press_ok = 0; }
        }
        CHECK(press_ok, "wrap32: step 0 held across the 2^32 boundary");
        fx_tick(&f, &st, 1, 14u, &m, &k);     /* start+30 = 0x0000000E */
        CHECK(m == 0u && k == 0u, "wrap32: gap starts at the wrapped deadline");
        fx_tick(&f, &st, 1, 44u, &m, &k);
        CHECK(k == 0x1Eu, "wrap32: step 1 after the wrapped gap");
    }

    /* -------- 11. timing change takes effect from the next phase ---------- */
    fx_init(&f, 2, 30, 30);
    macro_reset(&st);
    fx_tick(&f, &st, 1, 100, &m, &k);         /* step 0 press, deadline 130 */
    f.press = 50;                             /* config change mid-press    */
    fx_tick(&f, &st, 1, 130, &m, &k);
    CHECK(m == 0u && k == 0u,
          "retiming: old press deadline honoured; gap latched with old 30 ms");
    fx_tick(&f, &st, 1, 140, &m, &k);
    CHECK(m == 0u && k == 0u,
          "retiming: gap config change does not shorten the running gap");
    f.gap = 10;
    fx_tick(&f, &st, 1, 160, &m, &k);
    CHECK(k == 0x1Eu, "retiming: step 1 pressed, new press=50 -> deadline 210");
    fx_tick(&f, &st, 1, 209, &m, &k);
    CHECK(k == 0x1Eu, "retiming: new press still held at +49");
    fx_tick(&f, &st, 1, 210, &m, &k);
    CHECK(m == 0u && k == 0u, "retiming: gap at the new press deadline");
    fx_tick(&f, &st, 1, 220, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du,
          "retiming: new gap (10 ms) elapsed, wrapped to step 0");

    /* -------- 12. full-cycle output pattern, tick by tick ----------------- */
    fx_init(&f, 3, 30, 30);
    macro_reset(&st);
    {
        char sig[181];

        for (uint32_t t = 0; t < 180u; t++)
        {
            sig[t] = fx_sig(&f, &st, 1, t);
        }
        sig[180] = '\0';

        CHECK(memcmp(sig,
                     "000000000000000000000000000000"   /* step 0 press 30 */
                     ".............................."   /* gap 30          */
                     "111111111111111111111111111111"   /* step 1 press 30 */
                     ".............................."   /* gap 30          */
                     "222222222222222222222222222222"   /* step 2 press 30 */
                     "..............................",  /* gap 30          */
                     180) == 0,
              "pattern: two full 3-step cycles match the expected signature");
    }

    /* -------- 13. count 0 disables the engine ----------------------------- */
    fx_init(&f, 0, 30, 30);
    macro_reset(&st);
    fx_tick(&f, &st, 1, 0, &m, &k);
    CHECK(m == 0u && k == 0u && st.active == 0u, "count 0: held pedal outputs nothing");

    /* -------- 14. macro_reset clears a running cycle ---------------------- */
    fx_init(&f, 3, 30, 30);
    macro_reset(&st);
    fx_tick(&f, &st, 1, 0, &m, &k);           /* step 0 press, deadline 30 */
    fx_tick(&f, &st, 1, 30, &m, &k);          /* gap, deadline 60          */
    fx_tick(&f, &st, 1, 60, &m, &k);          /* step 1 press, deadline 90 */
    fx_tick(&f, &st, 1, 70, &m, &k);          /* mid step 1                */
    CHECK(k == 0x1Eu, "reset: running at step 1");
    macro_reset(&st);
    CHECK(st.active == 0u && st.pressing == 0u && st.step == 0u,
          "reset: state cleared");
    fx_tick(&f, &st, 1, 71, &m, &k);
    CHECK(m == 0x02u && k == 0x1Du, "reset: next hold restarts at step 0");

    /* -------- verdict ---------------------------------------------------- */
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures == 0)
    {
        printf("ALL PASS\n");
        return 0;
    }
    printf("FAILED %d\n", g_failures);
    return 1;
}
