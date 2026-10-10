/**
  * macro.c - pedal macro playback engine (contract: macro.h).
  *
  * Pure C11, <stdint.h> + "macro.h": no libc, no dynamic memory, non-blocking.
  * State machine per pedal: (idle) -> press step 0 for press_ms -> gap for
  * gap_ms -> press step 1 -> ... -> wrap to step 0.  Release at any point
  * returns to idle with zero outputs on the same tick.
  */
#include <stdint.h>
#include "macro.h"

void macro_reset(macro_state_t *st)
{
    st->active = 0u;
    st->pressing = 0u;
    st->step = 0u;
    st->reserved = 0u;
    st->deadline = 0u;
}

void macro_tick(macro_state_t *st,
                const uint8_t *mod_mask, const uint8_t *keycode, uint8_t count,
                uint16_t press_ms, uint16_t gap_ms,
                uint8_t held, uint32_t now_ms,
                uint8_t *out_mask, uint8_t *out_key)
{
    uint16_t press = (press_ms == 0u) ? 1u : press_ms;
    uint16_t gap = (gap_ms == 0u) ? 1u : gap_ms;

    *out_mask = 0u;
    *out_key = 0u;

    if ((held == 0u) || (count == 0u))
    {
        /* Release (or an empty step list): stop immediately, press nothing.
         * The next hold restarts the cycle at step 0. */
        st->active = 0u;
        st->pressing = 0u;
        return;
    }

    if (st->step >= count)
    {
        /* The step list shrank under a running cycle (MAC while held):
         * clamp instead of reading past the snapshot. */
        st->step = 0u;
    }

    if (st->active == 0u)
    {
        /* Rising edge: start the cycle at step 0, chord pressed. */
        st->active = 1u;
        st->step = 0u;
        st->pressing = 1u;
        st->deadline = now_ms + press;
    }
    else if ((int32_t)(st->deadline - now_ms) <= 0)
    {
        /* Phase boundary reached.  Exactly one advance per tick: after a
         * stall (flash save, ~20-40 ms) the cycle resumes instead of
         * fast-forwarding through the missed phases. */
        if (st->pressing != 0u)
        {
            st->pressing = 0u;
            st->deadline = now_ms + gap;
        }
        else
        {
            st->pressing = 1u;
            st->step = (uint8_t)((st->step + 1u) % count);
            st->deadline = now_ms + press;
        }
    }

    if (st->pressing != 0u)
    {
        *out_mask = mod_mask[st->step];
        *out_key = keycode[st->step];
    }
}
