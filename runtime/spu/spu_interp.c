/* spu_interp.c — SPU interpreter core. See spu_interp.h.
 *
 * Decode tables are generated from the validated Python decoder
 * (tools/gen_spu_interp.py -> spu_interp_tables.inc); execute calls the SAME
 * spu_<mnemonic> helpers the lifter emits, so interpreted and lifted code are
 * bit-identical. Uncommon opcodes trap loudly (never silently wrong).
 */
#include "spu_native_owned.h"
#include "spu_interp.h"
#include "spu_helpers.h"
#include "spu_native_leaf.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

/* Channel ABI (runtime/spu/spu_channels.c); the lifter emits these same protos
 * into each generated spu_recomp.h — declared here to stay header-independent. */
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);

#include "spu_interp_tables.inc"   /* spu_op enum, spu_op_name[], spu_dec_* */

/* Work descriptor for the next SYS_SPU_THREAD_STOP_RECEIVE_EVENT service, set
 * by the event-port send that wakes a sim SPU. Single-slot: dispatch is
 * synchronous -- the sending PPU thread runs the SPU inline. */
uint32_t g_spu_pending_evt[3];
int      g_spu_pending_evt_valid;

enum { NATIVE_LEAVES = 32 };
static spu_native_leaf native_leaves[NATIVE_LEAVES];
static unsigned char native_unbudgeted[NATIVE_LEAVES];
static unsigned native_count;
static uint32_t native_entries[0x40000 / 128];
static atomic_flag native_lock = ATOMIC_FLAG_INIT;
static atomic_int native_frozen;
static int native_instrumented,native_calltrace;
static SPU_THREAD_LOCAL int native_busy;
static void native_lock_enter(void) {
    while (atomic_flag_test_and_set_explicit(&native_lock,memory_order_acquire)) {}
}
static void native_lock_leave(void) {
    atomic_flag_clear_explicit(&native_lock,memory_order_release);
}
static int native_register(const spu_native_leaf* leaf,int unbudgeted) {
    const char* error=NULL;
    native_lock_enter();
    if (atomic_load_explicit(&native_frozen,memory_order_relaxed)) error="registry frozen";
    else if (!leaf || !leaf->prepare || (leaf->begin&3) || (leaf->end&3) ||
             leaf->begin>=leaf->end || leaf->end>0x40000) error="invalid extent/callback";
    else if (native_count==NATIVE_LEAVES) error="registry full";
    else {
        for (unsigned i=0;i<native_count;i++)
            if (native_leaves[i].begin==leaf->begin && native_leaves[i].prepare==leaf->prepare)
                error="duplicate entry/callback";
        if (!error) {
            native_leaves[native_count]=*leaf;
            native_unbudgeted[native_count++]=(unsigned char)unbudgeted;
            native_entries[leaf->begin>>7] |= 1u<<((leaf->begin>>2)&31);
        }
    }
    native_lock_leave();
    if (error) fprintf(stderr,"[spu-native-leaf] registration rejected: %s\n",error);
    return error ? -1 : 0;
}
int spu_native_leaf_register(const spu_native_leaf* leaf) {
    return native_register(leaf,0);
}
int spu_native_leaf_register_unbudgeted(const spu_native_leaf* leaf) {
    return native_register(leaf,1);
}
static void native_freeze(void) {
    if (atomic_load_explicit(&native_frozen,memory_order_acquire)) return;
    native_lock_enter();
    if (!atomic_load_explicit(&native_frozen,memory_order_relaxed)) {
        /* Presence, including an empty value, is a startup option. Capture
         * before release publication; racing first callers do not recapture. */
        static const char* options[]={"SPU_LS_LOWREAD","SPU_LS_WATCH","SPU_SMC_WATCH",
            "SPU_PCHIST","SPU_TRACE_CALLS","SPU_BOUNDARY_DIR"};
        native_calltrace=getenv("SPU_CALLTRACE")!=NULL;
        native_instrumented=native_calltrace;
        for(unsigned i=0;i<sizeof(options)/sizeof(options[0]);i++)
            native_instrumented |= getenv(options[i])!=NULL;
        atomic_store_explicit(&native_frozen,1,memory_order_release);
    }
    native_lock_leave();
}
static void native_error(const char* reason) {
    fprintf(stderr,"[spu-native-leaf] invalid execution: %s\n",reason); abort();
}
static int native_ready(const spu_context* ctx) {
    return ctx->ls==ctx->ls_store && ctx->status==SPU_STATUS_RUNNING &&
        !ctx->list_stall_mask &&
        !(ctx->int_enable && (__atomic_load_n(&ctx->event_status,__ATOMIC_ACQUIRE)&ctx->event_mask));
}
static int native_try(spu_context* ctx,uint32_t stop,uint64_t remaining,int has_budget,
                      uint32_t* last,uint64_t* steps) {
    if (native_busy || !native_ready(ctx)) return 0;
    const uint32_t pc=ctx->pc;
    const uint32_t interrupt_enable=ctx->int_enable, event_mask=ctx->event_mask;
    const int depth=ctx->host_depth;
    spu_native_leaf_plan chosen={0};
    unsigned accepted=0;
    native_busy=1;
    for (unsigned i=0;i<native_count;i++) {
        const spu_native_leaf* leaf=&native_leaves[i];
        const int unbudgeted=native_unbudgeted[i];
        if (leaf->begin!=pc || (stop && stop>=leaf->begin && stop<leaf->end)) continue;
        if (unbudgeted && (has_budget || stop)) continue;
        spu_native_leaf_plan plan={0};
        int result=leaf->prepare(ctx,remaining,&plan);
        if (result<0 || result>1) native_error("prepare result");
        if (ctx->pc!=pc) native_error("prepare changed entry PC");
        if (!native_ready(ctx)) { native_busy=0; return 0; }
        if (!result) continue;
        if (!plan.execute || (plan.resume_pc&3) ||
            plan.resume_pc>=0x40000 || (plan.resume_pc>=leaf->begin && plan.resume_pc<leaf->end) ||
            (unbudgeted ? plan.instructions || plan.last_pc :
             !plan.instructions || (plan.last_pc&3) || plan.last_pc<leaf->begin || plan.last_pc>=leaf->end))
            native_error("invalid plan");
        if (plan.instructions>remaining) continue;
        if (++accepted>1) native_error("ambiguous guards");
        chosen=plan;
    }
    if (accepted && !native_ready(ctx)) accepted=0;
    if (accepted) {
        PS3_SPU_EXECUTION(PS3_SPU_NATIVE_BEGIN, ctx, pc, pc,
            chosen.instructions, PS3_SPU_NATIVE_LOGICAL_PC, (uintptr_t)chosen.execute);
        chosen.execute(ctx,&chosen);
        if (ctx->pc!=chosen.resume_pc || ctx->status!=SPU_STATUS_RUNNING ||
            ctx->ls!=ctx->ls_store || ctx->list_stall_mask || ctx->host_depth!=depth ||
            ctx->int_enable!=interrupt_enable || ctx->event_mask!=event_mask)
            native_error("continuation/status");
        PS3_SPU_EXECUTION(PS3_SPU_NATIVE_END, ctx, chosen.instructions ? chosen.last_pc : pc, ctx->pc,
            chosen.instructions, PS3_SPU_NATIVE_LOGICAL_PC, (uintptr_t)chosen.execute);
        if (chosen.instructions) *last=chosen.last_pc;
        *steps+=chosen.instructions;
    }
    native_busy=0;
    return accepted!=0;
}

/* ---- decode: 32-bit insn -> fields (mirrors spu_disasm.spu_decode order) ---- */
typedef struct {
    spu_op   op;
    uint8_t  rt, ra, rb, rc, ch;
    int32_t  imm;     /* sign/zero-extended immediate or shift, per format */
    uint32_t tgt;     /* absolute LS branch target (br-family) */
} spu_ins;

static int32_t sx(uint32_t v, int bits) {
    uint32_t m = 1u << (bits - 1);
    return (int32_t)((v ^ m) - m);
}

static void spu_decode1(uint32_t insn, uint32_t pc, spu_ins* d) {
    uint32_t op4 = (insn >> 28) & 0xF,  op7 = (insn >> 25) & 0x7F;
    uint32_t op8 = (insn >> 24) & 0xFF, op9 = (insn >> 23) & 0x1FF;
    uint32_t op10 = (insn >> 22) & 0x3FF, op11 = (insn >> 21) & 0x7FF;
    d->rt = insn & 0x7F; d->ra = (insn >> 7) & 0x7F;
    d->rb = (insn >> 14) & 0x7F; d->rc = (insn >> 21) & 0x7F;
    d->ch = (insn >> 7) & 0x7F; d->imm = 0; d->tgt = 0;
    int32_t i10 = sx((insn >> 14) & 0x3FF, 10);
    int32_t i16 = sx((insn >> 7) & 0xFFFF, 16);
    uint32_t i18 = (insn >> 7) & 0x3FFFF;

    /* RRR (4) */
    if ((d->op = spu_dec_rrr(op4)) != SPU_word) return;
    /* RI18 (7) */
    if ((d->op = spu_dec_ri18(op7)) != SPU_word) {
        d->imm = (d->op == SPU_ila) ? (int32_t)i18 : (int32_t)(i18 & 0xFFFF);
        return;
    }
    /* RI16 (9) — before RI10 (shared top-8-bit opcodes) */
    if ((d->op = spu_dec_ri16(op9)) != SPU_word) {
        switch (d->op) {
        case SPU_lqa: case SPU_stqa: d->tgt = ((uint32_t)(i16 & 0xFFFF) << 2) & 0x3FFF0; break;
        case SPU_lqr: case SPU_stqr: d->tgt = ((uint32_t)(i16 * 4) + pc) & 0x3FFF0; break;
        case SPU_br: case SPU_brsl: case SPU_brz: case SPU_brnz:
        case SPU_brhz: case SPU_brhnz: d->tgt = ((uint32_t)(i16 * 4) + pc) & 0x3FFFF; break;
        case SPU_bra: case SPU_brasl: d->tgt = ((uint32_t)(i16 * 4)) & 0x3FFFF; break;
        case SPU_il: d->imm = i16; break;
        default: d->imm = (int32_t)(i16 & 0xFFFF); break; /* ilh/ilhu/iohl */
        }
        return;
    }
    /* channel ops (op11) — before RI10 (wrch/shli op8 clash) */
    if (op11 == 0x00D) { d->op = SPU_rdch;   return; }
    if (op11 == 0x10D) { d->op = SPU_wrch;   return; }
    if (op11 == 0x00F) { d->op = SPU_rchcnt; return; }
    /* RI10 (8) */
    if ((d->op = spu_dec_ri10(op8)) != SPU_word) {
        d->imm = (d->op == SPU_lqd || d->op == SPU_stqd) ? (i10 << 4) : i10;
        return;
    }
    /* RI8 (10) float<->int conversions */
    if ((d->op = spu_dec_ri8(op10)) != SPU_word) { d->imm = (insn >> 14) & 0xFF; return; }
    /* RR (11) */
    if ((d->op = spu_dec_rr(op11)) != SPU_word) {
        if (d->op == SPU_stop) d->imm = insn & 0x3FFF;
        return;
    }
    /* RI7 (11) rotate/shift/gen-control immediates */
    if ((d->op = spu_dec_ri7(op11)) != SPU_word) {
        d->imm = spu_ri7_unsigned(d->op) ? d->rb : sx(d->rb, 7);
        return;
    }
    /* LSX (redundant with RR's lqx/stqx, kept for order-parity) */
    if ((d->op = spu_dec_lsx(op11)) != SPU_word) return;
    d->op = SPU_word;
}

/* spu_lifted_lookup() is defined by spu_fn_registry.c (the single owner of the
 * lifted-function table + eviction). Declared in spu_interp.h. */

/* ---- execute one instruction; returns 1 if the SPU stopped ---- */
#define A   (ctx->gpr[d.ra])
#define B   (ctx->gpr[d.rb])
#define T   (ctx->gpr[d.rt])          /* also the 3rd source in RRR */
#define DST (ctx->gpr[d.rt])          /* default destination */
#define DSTC (ctx->gpr[d.rc])         /* RRR destination */
#define I   (d.imm)
#define PREF(r) ((r)._u32[0])

static int spu_step(spu_context* ctx) {
    uint32_t pc = ctx->pc, m = pc & SPU_LS_MASK;
    const uint8_t* p = ctx->ls;
    uint32_t insn = ((uint32_t)p[m] << 24) | ((uint32_t)p[m+1] << 16)
                  | ((uint32_t)p[m+2] << 8) | p[m+3];
    spu_ins d; spu_decode1(insn, pc, &d);
    uint32_t next = (pc + 4) & 0x3FFFC;

    switch (d.op) {
    /* immediates / loads */
    case SPU_il:  DST = spu_il(I); break;
    case SPU_ilh: DST = spu_ilh((uint16_t)I); break;
    case SPU_ilhu: DST = spu_splat_u32((uint32_t)I << 16); break;
    case SPU_ila:  DST = spu_splat_u32((uint32_t)I & 0x3FFFF); break;
    case SPU_iohl: DST = spu_ori(DST, (int32_t)((uint32_t)I & 0xFFFF)); break;
    case SPU_fsmbi: DST = spu_fsmbi(I); break;
    case SPU_lqd: case SPU_lqx: case SPU_lqa: case SPU_lqr: {
        uint32_t a = (d.op==SPU_lqd) ? PREF(A)+ (uint32_t)I
                   : (d.op==SPU_lqx) ? PREF(A)+PREF(B) : d.tgt;
        DST = spu_ls_read128(ctx, a); break; }
    case SPU_stqd: case SPU_stqx: case SPU_stqa: case SPU_stqr: {
        uint32_t a = (d.op==SPU_stqd) ? PREF(A)+ (uint32_t)I
                   : (d.op==SPU_stqx) ? PREF(A)+PREF(B) : d.tgt;
        spu_ls_write128(ctx, a, DST); break; }
    /* integer arithmetic */
    case SPU_a:  DST = spu_a(A,B); break;
    case SPU_ai: DST = spu_ai(A,I); break;
    case SPU_sf: DST = spu_sf(A,B); break;
    case SPU_sfi:DST = spu_sfi(A,I); break;
    case SPU_ah: DST = spu_ah(A,B); break;
    case SPU_ahi:DST = spu_ahi(A,I); break;
    case SPU_sfh:DST = spu_sfh(A,B); break;
    case SPU_sfhi:DST = spu_sfhi(A,I); break;
    case SPU_addx: DST = spu_addx(A,B,DST); break;
    case SPU_sfx:  DST = spu_sfx(A,B,DST); break;
    case SPU_cg:   DST = spu_cg(A,B); break;
    case SPU_cgx:  DST = spu_cgx(A,B,DST); break;
    case SPU_mpyi: DST = spu_mpyi(A,I); break;
    case SPU_mpyui: DST = spu_mpyui(A,I); break;
    /* logical */
    case SPU_and: DST = spu_and(A,B); break;
    case SPU_or:  DST = spu_or(A,B); break;
    case SPU_xor: DST = spu_xor(A,B); break;
    case SPU_nand:DST = spu_nand(A,B); break;
    case SPU_nor: DST = spu_nor(A,B); break;
    case SPU_andc:DST = spu_andc(A,B); break;
    case SPU_orc: DST = spu_orc(A,B); break;
    case SPU_eqv: DST = spu_eqv(A,B); break;
    case SPU_andi:DST = spu_andi(A,I); break;
    case SPU_ori: DST = spu_ori(A,I); break;
    case SPU_xori:DST = spu_xori(A,I); break;
    case SPU_andbi:DST = spu_andbi(A,I); break;
    case SPU_andhi:DST = spu_andhi(A,I); break;
    case SPU_orhi: DST = spu_orhi(A,I); break;
    case SPU_orbi: DST = spu_orbi(A,I); break;
    case SPU_xorhi:DST = spu_xorhi(A,I); break;
    case SPU_xorbi:DST = spu_xorbi(A,I); break;
    case SPU_orx:  DST = spu_orx(A); break;
    /* compares */
    case SPU_ceq: DST = spu_ceq(A,B); break;
    case SPU_ceqh:DST = spu_ceqh(A,B); break;
    case SPU_ceqb:DST = spu_ceqb(A,B); break;
    case SPU_ceqi:DST = spu_ceqi(A,I); break;
    case SPU_cgt: DST = spu_cgt(A,B); break;
    case SPU_cgth:DST = spu_cgth(A,B); break;
    case SPU_cgtb:DST = spu_cgtb(A,B); break;
    case SPU_cgti:DST = spu_cgti(A,I); break;
    case SPU_clgt: DST = spu_clgt(A,B); break;
    case SPU_clgth:DST = spu_clgth(A,B); break;
    case SPU_clgtb:DST = spu_clgtb(A,B); break;
    case SPU_clgti:DST = spu_clgti(A,I); break;
    /* shifts / rotates */
    case SPU_shl:  DST = spu_shl(A,B); break;
    case SPU_shli: DST = spu_shli(A,I); break;
    case SPU_shlh: DST = spu_shlh(A,B); break;
    case SPU_shlhi:DST = spu_shlhi(A,I); break;
    case SPU_rot:  DST = spu_rot(A,B); break;
    case SPU_roti: DST = spu_roti(A,I); break;
    case SPU_rotm: DST = spu_rotm(A,B); break;
    case SPU_rotmi:DST = spu_rotmi(A,I); break;
    case SPU_rotma:DST = spu_rotma(A,B); break;
    case SPU_rotmai:DST = spu_rotmai(A,I); break;
    case SPU_roth: DST = spu_roth(A,B); break;
    case SPU_rothi:DST = spu_rothi(A,I); break;
    case SPU_shlqbi: DST = spu_shlqbi(A,B); break;
    case SPU_shlqbii:DST = spu_shlqbii(A,I); break;
    case SPU_shlqby: DST = spu_shlqby(A,B); break;
    case SPU_shlqbyi:DST = spu_shlqbyi(A,I); break;
    case SPU_rotqbi: DST = spu_rotqbi(A,B); break;
    case SPU_rotqbii:DST = spu_rotqbii(A,I); break;
    case SPU_rotqby: DST = spu_rotqby(A,B); break;
    case SPU_rotqbyi:DST = spu_rotqbyi(A,I); break;
    case SPU_rotqmby: DST = spu_rotqmby(A,B); break;
    case SPU_rotqmbyi:DST = spu_rotqmbyi(A,I); break;
    /* select / shuffle / masks / bitcount (RRR: dest=rc, srcs=ra,rb,rt) */
    case SPU_selb:  DSTC = spu_selb(A,B,T); break;
    case SPU_shufb: DSTC = spu_shufb(A,B,T); break;
    case SPU_fsm:  DST = spu_fsm(A); break;
    case SPU_fsmb: DST = spu_fsmb(A); break;
    case SPU_fsmh: DST = spu_fsmh(A); break;
    case SPU_gb:   DST = spu_gb(A); break;
    case SPU_gbb:  DST = spu_gbb(A); break;
    case SPU_gbh:  DST = spu_gbh(A); break;
    case SPU_clz:  DST = spu_clz(A); break;
    case SPU_cntb: DST = spu_cntb(A); break;
    /* gen-controls for insertion */
    case SPU_cbd: DST = spu_cbd(A,I); break;
    case SPU_chd: DST = spu_chd(A,I); break;
    case SPU_cwd: DST = spu_cwd(A,I); break;
    case SPU_cdd: DST = spu_cdd(A,I); break;
    case SPU_cbx: DST = spu_cbx(A,B); break;
    case SPU_chx: DST = spu_chx(A,B); break;
    case SPU_cwx: DST = spu_cwx(A,B); break;
    case SPU_cdx: DST = spu_cdx(A,B); break;
    /* multiply */
    case SPU_mpy:  DST = spu_mpy(A,B); break;
    case SPU_mpyu: DST = spu_mpyu(A,B); break;
    case SPU_mpyh: DST = spu_mpyh(A,B); break;
    case SPU_mpya: DSTC = spu_mpya(A,B,T); break;
    /* sign-extend */
    case SPU_xsbh: DST = spu_xsbh(A); break;
    case SPU_xshw: DST = spu_xshw(A); break;
    case SPU_xswd: DST = spu_xswd(A); break;
    /* float */
    case SPU_fa: DST = spu_fa(A,B); break;
    case SPU_fs: DST = spu_fs(A,B); break;
    case SPU_fm: DST = spu_fm(A,B); break;
    case SPU_fcgt: DST = spu_fcgt(A,B); break;
    case SPU_fceq: DST = spu_fceq(A,B); break;
    case SPU_cflts: DST = spu_cflts(A,I); break;
    case SPU_cfltu: DST = spu_cfltu(A,I); break;
    case SPU_csflt: DST = spu_csflt(A,I); break;
    case SPU_cuflt: DST = spu_cuflt(A,I); break;
    case SPU_frest:  DST = spu_frest(A); break;
    case SPU_frsqest:DST = spu_frsqest(A); break;
    case SPU_fi:     DST = spu_fi(A,B); break;
    case SPU_fcmgt:  DST = spu_fcmgt(A,B); break;
    case SPU_fcmeq:  DST = spu_fcmeq(A,B); break;
    case SPU_fesd:   DST = spu_fesd(A); break;
    case SPU_frds:   DST = spu_frds(A); break;
    case SPU_fma:  DSTC = spu_fma(A,B,T); break;    /* RRR: dest=rc */
    case SPU_fms:  DSTC = spu_fms(A,B,T); break;
    case SPU_fnms: DSTC = spu_fnms(A,B,T); break;
    /* immediate byte/halfword compares */
    case SPU_ceqbi: DST = spu_ceqbi(A,I); break;
    case SPU_cgtbi: DST = spu_cgtbi(A,I); break;
    case SPU_clgtbi:DST = spu_clgtbi(A,I); break;
    case SPU_ceqhi: DST = spu_ceqhi(A,I); break;
    case SPU_cgthi: DST = spu_cgthi(A,I); break;
    case SPU_clgthi:DST = spu_clgthi(A,I); break;
    /* integer / byte extras */
    case SPU_bg:    DST = spu_bg(A,B); break;
    case SPU_bgx:   DST = spu_bgx(A,B,DST); break;   /* RR: rt is accumulator + dest */
    case SPU_absdb: DST = spu_absdb(A,B); break;
    case SPU_avgb:  DST = spu_avgb(A,B); break;
    case SPU_sumb:  DST = spu_sumb(A,B); break;
    case SPU_mpys:  DST = spu_mpys(A,B); break;
    case SPU_mpyhh: DST = spu_mpyhh(A,B); break;
    case SPU_mpyhhu:DST = spu_mpyhhu(A,B); break;
    case SPU_mpyhha: DST = spu_mpyhha(A,B,DST); break;   /* RR (0x346): rt is accumulator + dest, NOT RRR */
    case SPU_mpyhhau:DST = spu_mpyhhau(A,B,DST); break;
    /* rotate / shift extras */
    case SPU_rothm:    DST = spu_rothm(A,B); break;
    case SPU_rothmi:   DST = spu_rothmi(A,I); break;
    case SPU_rotmah:   DST = spu_rothma(A,B); break;
    case SPU_rotmahi:  DST = spu_rotmahi(A,I); break;
    case SPU_rotqmbi:  DST = spu_rotqmbi(A,B); break;
    case SPU_rotqmbii: DST = spu_rotqmbii(A,(int)I); break;
    case SPU_rotqbybi: DST = spu_rotqbybi(A,B); break;
    case SPU_rotqmbybi:DST = spu_rotqmbybi(A,B); break;
    case SPU_shlqbybi: DST = spu_shlqbybi(A,B); break;
    /* channels */
    case SPU_wrch: spu_wrch(ctx, d.ch, DST); break;
    case SPU_rdch: DST = spu_rdch(ctx, d.ch); break;
    case SPU_rchcnt: DST = spu_pref_u32(spu_rchcnt(ctx, d.ch)); break;   /* count in the preferred word, rest zero (as the lifter) */
    /* hints / no-ops */
    case SPU_nop: case SPU_lnop: case SPU_sync: case SPU_dsync:
    case SPU_hbr: case SPU_hbra: case SPU_hbrr: case SPU_mtspr:
    case SPU_fscrwr: break;
    case SPU_mfspr: DST = spu_mfspr(A); break;      /* SPRs read as zero */
    case SPU_fscrrd: DST = spu_fscrrd(A); break;    /* FPSCR reads as zero */
    /* double precision */
    case SPU_dfa:  DST = spu_dfa(A,B); break;
    case SPU_dfs:  DST = spu_dfs(A,B); break;
    case SPU_dfm:  DST = spu_dfm(A,B); break;
    case SPU_dfma: DST = spu_dfma(A,B,DST); break;   /* RR: rt is accumulator + dest */
    case SPU_dfms: DST = spu_dfms(A,B,DST); break;
    case SPU_dfnms:DST = spu_dfnms(A,B,DST); break;
    case SPU_dfnma:DST = spu_dfnma(A,B,DST); break;
    case SPU_dfceq:  DST = spu_dfceq(A,B); break;
    case SPU_dfcgt:  DST = spu_dfcgt(A,B); break;
    case SPU_dfcmeq: DST = spu_dfcmeq(A,B); break;
    case SPU_dfcmgt: DST = spu_dfcmgt(A,B); break;
    case SPU_dftsv:  DST = spu_dftsv(A, (int32_t)(d.rb & 0x7F)); break;   /* I7 sits in the rb field */
    /* control flow */
    case SPU_br: case SPU_bra: next = d.tgt; break;
    case SPU_brsl: case SPU_brasl: DST = spu_link((pc + 4) & 0x3FFFC); next = d.tgt;
        { extern void spu_trace_call(uint32_t,uint32_t); spu_trace_call(pc, d.tgt); } break;
    case SPU_brz:  if (PREF(DST) == 0) next = d.tgt; break;
    case SPU_brnz: if (PREF(DST) != 0) next = d.tgt; break;
    case SPU_brhz: if ((PREF(DST) & 0xFFFF) == 0) next = d.tgt; break;
    case SPU_brhnz:if ((PREF(DST) & 0xFFFF) != 0) next = d.tgt; break;
    /* bi/iret carry the interrupt enable/disable bits (E = 0x40000 sets
     * int_enable, D = 0x80000 clears it), effective when the branch is taken,
     * exactly as the lifter decodes them. */
    #define SPU_IED_BITS() do { \
        if (insn & 0x40000u) ctx->int_enable = 1; \
        else if (insn & 0x80000u) ctx->int_enable = 0; \
    } while (0)
    case SPU_bi:   next = PREF(A) & 0x3FFFC; SPU_IED_BITS(); break;
    case SPU_bisl: { uint32_t tg = PREF(A) & 0x3FFFC;   /* read ra BEFORE the link write: rt may alias ra */
        DST = spu_link((pc + 4) & 0x3FFFC); next = tg; break; }
    case SPU_bisled: { uint32_t tg = PREF(A) & 0x3FFFC;   /* branch when an enabled event is pending */
        DST = spu_link((pc + 4) & 0x3FFFC);
        if ((__atomic_load_n(&ctx->event_status, __ATOMIC_ACQUIRE) & ctx->event_mask) != 0) {
            next = tg;
            SPU_IED_BITS(); }   /* E/D effective when taken, like the lift */
        break; }
    case SPU_iret: next = ctx->srr0 & 0x3FFFC;
        /* The handler preserves every register: on a return to the point the
         * interrupt was taken, restore the register file saved there. The
         * lifted dispatch path does this in spu_irq_regs_maybe_restore; an iret
         * executed here has to as well, or the snapshot stays armed and later
         * lands on an unrelated dispatch. */
        SPU_IED_BITS();
        if (ctx->irq_saved && next == (ctx->irq_resume_pc & SPU_LS_MASK)) {
            memcpy(ctx->gpr, ctx->irq_gpr, sizeof ctx->irq_gpr);
            ctx->irq_saved = 0;
        }
        break;
    case SPU_biz:  if (PREF(DST) == 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_binz: if (PREF(DST) != 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_bihz: if ((PREF(DST) & 0xFFFF) == 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_bihnz:if ((PREF(DST) & 0xFFFF) != 0) next = PREF(A) & 0x3FFFC; break;
    case SPU_stop: case SPU_stopd:
        ctx->pc = next; ctx->stop_code = (d.op == SPU_stopd) ? 0x3FFFu : (uint32_t)I;   /* stopd carries no code field: RPCS3 reports 0x3FFF */
        ctx->status = SPU_STATUS_STOPPED_BY_STOP; return 1;
    /* conditional halts (assertions): stop the SPU when the condition holds,
     * else continue. The preferred word of ra is compared. */
    case SPU_heq: case SPU_heqi: case SPU_hgt: case SPU_hgti:
    case SPU_hlgt: case SPU_hlgti: {
        uint32_t a = PREF(A), rhs = (d.op==SPU_heq||d.op==SPU_hgt||d.op==SPU_hlgt) ? PREF(B) : (uint32_t)I;
        int hit = (d.op==SPU_heq||d.op==SPU_heqi)   ? (a == rhs)
                : (d.op==SPU_hgt||d.op==SPU_hgti)   ? ((int32_t)a > (int32_t)rhs)
                :                                     (a > rhs);      /* hlgt/hlgti */
        if (hit) { extern void spu_trace_dump(uint32_t); spu_trace_dump(pc);
                   ctx->pc = pc; ctx->stop_code = 0x1000; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1; }
        break; }
    default:
        fprintf(stderr, "[spu_interp] unimplemented op '%s' (0x%08X) at LS 0x%05X\n",
                d.op < SPU_OP_COUNT ? spu_op_name[d.op] : "?", insn, pc);
        ctx->pc = pc; ctx->status = SPU_STATUS_STOPPED_BY_HALT; return 1;
    }
    ctx->pc = next;
#if PS3_EXECUTION_OBSERVATION
    switch (d.op) {
    case SPU_br: case SPU_bra: case SPU_brsl: case SPU_brasl:
    case SPU_brz: case SPU_brnz: case SPU_brhz: case SPU_brhnz:
    case SPU_bi: case SPU_bisl: case SPU_bisled: case SPU_iret:
    case SPU_biz: case SPU_binz: case SPU_bihz: case SPU_bihnz:
        PS3_SPU_EXECUTION(PS3_SPU_INTERPRETED_CONTROL, ctx, pc, next,
            (uint64_t)d.op, PS3_SPU_SOURCE_INSTRUCTION, 0);
        break;
    default: break;
    }
#endif
    return 0;
}

SPU_THREAD_LOCAL uint32_t g_spu_interp_last_pc = 0;
SPU_THREAD_LOCAL uint64_t g_spu_interp_steps   = 0;

/* Call-trace ring buffer for diagnosing SPU asserts (env SPU_CALLTRACE). */
#define SPU_TRACE_N 32
static struct { uint32_t from, to; } s_trace[SPU_TRACE_N];
static unsigned s_trace_i = 0;
void spu_trace_call(uint32_t from, uint32_t to) {
    s_trace[s_trace_i % SPU_TRACE_N].from = from;
    s_trace[s_trace_i % SPU_TRACE_N].to   = to;
    s_trace_i++;
}
void spu_trace_dump(uint32_t at) {
    /* A direct dump before any interpreter run must not freeze registration. */
    if (atomic_load_explicit(&native_frozen,memory_order_acquire) ?
        !native_calltrace : !getenv("SPU_CALLTRACE")) return;
    fprintf(stderr, "[SPU-TRACE] halt at LS 0x%05X; last %d calls (from->to):\n", at, SPU_TRACE_N);
    unsigned n = s_trace_i < SPU_TRACE_N ? s_trace_i : SPU_TRACE_N;
    for (unsigned k = 0; k < n; k++) {
        unsigned idx = (s_trace_i - n + k) % SPU_TRACE_N;
        fprintf(stderr, "   0x%05X -> 0x%05X\n", s_trace[idx].from, s_trace[idx].to);
    }
}

uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa) {
    return spu_interp_run_until(ctx, start_lsa, 0);
}

static uint32_t spu_interp_run_until_scoped_body(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa, uint64_t budget, int native_call) {
    native_freeze();
    ctx->pc = start_lsa & 0x3FFFC;
    ctx->status = SPU_STATUS_RUNNING;
    uint64_t steps = 0;
    PS3_SPU_EXECUTION(PS3_SPU_INTERPRETER_INVOKE, ctx, 0, ctx->pc, 0,
        PS3_SPU_SOURCE_UNAVAILABLE | PS3_SPU_TARGET_CONTEXT_PC, 0);
    /* YDKJ_SPU_TRACE=N: log the last N PCs into a ring buffer and dump them when the
     * interp halts -- shows the path to a branch-to-0 (the cri task/policy wall). */
    static int _tr=-1; if(_tr<0){const char*e=getenv("YDKJ_SPU_TRACE");_tr=e?atoi(e):0;}
    static uint64_t _cap=0; { static int _ci=0; if(!_ci){_ci=1; const char*e=getenv("SPU_STEPCAP"); _cap=e?strtoull(e,0,0):0;} }
    /* YDKJ_CRI_GATE1TRACE=N: cri decode task busy-spins in the validator func_00026E80
     * (LS 0x26E80..0x26F14), a straight-line leaf that returns r3 = 0 / 0x8041090F /
     * 0x80410909. Hand-decoding its selb/fsm/gb/ceqh lanes proved unreliable, so log the
     * ACTUAL runtime result + inputs the first N times it returns. Fires on range-exit
     * (pc left the fn), when r3 and the leaf's non-restored intermediates are still live. */
    static int _g1=-1; if(_g1<0){const char*e=getenv("YDKJ_CRI_GATE1TRACE");_g1=e?atoi(e):0;}
    static int _g1in=0;
    uint32_t ring[64]; int rc=0, rn=0;
    for (;;) {
        if (budget && steps>=budget) {
            g_spu_interp_steps=steps; return ctx->pc;
        }
        /* Owned-job mode must not consume an enabled IRQ window entirely
         * inside the policy interpreter before the drain can observe it. */
        if (spu_native_owned_policy(ctx) && ctx->int_enable && !ctx->irq_saved &&
            (ctx->event_status & ctx->event_mask))
            spu_take_interrupt(ctx,NULL);
        /* Match the lifted path: only synthetic HLE tasks may bypass the
         * resident policy. Real tasksets also store 0xA70 in their API table.
         * Preserve the legacy CRI return convention while invoking the HLE. */
        if (ctx->pc == 0xA70u) {
            uint32_t sc = ((uint32_t)ctx->ls[0x27C4]<<24)|((uint32_t)ctx->ls[0x27C5]<<16)
                        | ((uint32_t)ctx->ls[0x27C6]<<8) | ctx->ls[0x27C7];
            if (ctx->image_id == 22 || (ctx->policy_mode && sc == 0xA70u)) {
                extern void spu_spurs_taskset_syscall(spu_context*);
                int _save = ctx->image_id; ctx->image_id = 22;
                spu_spurs_taskset_syscall(ctx);
                ctx->image_id = _save;
                ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;   /* resume at link r0 */
                continue;
            }
        }
        /* Rejoin the compiled fast path only for images that HAVE lifted
         * functions (image_id >= 0). image_id < 0 = pure interpretation: an
         * un-lifted image (e.g. a title's raw SPU jobs) must never rejoin
         * another image's functions that happen to share an LS address. */
        /* ponytail: pc-only match; a recursive call through the same site would stop early -- compare r1 too if one shows up. */
        if (ctx->image_id >= 0 && stop_lsa && ctx->pc == (stop_lsa & SPU_LS_MASK)) {
            g_spu_interp_steps=steps; g_spu_interp_last_pc=ctx->pc; return ctx->pc;
        }
        /* Cheap empty-registry path; guards run only at explicit entry PCs.
         * Instrumented instruction streams always retain interpretation. */
        if (native_count && ctx->pc<0x40000 && !(ctx->pc&3) &&
            (native_entries[ctx->pc>>7] & (1u<<((ctx->pc>>2)&31))) &&
            !_tr && !_cap && !_g1 && !g_spu_ls_watch_n &&
            g_spu_ls_probe<=0 && g_spu_smc_watch<=0 &&
            !native_instrumented) {
            uint32_t last=g_spu_interp_last_pc;
            if (native_try(ctx,stop_lsa,(budget ? budget : UINT64_MAX)-steps,budget!=0,&last,&steps)) {
                g_spu_interp_last_pc=last;
                continue;
            }
        }
        int owned_mode=spu_native_owned_policy(ctx);
        int can_rejoin=!owned_mode || (!budget && (!stop_lsa || (native_call && ctx->host_depth>0 && stop_lsa==ctx->drain_ret_pc)) && !_tr && !_cap && !_g1 &&
            !native_instrumented && spu_native_owned_can_run(ctx,ctx->pc));
        if (can_rejoin && ctx->image_id >= 0 && spu_lifted_lookup(ctx, ctx->pc)) {
            g_spu_interp_steps=steps; g_spu_interp_last_pc=ctx->pc; return ctx->pc;
        }
        g_spu_interp_last_pc = ctx->pc;
        if (_g1>0) {
            int inr = (ctx->pc >= 0x26E80u && ctx->pc < 0x26F14u);
            if (inr) { _g1in = 1; }
            else if (_g1in) {
                _g1in = 0;
                #define GLB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
                fprintf(stderr,"[gate1] func_00026E80 ret r3=%08X | LS2FB0=%08X LS2FB4=%08X LS2FB8=%08X | LS1E0=%08X LS1E4=%08X LS1E8=%08X | r1=%05X r29=%08X r32=%08X r16=%08X r22=%08X r8=%08X r6=%08X\n",
                    ctx->gpr[3]._u32[0], GLB(0x2FB0),GLB(0x2FB4),GLB(0x2FB8), GLB(0x1E0),GLB(0x1E4),GLB(0x1E8),
                    ctx->gpr[1]._u32[0], ctx->gpr[29]._u32[0], ctx->gpr[32]._u32[0],
                    ctx->gpr[16]._u32[0], ctx->gpr[22]._u32[0], ctx->gpr[8]._u32[0], ctx->gpr[6]._u32[0]);
                #undef GLB
                fflush(stderr); _g1--;
            }
        }
        if (_tr>0) { ring[rc&63]=ctx->pc; rc++; if(rn<64)rn++; }
        steps++;
        /* SPU_STEPCAP=N: a task that never halts (infinite work/wait loop) never
         * dumps its ring. Force a one-shot dump after N steps to see where it loops. */
        if (_tr>0 && _cap && steps == _cap) {
            fprintf(stderr,"[spu-trace] STEPCAP pc=0x%05X after %llu steps; last %d PCs:",
                    ctx->pc, (unsigned long long)steps, rn);
            for(int k=rn;k>0;k--) fprintf(stderr," %05X", ring[(rc-k)&63]);
            fprintf(stderr,"\n");
            /* dump the LS 0x2700 SpursTasksetContext the poll reads -- compare to the
             * working RPCS3 SPU0 dump (RUNNING/READY/ENABLED=0x80000000, SIGNALLED=0). */
            #define LB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
            fprintf(stderr,"[spu-trace] LS ctx: RUN=%08X READY=%08X PEND=%08X ENA=%08X SIG=%08X WAIT=%08X | 2790=%08X 2794=%08X 27D8=%08X 2FB0=%08X\n",
                LB(0x2700),LB(0x2710),LB(0x2720),LB(0x2730),LB(0x2740),LB(0x2750),LB(0x2790),LB(0x2794),LB(0x27D8),LB(0x2FB0));
            #undef LB
            fflush(stderr); _tr--; g_spu_interp_steps=steps; return 0x2000u;
        }
        if (spu_step(ctx)) {
            /* stop 0x110 = SYS_SPU_THREAD_STOP_RECEIVE_EVENT. The worker writes
             * the SPU queue number to its out-mailbox, stops, and lv2 replies
             * with {CELL_OK, data1, data2, data3} in the in-mailbox -- which is
             * how these sim SPUs receive each frame's work-descriptor EA.
             * Treating the stop as "job finished" let them resume with nothing
             * in that structure and DMA their results to address 0.
             * Service it and keep running; with no event pending, fall through
             * to the old behaviour so nothing waits forever. */
            if (ctx->stop_code == 0x110 && g_spu_pending_evt_valid) {
                spu_channel_read(&ctx->ch_out_mbox);      /* the SPU queue key */
                ctx->rcv_evt[0] = 0;                      /* CELL_OK */
                ctx->rcv_evt[1] = g_spu_pending_evt[0];
                ctx->rcv_evt[2] = g_spu_pending_evt[1];
                ctx->rcv_evt[3] = g_spu_pending_evt[2];
                ctx->rcv_evt_n = 4; ctx->rcv_evt_i = 0;
                g_spu_pending_evt_valid = 0;
                ctx->status = SPU_STATUS_RUNNING;
                continue;
            }
            g_spu_interp_steps = steps;
            if (_tr>0) { fprintf(stderr,"[spu-trace] halt stop=0x%X pc=0x%05X after %llu steps; last %d PCs:",
                    ctx->stop_code, ctx->pc, (unsigned long long)steps, rn);
                for(int k=rn;k>0;k--) fprintf(stderr," %05X", ring[(rc-k)&63]);
                fprintf(stderr,"\n"); fflush(stderr); _tr--; }
            return ctx->stop_code; }
    }
}

void spu_dispatch(spu_context* ctx, uint32_t target) {
    for (;;) {
        target &= 0x3FFFC;
        spu_lifted_fn fn = spu_lifted_lookup(ctx, target);
        if (fn && (!spu_native_owned_policy(ctx) ||
                   spu_native_owned_can_run(ctx,target))) {
            ctx->pc = target;
            PS3_SPU_RESOLVED(ctx, fn);
            fn(ctx); return;
        }
        uint32_t next = spu_interp_run(ctx, target);
        if (ctx->status & (SPU_STATUS_STOPPED_BY_STOP | SPU_STATUS_STOPPED_BY_HALT))
            return;
        if ((next & 0x3FFFC) == target) return;   /* no-progress guard */
        target = next;
    }
}

uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa) {
    return spu_interp_run_until_budget(ctx,start_lsa,stop_lsa,0);
}
uint32_t spu_interp_run_until_budget(spu_context* ctx,uint32_t start_lsa,uint32_t stop_lsa,uint64_t budget) {
    uint64_t saved=spu_sf_simd_enter();
    uint32_t result=spu_interp_run_until_scoped_body(ctx,start_lsa,stop_lsa,budget,0);
    spu_sf_simd_leave(saved);
    return result;
}

/* A dispatcher-owned host return is distinct from an arbitrary canonical stop.
 * Authorization is a call-local value, so nested execution/nonlocal exits
 * cannot leak permission into another interpreter invocation. */
uint32_t spu_interp_run_native_call(spu_context* ctx,uint32_t start,uint32_t stop) {
    int authorized=spu_native_owned_policy(ctx) && ctx->host_depth>0 &&
        stop && stop==ctx->drain_ret_pc;
    uint64_t saved=spu_sf_simd_enter();
    uint32_t result=spu_interp_run_until_scoped_body(ctx,start,stop,0,authorized);
    spu_sf_simd_leave(saved);
    return result;
}
