#!/usr/bin/env python3
"""PPC 603e -> C lifter. Instruction translator (emit_insn) + generators.
emit_insn(pc, w) -> dict(code=[C stmts], ctl=None|('jmp',t)|('cond',expr,t)|('ret',expr|None)|('ind',expr|None,'tgt'),
                         link=bool, meta=(kind,reg), key=(op,xo) or None if unsupported)
meta kind: 0 none, 1 int load (reg seq from r[reg]), 2 brx32, 3 brx16, 4 fp32 load, 5 fp64 load
"""
import struct

def sx(v, bits): return v - (1 << bits) if v & (1 << (bits - 1)) else v
def mask32(mb, me):
    if mb <= me: return ((0xFFFFFFFF >> mb) & (0xFFFFFFFF << (31 - me))) & 0xFFFFFFFF
    return (~mask32(me + 1, mb - 1)) & 0xFFFFFFFF if True else 0
def H(v): return '0x%08xu' % (v & 0xFFFFFFFF)

class U(Exception): pass

def emit_insn(pc, w):
    op = w >> 26
    rt = rs = (w >> 21) & 31; ra = (w >> 16) & 31; rb = (w >> 11) & 31
    rc = w & 1; oe = (w >> 10) & 1; xo = (w >> 1) & 0x3ff; xo5 = (w >> 1) & 0x1f
    simm = sx(w & 0xFFFF, 16); uimm = w & 0xFFFF
    code = []; ctl = None; meta = (0, 0); key = (op, xo if op in (19, 31, 59, 63) else None)
    A = lambda: 'c->r[%d]' % ra
    EA0 = lambda d: ('(uint32_t)(%d)' % d) if ra == 0 else '(c->r[%d] + (uint32_t)(%d))' % (ra, d)
    def cr0(expr): code.append('c->cr[0] = cmp_s((int32_t)(%s), 0, c->xer);' % expr)
    def setrt(expr, dst, rcflag=rc):
        code.append('c->r[%d] = %s;' % (dst, expr))
        if rcflag: cr0('c->r[%d]' % dst)
    def FD(n): return 'c->f[%d].d' % n
    def fcr1():
        if rc: code.append('c->cr[1] = (c->fpscr >> 28) & 15;')
    def link():
        return ['c->lr = 0x%08xu;' % ((pc + 4) & 0xFFFFFFFF)] if (w & 1) else []
    def load(sz, signed, update, idx, kind=1):
        nonlocal meta
        ea = ('(%s + c->r[%d])' % ('0' if ra == 0 else 'c->r[%d]' % ra, rb)) if idx else EA0(simm)
        if update and ra == 0: raise U()
        code.append('{ uint32_t ea = %s;' % ea)
        v = 'LD%d(c, ea)' % sz
        if signed: v = '(uint32_t)(int32_t)(int%d_t)%s' % (sz, v)
        code.append('  c->r[%d] = %s;' % (rt, v))
        if update: code.append('  c->r[%d] = ea;' % ra)
        code.append('}'); meta = (kind, rt)
    def store(sz, update, idx):
        ea = ('(%s + c->r[%d])' % ('0' if ra == 0 else 'c->r[%d]' % ra, rb)) if idx else EA0(simm)
        if update and ra == 0: raise U()
        code.append('{ uint32_t ea = %s; ST%d(c, ea, (uint%d_t)c->r[%d]);' % (ea, sz, sz, rs) + (' c->r[%d] = ea;' % ra if update else '') + ' }')
    def fload(sz, update, idx):
        nonlocal meta
        ea = ('(%s + c->r[%d])' % ('0' if ra == 0 else 'c->r[%d]' % ra, rb)) if idx else EA0(simm)
        if update and ra == 0: raise U()
        code.append('{ uint32_t ea = %s;' % ea)
        code.append(('  %s = (double)f32_of_bits(LD32(c, ea));' if sz == 32 else '  c->f[%d].u = LD64(c, ea);') % ((FD(rt)) if sz == 32 else rt))
        if update: code.append('  c->r[%d] = ea;' % ra)
        code.append('}'); meta = (4 if sz == 32 else 5, rt)
    def fstore(sz, update, idx):
        ea = ('(%s + c->r[%d])' % ('0' if ra == 0 else 'c->r[%d]' % ra, rb)) if idx else EA0(simm)
        if update and ra == 0: raise U()
        v = 'bits_of_f32((float)%s)' % FD(rs) if sz == 32 else 'c->f[%d].u' % rs
        code.append('{ uint32_t ea = %s; ST%d(c, ea, %s);' % (ea, sz, v) + (' c->r[%d] = ea;' % ra if update else '') + ' }')
    try:
        if op == 14: setrt(('(uint32_t)%d' % simm) if ra == 0 else '%s + (uint32_t)%d' % (A(), simm), rt, 0)
        elif op == 15: setrt(H(simm << 16) if ra == 0 else '%s + %s' % (A(), H(simm << 16)), rt, 0)
        elif op == 12 or op == 13:
            code.append('{ uint32_t a = %s, b = %s; uint64_t s = (uint64_t)a + b; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), H(simm), rt))
            if op == 13: cr0('c->r[%d]' % rt)
        elif op == 8:
            code.append('{ uint32_t a = %s, b = %s; c->r[%d] = b - a; set_ca(c, b >= a); }' % (A(), H(simm), rt))
        elif op == 7: setrt('(uint32_t)((int32_t)%s * %d)' % (A(), simm), rt, 0)
        elif op == 11: code.append('c->cr[%d] = cmp_s((int32_t)%s, %d, c->xer);' % (rt >> 2, A(), simm))
        elif op == 10: code.append('c->cr[%d] = cmp_u(%s, %s, c->xer);' % (rt >> 2, A(), H(uimm)))
        elif op in (24, 25, 26, 27, 28, 29):
            imm = uimm << 16 if op in (25, 27, 29) else uimm
            f = {24: '|', 25: '|', 26: '^', 27: '^', 28: '&', 29: '&'}[op]
            code.append('c->r[%d] = c->r[%d] %s %s;' % (ra, rs, f, H(imm)))
            if op in (28, 29): cr0('c->r[%d]' % ra)
        elif op == 21:
            sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31
            code.append('c->r[%d] = rotl32(c->r[%d], %d) & %s;' % (ra, rs, sh, H(mask32(mb, me))))
            if rc: cr0('c->r[%d]' % ra)
        elif op == 20:
            sh, mb, me = rb, (w >> 6) & 31, (w >> 1) & 31; m = H(mask32(mb, me))
            code.append('c->r[%d] = (rotl32(c->r[%d], %d) & %s) | (c->r[%d] & ~%s);' % (ra, rs, sh, m, ra, m))
            if rc: cr0('c->r[%d]' % ra)
        elif op == 23:
            mb, me = (w >> 6) & 31, (w >> 1) & 31
            code.append('c->r[%d] = rotl32(c->r[%d], c->r[%d] & 31) & %s;' % (ra, rs, rb, H(mask32(mb, me))))
            if rc: cr0('c->r[%d]' % ra)
        elif op in (32, 33): load(32, 0, op == 33, 0)
        elif op in (34, 35): load(8, 0, op == 35, 0)
        elif op in (40, 41): load(16, 0, op == 41, 0)
        elif op in (42, 43): load(16, 1, op == 43, 0)
        elif op in (36, 37): store(32, op == 37, 0)
        elif op in (38, 39): store(8, op == 39, 0)
        elif op in (44, 45): store(16, op == 45, 0)
        elif op == 46:
            code.append('{ uint32_t ea = %s; for (int i = %d; i < 32; i++, ea += 4) c->r[i] = LD32(c, ea); }' % (EA0(simm), rt)); meta = (1, rt)
        elif op == 47:
            code.append('{ uint32_t ea = %s; for (int i = %d; i < 32; i++, ea += 4) ST32(c, ea, c->r[i]); }' % (EA0(simm), rs))
        elif op in (48, 49): fload(32, op == 49, 0)
        elif op in (50, 51): fload(64, op == 51, 0)
        elif op in (52, 53): fstore(32, op == 53, 0)
        elif op in (54, 55): fstore(64, op == 55, 0)
        elif op == 18:
            li = sx(w & 0x03FFFFFC, 26); t = (li if (w & 2) else pc + li) & 0xFFFFFFFF
            code += link(); ctl = ('jmp', t)
        elif op == 16:
            bo, bi = rt, ra; bd = sx(w & 0xFFFC, 16); t = (bd if (w & 2) else pc + bd) & 0xFFFFFFFF
            code.append('int tk;')
            conds = []
            if not (bo & 4): code.append('c->ctr--;'); conds.append('(c->ctr %s 0)' % ('==' if bo & 2 else '!='))
            if not (bo & 16): conds.append('(crbit(c, %d) == %d)' % (bi, (bo >> 3) & 1))
            code.append('tk = %s;' % (' && '.join(conds) if conds else '1')); code += link(); ctl = ('cond', 'tk', t)
        elif op == 19:
            if xo in (16, 528):
                bo, bi = rt, ra
                code.append('uint32_t tgt = %s; int tk;' % ('c->lr & ~3u' if xo == 16 else 'c->ctr & ~3u'))
                conds = []
                if xo == 16 and not (bo & 4): code.append('c->ctr--;'); conds.append('(c->ctr %s 0)' % ('==' if bo & 2 else '!='))
                if not (bo & 16): conds.append('(crbit(c, %d) == %d)' % (bi, (bo >> 3) & 1))
                code.append('tk = %s;' % (' && '.join(conds) if conds else '1')); code += link()
                ctl = ('ret' if xo == 16 else 'ind', 'tk', 'tgt')
            elif xo == 0: code.append('c->cr[%d] = c->cr[%d];' % (rt >> 2, ra >> 2))
            elif xo in (33, 129, 193, 225, 257, 289, 417, 449):
                f = {33: '!(a | b)', 129: 'a & !b', 193: 'a ^ b', 225: '!(a & b)', 257: 'a & b', 289: '!(a ^ b)', 417: 'a | !b', 449: 'a | b'}[xo]
                code.append('{ int a = crbit(c, %d), b = crbit(c, %d); setcrbit(c, %d, %s); }' % (ra, rb, rt, f))
            elif xo == 150: pass
            elif xo == 50: code.append('c->msr = c->spr[27];'); ctl = ('ind', None, 'c->spr[26] & ~3u')
            else: raise U()
        elif op == 31:
            if xo in (23, 55): load(32, 0, xo == 55, 1)
            elif xo in (87, 119): load(8, 0, xo == 119, 1)
            elif xo in (279, 311): load(16, 0, xo == 311, 1)
            elif xo in (343, 375): load(16, 1, xo == 375, 1)
            elif xo == 20: load(32, 0, 0, 1)
            elif xo == 534: load(32, 0, 0, 1, 2); code[-2] = '  c->r[%d] = bswap32(LD32(c, ea));' % rt
            elif xo == 790: load(16, 0, 0, 1, 3); code[-2] = '  c->r[%d] = bswap16(LD16(c, ea));' % rt
            elif xo in (151, 183): store(32, xo == 183, 1)
            elif xo in (215, 247): store(8, xo == 247, 1)
            elif xo in (407, 439): store(16, xo == 439, 1)
            elif xo == 662: code.append('{ uint32_t ea = %s; ST32(c, ea, bswap32(c->r[%d])); }' % (('(%s + c->r[%d])' % ('0' if ra == 0 else A(), rb)), rs))
            elif xo == 918: code.append('{ uint32_t ea = %s; ST16(c, ea, bswap16((uint16_t)c->r[%d])); }' % (('(%s + c->r[%d])' % ('0' if ra == 0 else A(), rb)), rs))
            elif xo == 150 + 0 and False: pass
            elif xo == 150 or xo == 0x96: raise U()
            elif xo in (535, 567): fload(32, xo == 567, 1)
            elif xo in (599, 631): fload(64, xo == 631, 1)
            elif xo in (663, 695): fstore(32, xo == 695, 1)
            elif xo in (727, 759): fstore(64, xo == 759, 1)
            elif xo == 983: code.append('{ uint32_t ea = %s; ST32(c, ea, (uint32_t)c->f[%d].u); }' % (('(%s + c->r[%d])' % ('0' if ra == 0 else A(), rb)), rs))
            elif xo == 0: code.append('c->cr[%d] = cmp_s((int32_t)%s, (int32_t)c->r[%d], c->xer);' % (rt >> 2, A(), rb))
            elif xo == 32: code.append('c->cr[%d] = cmp_u(%s, c->r[%d], c->xer);' % (rt >> 2, A(), rb))
            elif xo == 266 and not oe: setrt('%s + c->r[%d]' % (A(), rb), rt)
            elif xo == 266 and oe:
                code.append('{ uint32_t a = %s, b = c->r[%d], r = a + b; set_ov(c, (~(a ^ b) & (a ^ r)) >> 31); c->r[%d] = r; }' % (A(), rb, rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 40 and not oe: setrt('c->r[%d] - %s' % (rb, A()), rt)
            elif xo == 40 and oe:
                code.append('{ uint32_t a = %s, b = c->r[%d], r = b - a; set_ov(c, ((b ^ a) & (b ^ r)) >> 31); c->r[%d] = r; }' % (A(), rb, rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 10 and not oe:
                code.append('{ uint32_t a = %s, b = c->r[%d]; uint64_t s = (uint64_t)a + b; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rb, rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 8 and not oe:
                code.append('{ uint32_t a = %s, b = c->r[%d]; c->r[%d] = b - a; set_ca(c, b >= a); }' % (A(), rb, rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 138 and not oe:
                code.append('{ uint32_t a = %s, b = c->r[%d], ci = (c->xer >> 29) & 1; uint64_t s = (uint64_t)a + b + ci; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rb, rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 136 and not oe:
                code.append('{ uint32_t a = %s, b = c->r[%d], ci = (c->xer >> 29) & 1; uint64_t s = (uint64_t)(uint32_t)~a + b + ci; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rb, rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 202 and not oe:
                code.append('{ uint32_t a = %s, ci = (c->xer >> 29) & 1; uint64_t s = (uint64_t)a + ci; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 234 and not oe:
                code.append('{ uint32_t a = %s, ci = (c->xer >> 29) & 1; uint64_t s = (uint64_t)a + ci + 0xFFFFFFFFu; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 200 and not oe:
                code.append('{ uint32_t a = %s, ci = (c->xer >> 29) & 1; uint64_t s = (uint64_t)(uint32_t)~a + ci; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 232 and not oe:
                code.append('{ uint32_t a = %s, ci = (c->xer >> 29) & 1; uint64_t s = (uint64_t)(uint32_t)~a + ci + 0xFFFFFFFFu; c->r[%d] = (uint32_t)s; set_ca(c, s >> 32); }' % (A(), rt))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 104 and not oe: setrt('(uint32_t)(-(int32_t)%s)' % A(), rt)
            elif xo == 235:
                code.append('{ int64_t r = (int64_t)(int32_t)%s * (int32_t)c->r[%d]; c->r[%d] = (uint32_t)r;%s }' % (A(), rb, rt, ' set_ov(c, r != (int64_t)(int32_t)r);' if oe else ''))
                if rc: cr0('c->r[%d]' % rt)
            elif xo == 75: setrt('(uint32_t)(((int64_t)(int32_t)%s * (int32_t)c->r[%d]) >> 32)' % (A(), rb), rt)
            elif xo == 11: setrt('(uint32_t)(((uint64_t)%s * c->r[%d]) >> 32)' % (A(), rb), rt)
            elif xo == 491 and not oe: setrt('(c->r[%d] == 0) ? (%s < 0x80000000u ? 0u : 0xFFFFFFFFu) : ((c->r[%d] == 0xFFFFFFFFu && %s == 0x80000000u) ? 0xFFFFFFFFu : (uint32_t)((int32_t)%s / (int32_t)c->r[%d]))' % (rb, A(), rb, A(), A(), rb), rt)  # mirrors Supermodel's handling of the undefined cases
            elif xo == 459 and not oe: setrt('c->r[%d] ? %s / c->r[%d] : 0' % (rb, A(), rb), rt)
            elif xo == 28: setrt('c->r[%d] & c->r[%d]' % (rs, rb), ra)
            elif xo == 60: setrt('c->r[%d] & ~c->r[%d]' % (rs, rb), ra)
            elif xo == 444: setrt('c->r[%d] | c->r[%d]' % (rs, rb), ra)
            elif xo == 412: setrt('c->r[%d] | ~c->r[%d]' % (rs, rb), ra)
            elif xo == 316: setrt('c->r[%d] ^ c->r[%d]' % (rs, rb), ra)
            elif xo == 476: setrt('~(c->r[%d] & c->r[%d])' % (rs, rb), ra)
            elif xo == 124: setrt('~(c->r[%d] | c->r[%d])' % (rs, rb), ra)
            elif xo == 284: setrt('~(c->r[%d] ^ c->r[%d])' % (rs, rb), ra)
            elif xo == 24: setrt('(c->r[%d] & 0x20) ? 0 : c->r[%d] << (c->r[%d] & 31)' % (rb, rs, rb), ra)
            elif xo == 536: setrt('(c->r[%d] & 0x20) ? 0 : c->r[%d] >> (c->r[%d] & 31)' % (rb, rs, rb), ra)
            elif xo == 792:
                code.append('{ uint32_t n = c->r[%d] & 63, v = c->r[%d]; int32_t s = (int32_t)v; uint32_t r = n > 31 ? (uint32_t)(s >> 31) : (uint32_t)(s >> n); c->r[%d] = r; set_ca(c, s < 0 && (n > 31 ? 1 : (v & ((1u << n) - 1))) != 0 && n != 0); }' % (rb, rs, ra))
                if rc: cr0('c->r[%d]' % ra)
            elif xo == 824:
                code.append('{ uint32_t v = c->r[%d]; int32_t s = (int32_t)v; c->r[%d] = (uint32_t)(s >> %d); set_ca(c, s < 0 && %s); }' % (rs, ra, rb, '(v & %s) != 0' % H((1 << rb) - 1) if rb else '0'))
                if rc: cr0('c->r[%d]' % ra)
            elif xo == 26: setrt('cntlzw(c->r[%d])' % rs, ra)
            elif xo == 954: setrt('(uint32_t)(int32_t)(int8_t)c->r[%d]' % rs, ra)
            elif xo == 922: setrt('(uint32_t)(int32_t)(int16_t)c->r[%d]' % rs, ra)
            elif xo == 19: code.append('c->r[%d] = mfcr(c);' % rt)
            elif xo == 144:
                crm = (w >> 12) & 0xFF; code.append('mtcrf(c, 0x%02x, c->r[%d]);' % (crm, rs))
            elif xo == 83: code.append('c->r[%d] = c->msr;' % rt)
            elif xo == 146: code.append('c->msr = c->r[%d];' % rs)
            elif xo == 595: code.append('c->r[%d] = c->sr[%d];' % (rt, ra & 15))
            elif xo == 210: code.append('c->sr[%d] = c->r[%d];' % (ra & 15, rs))
            elif xo == 242: code.append('c->sr[c->r[%d] >> 28] = c->r[%d];' % (rb, rs))
            elif xo == 659: code.append('c->r[%d] = c->sr[c->r[%d] >> 28];' % (rt, rb))
            elif xo in (339, 467, 371):
                spr = ((w >> 16) & 31) | (((w >> 11) & 31) << 5)
                if xo == 371: spr = 268 if ra == 12 else 269; xo2 = 339
                else: xo2 = xo
                if xo2 == 339:
                    if spr in (22, 268, 269, 284, 285): code.append('c->r[%d] = TB_READ(c, %d, %d);' % (rt, spr, rt))
                    elif spr == 1: code.append('c->r[%d] = c->xer;' % rt)
                    elif spr == 8: code.append('c->r[%d] = c->lr;' % rt)
                    elif spr == 9: code.append('c->r[%d] = c->ctr;' % rt)
                    else: code.append('c->r[%d] = c->spr[%d];' % (rt, spr))
                else:
                    if spr == 1: code.append('c->xer = c->r[%d];' % rs)
                    elif spr == 8: code.append('c->lr = c->r[%d];' % rs)
                    elif spr == 9: code.append('c->ctr = c->r[%d];' % rs)
                    elif spr in (22, 268, 269, 284, 285): code.append('ppc_mtspr(c, %d, c->r[%d]);' % (spr, rs))
                    else: code.append('c->spr[%d] = c->r[%d];' % (spr, rs))
            elif xo in (598, 854, 86, 54, 246, 278, 982, 470, 306, 566, 1014, 982, 274, 54): pass
            else: raise U()
        elif op in (59, 63):
            fa, fb, fc = ra, rb, (w >> 6) & 31
            single = op == 59
            R = (lambda e: '(double)(float)(%s)' % e) if single else (lambda e: '(%s)' % e)
            if xo5 in (18, 20, 21, 25, 28, 29, 30, 31, 24, 23, 26) and not (op == 63 and xo in (0, 12, 14, 15, 32, 38, 40, 64, 70, 72, 134, 136, 264, 583, 711)):
                if xo5 == 21: code.append('%s = %s;' % (FD(rt), R('%s + %s' % (FD(fa), FD(fb)))))
                elif xo5 == 20: code.append('%s = %s;' % (FD(rt), R('%s - %s' % (FD(fa), FD(fb)))))
                elif xo5 == 18: code.append('%s = %s;' % (FD(rt), R('%s / %s' % (FD(fa), FD(fb)))))
                elif xo5 == 25: code.append('%s = %s;' % (FD(rt), R('%s * %s' % (FD(fa), FD(fc)))))
                elif xo5 == 29: code.append('%s = %s;' % (FD(rt), R('PPC_FMA(%s, %s, %s)' % (FD(fa), FD(fc), FD(fb)))))
                elif xo5 == 28: code.append('%s = %s;' % (FD(rt), R('PPC_FMA(%s, %s, -%s)' % (FD(fa), FD(fc), FD(fb)))))
                elif xo5 == 31: code.append('%s = %s;' % (FD(rt), R('-PPC_FMA(%s, %s, %s)' % (FD(fa), FD(fc), FD(fb)))))
                elif xo5 == 30: code.append('%s = %s;' % (FD(rt), R('-PPC_FMA(%s, %s, -%s)' % (FD(fa), FD(fc), FD(fb)))))
                elif xo5 == 23 and op == 63: code.append('%s = (isnan(%s) || %s < 0.0) ? %s : %s;' % (FD(rt), FD(fa), FD(fa), FD(fb), FD(fc)))
                elif xo5 == 24 and op == 59: code.append('%s = (double)(float)(1.0 / %s);' % (FD(rt), FD(fb)))
                elif xo5 == 26 and op == 63: code.append('%s = 1.0 / sqrt(%s);' % (FD(rt), FD(fb)))
                else: raise U()
                fcr1()
            elif op == 63 and xo == 0: code.append('c->cr[%d] = fcmp(%s, %s);' % (rt >> 2, FD(fa), FD(fb)))
            elif op == 63 and xo == 32: code.append('c->cr[%d] = fcmp(%s, %s);' % (rt >> 2, FD(fa), FD(fb)))
            elif op == 63 and xo == 12: code.append('%s = (double)(float)%s;' % (FD(rt), FD(fb))); fcr1()
            elif op == 63 and xo == 15: code.append('c->f[%d].u = fctiwz(%s);' % (rt, FD(fb))); fcr1()
            elif op == 63 and xo == 14: code.append('c->f[%d].u = fctiw(%s);' % (rt, FD(fb))); fcr1()
            elif op == 63 and xo == 72: code.append('c->f[%d].u = c->f[%d].u;' % (rt, rb)); fcr1()
            elif op == 63 and xo == 40: code.append('c->f[%d].u = c->f[%d].u ^ 0x8000000000000000ull;' % (rt, rb)); fcr1()
            elif op == 63 and xo == 264: code.append('c->f[%d].u = c->f[%d].u & 0x7FFFFFFFFFFFFFFFull;' % (rt, rb)); fcr1()
            elif op == 63 and xo == 136: code.append('c->f[%d].u = c->f[%d].u | 0x8000000000000000ull;' % (rt, rb)); fcr1()
            elif op == 63 and xo == 583: code.append('c->f[%d].u = c->fpscr;' % rt); fcr1()
            elif op == 63 and xo == 711:
                fm = (w >> 17) & 0xFF
                code.append('{ uint32_t m = 0; for (int i = 0; i < 8; i++) if (0x%02x & (0x80 >> i)) m |= 0xFu << (28 - 4 * i); c->fpscr = (c->fpscr & ~m) | ((uint32_t)c->f[%d].u & m); }' % (fm, rb)); fcr1()
            elif op == 63 and xo in (38, 70, 134, 64): raise U()
            else: raise U()
        elif op == 17 and (w & 2): code.append('ppc_sc(c, 0x%08xu);' % ((pc + 4) & 0xFFFFFFFF))
        elif op in (0, 1, 2, 3, 4, 5, 6, 9, 17, 22, 30, 56, 57, 58, 60, 61, 62): raise U()
        else: raise U()
    except U:
        return dict(code=['ppc_unimpl(c, 0x%08xu, 0x%08xu);' % (pc, w)], ctl=None, link=False, meta=(0, 0), key=key, unimpl=True)
    return dict(code=code, ctl=ctl, link=bool(w & 1) and op in (16, 18, 19), meta=meta, key=key, unimpl=False)

def decode_image(path, base=0):
    d = open(path, 'rb').read()
    return [struct.unpack('>I', d[i:i + 4])[0] for i in range(0, len(d) - 3, 4)]

# ---------- generators ----------
def gen_step_table(words, out_c, out_meta):
    """Switch-based exec_at(): executes ONE instruction, returns next pc. Used by the verify harness."""
    with open(out_c, 'w') as f:
        f.write('#include "ppc_rt.h"\nuint32_t exec_at(CPU *c, uint32_t pc) {\n uint32_t npc = pc + 4;\n switch (pc) {\n')
        unimp = 0
        for i, w in enumerate(words):
            pc = i * 4; r = emit_insn(pc, w)
            if r['unimpl']: unimp += 1
            f.write(' case 0x%x: {\n' % pc)
            for s in r['code']: f.write('  ' + s + '\n')
            ctl = r['ctl']
            if ctl:
                if ctl[0] == 'jmp': f.write('  npc = 0x%08xu;\n' % ctl[1])
                elif ctl[0] == 'cond': f.write('  if (%s) npc = 0x%08xu;\n' % (ctl[1], ctl[2]))
                elif ctl[0] in ('ret', 'ind'): f.write('  %s npc = %s;\n' % (('if (%s)' % ctl[1]) if ctl[1] else '', ctl[2]))
            f.write('  break; }\n')
        f.write(' default: ppc_unimpl(c, pc, 0); break;\n }\n return npc;\n}\n')
        f.write('const unsigned char meta_kind[] = {%s};\nconst unsigned char meta_reg[] = {%s};\n' % (
            ','.join(str(emit_insn(i * 4, w)['meta'][0]) for i, w in enumerate(words)),
            ','.join(str(emit_insn(i * 4, w)['meta'][1]) for i, w in enumerate(words))))
    return unimp

if __name__ == '__main__':
    import sys, collections
    words = decode_image(sys.argv[1])[:int(sys.argv[3], 16) // 4]
    n = gen_step_table(words, sys.argv[2], None)
    print('istruzioni:', len(words), 'non supportate:', n)

# ---------- real recompilation form: one C function per Ghidra function ----------
def gen_functions(mem, entries_in, edges, out_c, out_h, out_dispatch, data_mem=()):
    """mem: [(base, words)] code segments.  entries_in: iterable of function entry addresses (Ghidra +
    pointer seeds).  Function bodies are rediscovered here by recursive descent (so computed-goto
    targets and code Ghidra could not reach are covered).  data_mem: extra [(base, words)] images used
    only to read jump tables.  edges: optional observed indirect-branch targets."""
    import bisect
    segs = list(mem)
    def word_at(a):
        for base, ws in segs:
            if base <= a < base + 4 * len(ws): return ws[(a - base) // 4]
        return None
    def data_at(a):
        w_ = word_at(a)
        if w_ is not None: return w_
        for base, ws in data_mem:
            if base <= a < base + 4 * len(ws): return ws[(a - base) // 4]
        return None
    ind_targets = {}
    for a, b in edges: ind_targets.setdefault(a, set()).add(b)
    cache = {}
    def ins(pc):
        r = cache.get(pc)
        if r is None:
            w_ = word_at(pc)
            if w_ is None: return None
            r = cache[pc] = emit_insn(pc, w_)
        return r
    def reltable_targets(pc):
        known = {}; Ks = []
        for a in range(pc - 4 * 40, pc, 4):
            w_ = word_at(a)
            if w_ is None: continue
            op_ = w_ >> 26; rd = (w_ >> 21) & 31; ra_ = (w_ >> 16) & 31; imm = w_ & 0xFFFF
            simm_ = imm - 0x10000 if imm & 0x8000 else imm
            if op_ == 15: known[rd] = ((0 if ra_ == 0 else known[ra_]) + (simm_ << 16)) & 0xFFFFFFFF if (ra_ == 0 or known.get(ra_) is not None) else None
            elif op_ == 14:
                base_ = 0 if ra_ == 0 else known.get(ra_)
                known[rd] = None if base_ is None else (base_ + simm_) & 0xFFFFFFFF
                if known[rd] is not None: Ks.append(known[rd])
            elif op_ == 24:
                base_ = known.get(ra_); d_ = (w_ >> 16) & 31
                known[d_] = None if base_ is None else base_ | imm
                if known[d_] is not None: Ks.append(known[d_])
        out = set()
        for K in dict.fromkeys(Ks):
            if K & 3 or data_at(K) is None: continue
            for kind in ('rel', 'abs'):
                tg = []
                for i in range(1024):
                    wv = data_at(K + 4 * i)
                    if wv is None: break
                    t = (K + wv) & 0xFFFFFFFF if kind == 'rel' else wv
                    if t & 3 or word_at(t) is None or word_at(t) == 0: break
                    if abs(t - pc) > 0x20000: break          # switch targets stay near their function
                    tg.append(t)
                if len(tg) >= 2: out |= set(tg); break
        return out
    def terminal(r):
        c0 = r['ctl']
        if c0 is None: return False
        if r['key'] == (19, 50): return True
        if c0[0] == 'jmp': return not r['link']
        if c0[0] == 'ret': return (not r['link']) and 'tk = 1;' in r['code']
        if c0[0] == 'ind': return (not r['link']) and c0[1] is None
        return False
    entries = set(entries_in)
    for base, ws in segs:
        if base != 0: entries |= {e for e in entries if base <= e < base + 4 * len(ws)}
    bodies = {}; sites = {}
    work = sorted(entries)
    stats = dict(funcs=0, insns=0, unimpl=0, ind_sites=0, ind_resolved=0)
    while work:
        e = work.pop()
        if e in bodies or ins(e) is None: continue
        vis = set(); stack = [e]; calls = set()
        while stack:
            pc = stack.pop()
            while pc not in vis:
                r = ins(pc)
                if r is None or r['unimpl']: break
                vis.add(pc)
                c0 = r['ctl']; lk = r['link']
                if c0:
                    if c0[0] == 'jmp':
                        t = c0[1]
                        if lk: calls.add(t)
                        elif t in entries and t != e: calls.add(t)
                        elif ins(t) is not None: stack.append(t)
                    elif c0[0] == 'cond':
                        t = c0[2]
                        if lk: calls.add(t)
                        elif t in entries and t != e: calls.add(t)
                        elif ins(t) is not None: stack.append(t)
                    elif c0[0] == 'ind' and not lk:
                        ts = sorted(ind_targets.get(pc, ())) or sorted(reltable_targets(pc))
                        sites[pc] = ts
                        for t in ts:
                            if t in entries and t != e: calls.add(t)
                            elif ins(t) is not None: stack.append(t)
                    if terminal(r): break
                pc += 4
        bodies[e] = vis
        for t in calls:
            if t not in entries and ins(t) is not None: entries.add(t); work.append(t)
    eset = set(bodies)
    with open(out_h, 'w') as h:
        h.write('#pragma once\n#include "ppc_rt.h"\n')
        for e in sorted(eset): h.write('void fn_%08x(CPU *c);\n' % e)
    def call(t): return ('fn_%08x(c);' % t) if t in eset else ('dispatch(c, 0x%08xu);' % t)
    NPART = 16
    import contextlib
    parts = [open(out_c.replace('.c', '_%02d.c' % i), 'w') for i in range(NPART)]
    for o_ in parts: o_.write('#include "recomp.h"\n')
    with contextlib.ExitStack() as _stk:
        for _o in parts: _stk.enter_context(_o)
        for fi, e in enumerate(sorted(bodies)):
            o = parts[fi * NPART // max(1, len(bodies))]
            vis = bodies[e]; pcs = sorted(vis)
            labels = set(); backs = set(); blocks = {e}
            for pc in pcs:
                r = ins(pc); c0 = r['ctl']
                if c0:
                    tg = []
                    if c0[0] in ('jmp', 'cond') and not r['link']: tg = [c0[-1]]
                    elif c0[0] == 'ind' and not r['link']: tg = sites.get(pc, [])
                    for t in tg:
                        if t in vis and not (t in eset and t != e):
                            labels.add(t)
                            if t <= pc: backs.add(t)
                    blocks.add(pc + 4)
                if pc - 4 not in vis: blocks.add(pc)
            blocks = (blocks | labels) & vis
            cost = {}
            for b0 in sorted(blocks):
                n_ = 0; q = b0
                while q in vis and (q == b0 or q not in blocks): n_ += 1; q += 4
                cost[b0] = n_
            o.write('void fn_%08x(CPU *c) {\n' % e)
            for pc in pcs:
                r = ins(pc); stats['insns'] += 1; stats['unimpl'] += r['unimpl']
                if pc in labels: o.write('L_%08x:;\n' % pc)
                if pc in blocks:
                    o.write(' c->cyc -= %d;\n' % cost[pc])
                    if pc in backs or pc == e: o.write(' if (c->cyc < 0) ppc_safepoint(c, 0x%08xu);\n' % pc)
                o.write(' { /* %08x */\n' % pc)
                for s_ in r['code']: o.write('  ' + s_ + '\n')
                ctl, lk = r['ctl'], r['link']
                def intra(t): return t in vis and not (t in eset and t != e)
                if r['key'] == (19, 50):
                    o.write('  ppc_rfi(c); return;\n }\n'); 
                elif ctl:
                    k = ctl[0]
                    if k == 'jmp':
                        t = ctl[1]
                        if lk: o.write('  ' + call(t) + '\n')
                        elif intra(t): o.write('  goto L_%08x;\n' % t)
                        else: o.write('  { %s return; }\n' % call(t))
                    elif k == 'cond':
                        t = ctl[2]
                        if lk: o.write('  if (%s) { %s }\n' % (ctl[1], call(t)))
                        elif intra(t): o.write('  if (%s) goto L_%08x;\n' % (ctl[1], t))
                        else: o.write('  if (%s) { %s return; }\n' % (ctl[1], call(t)))
                    elif k == 'ret':
                        if lk: o.write('  if (%s) dispatch(c, tgt);\n' % ctl[1])
                        else: o.write('  if (%s) return;\n' % ctl[1])
                    elif k == 'ind':
                        cond = ctl[1] or '1'
                        if lk: o.write('  if (%s) dispatch(c, %s);\n' % (cond, ctl[2]))
                        else:
                            stats['ind_sites'] += 1
                            ts = sites.get(pc, [])
                            o.write('  if (%s) {\n' % cond)
                            if ts:
                                stats['ind_resolved'] += 1
                                o.write('   switch (%s) {\n' % ctl[2])
                                for t in ts:
                                    if intra(t): o.write('    case 0x%08xu: goto L_%08x;\n' % (t, t))
                                    else: o.write('    case 0x%08xu: %s return;\n' % (t, call(t)))
                                o.write('    default: dispatch(c, %s); return;\n   }\n' % ctl[2])
                            else: o.write('   dispatch(c, %s); return;\n' % ctl[2])
                            o.write('  }\n')
                if r['key'] != (19, 50): o.write(' }\n')
            o.write(' return;\n}\n\n')
            stats['funcs'] += 1
    entries = sorted(eset)
    with open(out_dispatch, 'w') as d:
        d.write('#include "recomp.h"\nvoid dispatch(CPU *c, uint32_t a) {\n switch (a) {\n')
        for e in entries: d.write('  case 0x%08xu: fn_%08x(c); return;\n' % (e, e))
        d.write('  default: ppc_unimpl(c, a, 0); return;\n }\n}\n')
    return stats
