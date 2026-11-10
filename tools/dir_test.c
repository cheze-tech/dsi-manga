// Host test: checks d-pad orientation, reading direction and dual-page steps.
//   gcc -O1 -Itools/stubs -Isource -o dir_test tools/dir_test.c source/pdf.c source/id3art.c tools/stubs/stubs.c
#define main enhanced_main
#include "enhanced.c"
#undef main
#include <assert.h>

int main(void) {
    for (int view = 0; view < V_COUNT; view++) {
        int rot = rot_of(view), dual = is_dual(view);
        u32 l, r, u, d;
        vkeys(view, &l, &r, &u, &d);
        u32 physical_left = rot == 0 ? KEY_LEFT : rot == 1 ? KEY_UP : KEY_DOWN;
        u32 physical_right = rot == 0 ? KEY_RIGHT : rot == 1 ? KEY_DOWN : KEY_UP;
        assert(l == physical_left && r == physical_right);
        for (int rtl = 0; rtl < 2; rtl++) {
            int want_next = dual ? 2 : 1;
            assert(dpad_step(view, rtl, l) == (rtl ? want_next : -want_next));
            assert(dpad_step(view, rtl, r) == (rtl ? -want_next : want_next));
            assert(dpad_step(view, rtl, physical_left) == dpad_step(view, rtl, l));
            assert(dpad_step(view, rtl, physical_right) == dpad_step(view, rtl, r));
            assert(dpad_step(view, rtl, u) == 0 && dpad_step(view, rtl, d) == 0);
        }
    }
    puts("direction mapping: OK");
    return 0;
}
