// Host test: checks d-pad orientation, reading direction, dual-page steps and
// that rapid/held/overlapping input can never turn a page backwards.
//   gcc -O1 -Itools/stubs -Isource -o dir_test tools/dir_test.c source/pdf.c source/id3art.c source/epub.c tools/stubs/stubs.c
#define main enhanced_main
#include "enhanced.c"
#undef main
#include <assert.h>

static int clamp_page(int page, int step, int n) {
    int np = page + step;
    if (np < 0) np = 0;
    if (np > n - 1) np = n - 1;
    return np;
}

int main(void) {
    for (int view = 0; view < V_COUNT; view++) {
        int rot = rot_of(view), dual = is_dual(view);
        u32 l, r, u, d;
        vkeys(view, &l, &r, &u, &d);
        u32 physical_left = rot == 0 ? KEY_LEFT : rot == 1 ? KEY_UP : KEY_DOWN;
        u32 physical_right = rot == 0 ? KEY_RIGHT : rot == 1 ? KEY_DOWN : KEY_UP;
        assert(l == physical_left && r == physical_right);
        assert((l & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN)) == l);

        for (int rtl = 0; rtl < 2; rtl++) {
            int want_next = dual ? 2 : 1;
            assert(dpad_step(view, rtl, l) == (rtl ? want_next : -want_next));
            assert(dpad_step(view, rtl, r) == (rtl ? -want_next : want_next));
            assert(dpad_step(view, rtl, physical_left) == dpad_step(view, rtl, l));
            assert(dpad_step(view, rtl, physical_right) == dpad_step(view, rtl, r));
            assert(dpad_step(view, rtl, u) == 0 && dpad_step(view, rtl, d) == 0);

            // Only edges count: a held key (no new press this frame) never steps.
            assert(dpad_step(view, rtl, 0) == 0);

            // Both opposing directions in one snapshot is ambiguous. It used to
            // resolve to the "previous page" branch, which is exactly the
            // occasional backwards flick. It must now be ignored.
            assert(dpad_step(view, rtl, l | r) == 0);
            assert(dpad_step(view, rtl, l | r | u) == 0);
            assert(dpad_step(view, rtl, l | r | u | d) == 0);
            assert(dpad_step(view, rtl, KEY_A | KEY_B) == 0);

            // A stale press replayed next to a fresh one (what the old
            // `keysDown() | g_pend` merge produced) must not invert either.
            u32 stale_left = l, fresh_right = r;
            assert(dpad_step(view, rtl, stale_left | fresh_right) == 0);
        }
    }

    // Rapid alternating flicks in the default, unrotated top-to-bottom layout:
    // every tap moves exactly one page in the direction of the key, no repeats
    // and no inversions, across the whole page range.
    {
        int view = V_PORTRAIT, rtl = 1;                  // cfg default: view 0, rtl 1
        u32 l, r, u, d;
        vkeys(view, &l, &r, &u, &d);
        int n = 40, page = 20, taps = 0;
        for (int i = 0; i < 400; i++) {
            u32 key = (i & 1) ? l : r;
            int want = (i & 1) ? 1 : -1;
            int step = dpad_step(view, rtl, key);
            assert(step == want);
            int np = clamp_page(page, step, n);
            assert(np == page + want);                   // never clamped here
            assert((np - page) == want);                 // direction preserved
            page = np;
            if (page < 10 || page > 30) page = 20;       // keep away from the ends
            taps++;
        }
        assert(taps == 400);

        // Same in left-to-right reading: the two keys simply swap roles.
        for (int i = 0; i < 200; i++) {
            u32 key = (i & 1) ? r : l;
            int want = (i & 1) ? 1 : -1;
            assert(dpad_step(view, 0, key) == want);
        }
    }

    // Repeated taps of the same key each turn one page: no debounce loss, no
    // double step. A held key produces no further edges, so no runaway turns.
    for (int view = 0; view < V_COUNT; view++)
        for (int rtl = 0; rtl < 2; rtl++)
            for (int tap = 0; tap < 20; tap++) {
                u32 l, r, u, d;
                vkeys(view, &l, &r, &u, &d);
                u32 key = rtl ? l : r;
                int step = dpad_step(view, rtl, key);
                assert(step == (is_dual(view) ? 2 : 1));
                assert(dpad_step(view, rtl, 0) == 0);    // held frames in between
            }

    // Single-page and dual-page must agree on direction; dual only steps wider.
    for (int rtl = 0; rtl < 2; rtl++) {
        u32 l, r, u, d;
        vkeys(V_PORTRAIT, &l, &r, &u, &d);
        int single = dpad_step(V_PORTRAIT, rtl, l);
        u32 dl, dr, du, dd;
        vkeys(V_DUAL_L, &dl, &dr, &du, &dd);
        int dual = dpad_step(V_DUAL_L, rtl, dl);
        assert((single > 0) == (dual > 0) && (single < 0) == (dual < 0));
        assert(dual == single * 2);
    }

    puts("direction mapping: OK");
    return 0;
}
