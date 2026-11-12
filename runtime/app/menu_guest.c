/* Touch menus implemented inside the game's own menu code.
 *
 * A few lifted guest functions are replaced (the lifter output is post-processed by tools/apply_overrides.py:
 * the original body is renamed fn_XXXXXXXX_orig, every call site keeps calling fn_XXXXXXXX, which is defined here).
 * Each replacement applies the touch request to the game's own menu variables (cursor, page, confirm) and then runs
 * the original routine, so highlight, sounds, animations and the transition to the next screen are the game's own.
 *
 *   state 3   game select   cursor 0x22432c (0 championship, 1 practice)      logic fn_0002d26c
 *   state 6   course select cursor 0x1371c0 (carousel angle 0x1371b8)        logic fn_0000532c
 *   state 9   car select    cursor 0x224324 (0..5, extra page 6/7)           logic fn_00003f30 / fn_00004028, page fn_00047754
 *   state 12  gearbox       cursor 0x224334 (0 automatic, 1 manual)          logic fn_0003576c
 *   timer digits: fn_00046ce4 (hidden), countdown 0x22d5f4 (held: no time limit, also on the name entry, state 15)
 *
 * The host (play_main.cpp) turns taps into item numbers and sets the m3m_* requests; the guest side consumes them.
 */
#include "recomp.h"

volatile int m3m_on = 0;          /* no countdown, no timer digits */
volatile int m3m_touch = 0;       /* menus are driven by touch: the steering byte reads centre while a menu routine runs */
volatile int m3m_tap = -1;        /* item tapped (screen specific), consumed by the screen it was meant for */
volatile int m3m_tapState = -1;   /* game state the tap was computed for */
volatile int m3m_back = 0;        /* go back one page */
volatile int m3m_extras = 0;      /* the extra cars are shown on the car screen (read by the host for the tap areas) */
volatile int m3m_page = 0;        /* toggle the extra cars page */

#define ST_ADDR   0x22435cu
#define TIMER     0x22d5f4u
#define STEER     0x4a9cf0u       /* menu steering byte (0x80 = centre) */
#define GAS       0x4a9cf1u       /* menu pedal byte (> 0x80 = pressed) */
#define TIMER_HOLD 0x21c          /* the value every selection screen starts with */

static void sound(CPU *c, int id) { uint32_t lr = c->lr, r3 = c->r[3]; c->r[3] = (uint32_t)id; c->lr = 0; fn_00001788(c); c->lr = lr; c->r[3] = r3; }

static int s_sel = -1, s_selState = -1;   /* item highlighted by the last tap on this screen: only a tap on it confirms */
static int take_tap(CPU *c) {     /* the pending tap if it was meant for the current screen */
    int st = (int)LD32(c, ST_ADDR);
    if (st != s_selState) { s_selState = st; s_sel = -1; }
    int t = m3m_tap;
    if (t < 0) return -1;
    m3m_tap = -1;
    return st == m3m_tapState ? t : -1;
}
/* tap t on a screen whose cursor is cur: 1 = confirm, 0 = (re)select */
static int tap_confirms(int t, int cur) { int ok = t == cur && s_sel == t; s_sel = t; return ok; }

/* Runs an original menu routine. confirm: the pedal is reported pressed (and the release/arm flag set) for this call only.
   With touch menus the steering byte reads centre (the cursor is set directly) and the countdown is held. */
static void run(CPU *c, void (*orig)(CPU *), int confirm, uint32_t armAddr) {
    uint8_t steer = LD8(c, STEER), gas = LD8(c, GAS);
    if (m3m_on) ST32(c, TIMER, TIMER_HOLD);
    if (m3m_touch) ST8(c, STEER, 0x80);
    if (confirm) { ST32(c, armAddr, 1); ST8(c, GAS, 0xff); }
    orig(c);
    ST8(c, STEER, steer); ST8(c, GAS, gas);
}

/* Back one page: the game is put into the transition state that builds the previous screen, exactly as when moving forward. */
static void go_back(CPU *c, int st) {
    uint32_t lr = c->lr;
    if (st == 9 && LD32(c, 0x22432c) == 1) {   /* practice: car -> course select (state 4 builds it when its timer has run out) */
        ST32(c, TIMER, 0); ST32(c, ST_ADDR, 4);
    } else if (st == 6 || st == 9) {     /* course / championship car -> game select (state 1 reloads the game select background) */
        ST8(c, 0x1d1380, 5); ST32(c, ST_ADDR, 1);
    } else if (st == 12) {               /* gearbox -> car select: what state 7 does when it leaves the course page */
        ST32(c, 0x4a9650, 0x80000000u);
        c->r[3] = 0xf10f8000u; c->r[4] = 0x800; c->lr = 0; fn_00066c54(c);
        c->lr = 0; fn_00003c10(c);
        ST8(c, 0x1d1380, 5);
        ST32(c, TIMER, TIMER_HOLD);
        ST32(c, ST_ADDR, 8);
    }
    c->lr = lr;
}

static int back_pending(CPU *c, int st) {
    if (!m3m_back) return 0;
    m3m_back = 0;
    if ((int)LD32(c, ST_ADDR) != st) return 0;
    go_back(c, st);
    return 1;
}

/* ---- timer digits ---- */
void fn_00046ce4(CPU *c) {
    if (m3m_on) {
        uint32_t s = LD32(c, ST_ADDR);
        if (s == 3 || s == 6 || s == 9 || s == 12) return;
        if (s == 15) { ST32(c, TIMER, 0x5dc); return; }   /* name entry: no time limit either (END finishes it) */
    }
    fn_00046ce4_orig(c);
}

/* ---- game select ---- */
void fn_0002d26c(CPU *c) {
    int t = take_tap(c), confirm = 0;
    if (m3m_back) m3m_back = 0;                  /* nothing before this page */
    if (t == 0 || t == 1) {
        int cur = (int)LD32(c, 0x22432c);
        if (tap_confirms(t, cur)) confirm = 1;
        else if (t != cur) { ST32(c, 0x22432c, (uint32_t)t); sound(c, 10); }
    }
    run(c, fn_0002d26c_orig, confirm, 0x1adbe8);
}

/* ---- course select (practice) ---- */
/* A tap on a side course turns the carousel the way the cabinet wheel does: the steering byte is pushed to one side for a few
   frames and released, the game's own code spins the cards and settles on the next course (sound and picture swap included). */
static int s_spin = 0, s_spinDir = 0;
void fn_0000532c(CPU *c) {
    if (back_pending(c, 6)) { s_spin = 0; return; }
    int t = take_tap(c), confirm = 0;
    int locked = LD32(c, 0x1371d0) != 0, moving = LD32(c, 0x1371bc) != 0;
    if (t >= 0 && t < 4 && !locked && !moving && s_spin == 0) {
        int cur = (int)LD32(c, 0x1371c0) & 3;
        if (tap_confirms(t, cur)) confirm = 1;
        else if (t != cur) {
            int d = (t - cur) & 3;                       /* 1: right card, 3: left card, 2: the one at the back */
            s_spinDir = d == 3 ? -1 : 1;                 /* wheel right = next course */
            s_spin = d == 2 ? 30 : 7;                    /* push long enough to pass one or two courses, then the game settles */
        }
    }
    if (s_spin > 0) {
        s_spin--;
        uint8_t steer = LD8(c, STEER);
        uint32_t tm = m3m_touch; m3m_touch = 0;          /* run() must not centre the wheel this frame */
        ST8(c, STEER, (uint8_t)(s_spinDir > 0 ? 0x80 + 0x60 : 0x80 - 0x60));
        run(c, fn_0000532c_orig, 0, 0x1371b4);
        m3m_touch = tm;
        ST8(c, STEER, steer);
        return;
    }
    run(c, fn_0000532c_orig, confirm, 0x1371b4);
}

/* ---- car select ---- */
static void car_tap(CPU *c, void (*orig)(CPU *), int lo, int hi) {
    int t = take_tap(c), confirm = 0;
    if (t >= lo && t <= hi && LD32(c, 0x224378) == 0) {
        int cur = (int)LD32(c, 0x224324);
        if (tap_confirms(t, cur)) confirm = 1;
        else if (t != cur) { ST32(c, 0x224324, (uint32_t)t); sound(c, 10); }
    }
    run(c, orig, confirm, 0x137180);
}
void fn_00003f30(CPU *c) { car_tap(c, fn_00003f30_orig, 0, m3m_extras ? 7 : 5); }   /* six cars, eight with the extra ones */
void fn_00004028(CPU *c) { car_tap(c, fn_00004028_orig, 6, 7); }   /* page 2: the two extra cars */

/* All eight cars on one screen. With the extra cars unlocked the game keeps them on a second page (camera pan reached with a
   button combination). Here every card is pushed a little further back (smaller) with its on-screen centre unchanged, and the two
   extra cards go in the space this frees at the bottom centre; the cursor covers 0..7. Drawing (fn_00004224), highlight, confirm
   and the fly-out are the game's own. Positions: x, y, z floats per card at 0x47d530 + 12*i (i 0..5) and 0x47d578 + 12*(i-6). */
static const float Z1 = 2.7f, KP = 1.15f, EX = 0.82f, EY = -1.15f;   /* depth of the cards (orig 1.5), x/y spread, extra card centres: tuned so no card touches another */
static int extras_unlocked(CPU *c) { uint32_t lr = c->lr, r3 = c->r[3]; c->lr = 0; fn_0004bcf0(c); int u = c->r[3] != 0; c->lr = lr; c->r[3] = r3; return u; }
static void layout8(CPU *c) {
    float k = KP;
    for (int i = 0; i < 6; i++) {
        uint32_t a = 0x47d530 + 12 * i;
        ST32(c, a + 0, bits_of_f32(f32_of_bits(LD32(c, a + 0)) * k));
        ST32(c, a + 4, bits_of_f32(f32_of_bits(LD32(c, a + 4)) * k));
        ST32(c, a + 8, bits_of_f32(Z1));
    }
    ST32(c, 0x47d578, bits_of_f32(-EX * k)); ST32(c, 0x47d57c, bits_of_f32(EY * k)); ST32(c, 0x47d580, bits_of_f32(Z1));   /* card 6 */
    ST32(c, 0x47d584, bits_of_f32(EX * k));  ST32(c, 0x47d588, bits_of_f32(EY * k)); ST32(c, 0x47d58c, bits_of_f32(Z1));   /* card 7 */
    ST32(c, 0x47d4ac, LD32(c, 0xe9640)); ST32(c, 0x47d4b8, LD32(c, 0xe9640));   /* start turned away, like the others (state 8 turns them in) */
}
/* car screen set-up (entering it forward or with BACK) */
void fn_00003c10(CPU *c) {
    fn_00003c10_orig(c);
    m3m_extras = extras_unlocked(c);
    if (m3m_extras) layout8(c);
}
/* cars fly out after the confirm: the game moves only the cards of the current page, move the extra ones with them */
void fn_000479b4(CPU *c) {
    if (m3m_extras && LD32(c, 0x224370) == 0) {
        for (int i = 6; i < 8; i++) {
            uint32_t a = 0x47d530 + 12 * i;
            float x = f32_of_bits(LD32(c, a)), dz = f32_of_bits(LD32(c, 0xf2050)), dx = f32_of_bits(LD32(c, 0xf2058));
            ST32(c, a, bits_of_f32(x < 0 ? x - dx : x + dx));
            ST32(c, a + 8, bits_of_f32(f32_of_bits(LD32(c, a + 8)) + dz));
        }
    }
    fn_000479b4_orig(c);
}

void fn_00047754(CPU *c) {
    if (back_pending(c, 9)) return;
    m3m_page = 0;
    if (m3m_extras) { ST32(c, 0x224370, 0); ST32(c, 0x224378, 0); }   /* one page: never the pan to the second one */
    if (m3m_on) ST32(c, TIMER, TIMER_HOLD);
    fn_00047754_orig(c);
}

/* ---- gearbox ---- */
void fn_0003576c(CPU *c) {
    if (back_pending(c, 12)) return;
    int t = take_tap(c), confirm = 0;
    if (t == 0 || t == 1) {
        int cur = (int)LD32(c, 0x224334);
        if (tap_confirms(t, cur)) confirm = 1;
        else if (t != cur) { ST32(c, 0x224334, (uint32_t)t); sound(c, 10); }
    }
    run(c, fn_0003576c_orig, confirm, 0x1afd6c);
}

/* ---- car screen entrance (state 8): the game turns the six cards in one after the other; the two extra cards follow the same way ---- */
#define ROT(i) (0x47d464u + 12u * (i))
void fn_0004758c(CPU *c) {
    fn_0004758c_orig(c);
    if (!m3m_extras || LD32(c, ST_ADDR) != 8) return;
    float step = f32_of_bits(LD32(c, 0xf202c)), end = f32_of_bits(LD32(c, 0xf2030)), next = f32_of_bits(LD32(c, 0xf2034));
    int finished = LD32(c, 0x1d135c) != 0;
    int go6 = finished || ((int)LD32(c, 0x224340) >= 6 && f32_of_bits(LD32(c, ROT(5))) >= next);
    float r6 = f32_of_bits(LD32(c, ROT(6))), r7 = f32_of_bits(LD32(c, ROT(7)));
    if (go6 && r6 < end) { r6 += step; if (r6 > end) r6 = end; ST32(c, ROT(6), bits_of_f32(r6)); }
    int go7 = go6 && r6 >= next;
    if (go7 && r7 < end) { r7 += step; if (r7 > end) r7 = end; ST32(c, ROT(7), bits_of_f32(r7)); }
    if (finished && (r6 < end || r7 < end) && (int)LD32(c, TIMER) < 2) ST32(c, TIMER, 2);   /* the screen opens when they are in place too */
    /* draw them (the game draws the extra cards only on the car screen itself); a card not started yet stays hidden */
    uint32_t x6 = LD32(c, 0x47d578), x7 = LD32(c, 0x47d584), lr = c->lr;
    if (!go6) ST32(c, 0x47d578, bits_of_f32(1000.0f));
    if (!go7) ST32(c, 0x47d584, bits_of_f32(1000.0f));
    c->lr = 0; fn_00004224(c); c->lr = lr;
    ST32(c, 0x47d578, x6); ST32(c, 0x47d584, x7);
}

/* ---- name entry (state 15): BACK returns to the gearbox page ---- */
void fn_00047efc(CPU *c) {
    if (m3m_back) {
        m3m_back = 0;
        if (LD32(c, ST_ADDR) == 15) {
            uint32_t lr = c->lr;
            /* the car screen background back into the 2D layer (as state 8 loads it), then state 10 rebuilds the gearbox page */
            c->r[3] = 4; c->lr = 0; fn_00074ec8(c);
            c->r[3] = 0; c->r[4] = 0; c->r[5] = LD32(c, 0x1d132c); c->r[6] = 0; c->r[7] = 0xf10fc000u; c->lr = 0; fn_00068384(c);
            c->r[3] = 0; c->lr = 0; fn_00074ec8(c);
            c->lr = lr;
            ST32(c, TIMER, 0);
            ST32(c, ST_ADDR, 10);
            return;
        }
    }
    fn_00047efc_orig(c);
}
