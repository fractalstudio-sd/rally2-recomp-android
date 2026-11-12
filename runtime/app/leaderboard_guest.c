/* Online leaderboard: the game's own record-keeping routine is hooked; after it ran, the finished race is handed to the
 * host as one JSON line (the host queues it, the Android side uploads it).
 *
 * fn_0002bd60 runs at the end of a race and files the result in the game's ranking tables:
 *   0x22cf14 mode (0/1 championship, >= 2 practice)      0x22cf1c name (3 characters)
 *   0x22cf28 car (0..7)   0x22cf2c gearbox (0 AT, 1 MT)   0x22cf48 course of a practice run
 *   0x22d07c race time (1/6000 s)   0x22d604 race completed   0x22d60c result not valid for the records
 *   0x22cf60 -> course of each championship stage, 0x22cfa4 stages driven, 0x22d088 + 4*i time of stage i
 * Names of cars and courses are read from the game's own tables (the user's ROM), not from this file. */
#include "recomp.h"
#include <stdio.h>
#include <stdlib.h>

void lb_submit(const char *json);          /* host: queue one result */

#define MODE   0x22cf14u
#define NAME   0x22cf1cu
#define CAR    0x22cf28u
#define GEAR   0x22cf2cu
#define PCOURSE 0x22cf48u
#define STAGES 0x22cf60u
#define NSTAGE 0x22cfa4u
#define STIME  0x22d088u
#define TOTAL  0x22d07cu
#define DONE   0x22d604u
#define INVAL  0x22d60cu
#define CARNAMES 0xf23ecu                   /* 8 pointers to 7-character names */
#define COURSENAMES 0xf27b8u                /* pointers to 10-character names, indexed by course 0..5 */

static void rom_str(CPU *c, uint32_t ptrTab, uint32_t idx, int max, char *out) {
    uint32_t p = LD32(c, ptrTab + 4 * idx); int n = 0;
    if (p < 0x800000u) for (; n < max; n++) { uint8_t ch = LD8(c, p + (uint32_t)n); if (!ch || ch < 32 || ch > 126 || ch == '"' || ch == '\\') break; out[n] = (char)ch; }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = 0;
}

static void submit(CPU *c, const char *kind, int course, uint32_t time) {
    char name[4], car[16], crs[16], js[384];
    for (int i = 0; i < 3; i++) { uint8_t ch = LD8(c, NAME + (uint32_t)i) & 0x7f; name[i] = (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ? (char)ch : '.'; }
    name[3] = 0;
    uint32_t carIdx = LD32(c, CAR) & 7, gear = LD32(c, GEAR);
    rom_str(c, CARNAMES, carIdx, 12, car);
    if (course >= 0) rom_str(c, COURSENAMES, (uint32_t)course % 6, 12, crs); else crs[0] = 0;
    snprintf(js, sizeof js, "{\"kind\":\"%s\",\"course\":%d,\"courseName\":\"%s\",\"car\":%u,\"carName\":\"%s\",\"gear\":\"%s\",\"mode\":%u,\"name\":\"%s\",\"time\":%u}",
             kind, course, crs, carIdx, car, gear == 1 ? "MT" : "AT", LD32(c, MODE), name, time);
    lb_submit(js);
}

void fn_0002bd60(CPU *c) {
    uint32_t mode = LD32(c, MODE), done = LD32(c, DONE), inval = LD32(c, INVAL), total = LD32(c, TOTAL);
    uint32_t nst = LD32(c, NSTAGE), stp = LD32(c, STAGES);
    if (getenv("LB_DEBUG")) fprintf(stderr, "[lb] race end: mode %u done %u inval %u total %u stages %u\n", mode, done, inval, total, nst);
    fn_0002bd60_orig(c);                    /* the game files its own records first, unchanged */
    if (inval) return;
    if (mode < 2) {                         /* championship: the total, and every stage driven */
        if (done && total) submit(c, "championship", -1, total);
        if (nst > 8) nst = 8;
        for (uint32_t i = 0; i < nst && stp < 0x800000u; i++) {
            uint32_t t = LD32(c, STIME + 4 * i), crs = LD32(c, stp + 4 * i);
            if (t && crs < 12) submit(c, "stage", (int)crs, t);
        }
    } else if (done && total) {            /* practice: one course */
        uint32_t crs = LD32(c, PCOURSE);
        if (crs < 12) submit(c, "practice", (int)crs, total);
    }
}
