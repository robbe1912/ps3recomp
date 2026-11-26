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
#ifdef _WIN32
#include <windows.h>
#endif

/* Channel ABI (runtime/spu/spu_channels.c); the lifter emits these same protos
 * into each generated spu_recomp.h — declared here to stay header-independent. */
u128 spu_rdch(spu_context* ctx, uint32_t channel);
void spu_wrch(spu_context* ctx, uint32_t channel, u128 value);
uint32_t spu_rchcnt(spu_context* ctx, uint32_t channel);
extern void spu_indirect_branch(spu_context*);   /* spu_channels.c (resolver) */

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
    uint32_t next = pc + 4;

    switch (d.op) {
    /* immediates / loads */
    case SPU_il:  DST = spu_splat_u32((uint32_t)(int32_t)I); break;
    case SPU_ilh: DST = spu_ilh(I); break;
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
    case SPU_mpyui:DST = spu_mpyui(A,I); break;
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
    case SPU_rothmi: DST = spu_rothmi(A,I); break;
    case SPU_rotmahi:DST = spu_rotmahi(A,I); break;
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
    case SPU_dfa: DST = spu_dfa(A,B); break;
    case SPU_dfs: DST = spu_dfs(A,B); break;
    case SPU_dfm: DST = spu_dfm(A,B); break;
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
    case SPU_absdb: DST = spu_absdb(A,B); break;
    case SPU_avgb:  DST = spu_avgb(A,B); break;
    case SPU_sumb:  DST = spu_sumb(A,B); break;
    case SPU_mpys:  DST = spu_mpys(A,B); break;
    case SPU_mpyhh: DST = spu_mpyhh(A,B); break;
    case SPU_mpyhhu:DST = spu_mpyhhu(A,B); break;
    case SPU_mpyhha:DSTC = spu_mpyhha(A,B,T); break;  /* RRR */
    /* rotate / shift extras */
    case SPU_rothm:    DST = spu_rothm(A,B); break;
    case SPU_rotqmbi:  DST = spu_rotqmbi(A,B); break;
    case SPU_rotqmbii: DST = spu_rotqmbii(A,(int)I); break;
    case SPU_rotqbybi: DST = spu_rotqbybi(A,B); break;
    case SPU_rotqmbybi:DST = spu_rotqmbybi(A,B); break;
    case SPU_shlqbybi: DST = spu_shlqbybi(A,B); break;
    /* channels */
    case SPU_wrch: spu_wrch(ctx, d.ch, DST); break;
    case SPU_rdch: DST = spu_rdch(ctx, d.ch); break;
    case SPU_rchcnt: DST = spu_splat_u32(spu_rchcnt(ctx, d.ch)); break;
    /* hints / no-ops */
    case SPU_nop: case SPU_lnop: case SPU_sync: case SPU_dsync:
    case SPU_hbr: case SPU_hbra: case SPU_hbrr: case SPU_mtspr:
    case SPU_mfspr: case SPU_fscrrd: case SPU_fscrwr: break;
    /* control flow */
    case SPU_br: case SPU_bra: next = d.tgt; break;
    case SPU_brsl: case SPU_brasl: DST = spu_link(pc + 4); next = d.tgt;
        { extern void spu_trace_call(uint32_t,uint32_t); spu_trace_call(pc, d.tgt); } break;
    case SPU_brz:  if (PREF(DST) == 0) next = d.tgt; break;
    case SPU_brnz: if (PREF(DST) != 0) next = d.tgt; break;
    case SPU_brhz: if ((PREF(DST) & 0xFFFF) == 0) next = d.tgt; break;
    case SPU_brhnz:if ((PREF(DST) & 0xFFFF) != 0) next = d.tgt; break;
    /* bi* interrupt-control bits mirror the lifted path (spu_channels.c):
     * E (0x40000) sets int_enable, D (0x80000) clears it. A branch to srr0
     * with interrupts re-enabled is the interrupt RETURN: bail to the host
     * dispatcher so the register file saved at TAKE is restored
     * (spu_irq_regs_maybe_restore). Branching in-place keeps the handler's
     * regs and derails the continuation -- the MS1 wwsjob PM run#53 wedged
     * exactly there: StallAck fired, iret was swallowed, and the loop after
     * srr0 spun forever on derailed registers. */
#define SPU_IRET_BAIL(tgt) do { \
        if (insn & 0x40000u) ctx->int_enable = 1; \
        if (insn & 0x80000u) ctx->int_enable = 0; \
        if (ctx->irq_saved && ctx->int_enable && \
            (tgt) == (ctx->irq_resume_pc & 0x3FFFC) && (tgt)) { \
            ctx->pc = (tgt); ctx->status = SPU_STATUS_RUNNING; \
            ctx->stop_code = 0; return 2; } } while (0)
    case SPU_bi:   next = PREF(A) & 0x3FFFC; SPU_IRET_BAIL(next); break;
    case SPU_bisl: DST = spu_link(pc + 4); next = PREF(A) & 0x3FFFC;
        if (insn & 0x40000u) ctx->int_enable = 1;
        if (insn & 0x80000u) ctx->int_enable = 0;
        break;
    case SPU_iret: ctx->int_enable = 1; next = ctx->srr0 & 0x3FFFC;
        ctx->pc = next; ctx->status = SPU_STATUS_RUNNING; ctx->stop_code = 0;
        return 2;
    case SPU_biz:  if (PREF(DST) == 0) { next = PREF(A) & 0x3FFFC; SPU_IRET_BAIL(next); } break;
    case SPU_binz: if (PREF(DST) != 0) { next = PREF(A) & 0x3FFFC; SPU_IRET_BAIL(next); } break;
    case SPU_bihz: if ((PREF(DST) & 0xFFFF) == 0) { next = PREF(A) & 0x3FFFC; SPU_IRET_BAIL(next); } break;
    case SPU_bihnz:if ((PREF(DST) & 0xFFFF) != 0) { next = PREF(A) & 0x3FFFC; SPU_IRET_BAIL(next); } break;
#undef SPU_IRET_BAIL
    case SPU_stop: case SPU_stopd:
        /* Hardening (Fix A, interpreter side): a policy excursion executing
         * a ZERO word read it as stop 0x0000 and killed the lane
         * (native_usercheck.err:4502 "synthesised stop 0x0000 at LS
         * 0x39004"). A real policy stop carries a code; zero stops only
         * come from virgin LS. Bail recoverably to the TAKE-time register
         * file at srr0, like SPU_IRET_BAIL. */
        if (ctx->policy_mode && I == 0 && ctx->irq_saved &&
            spu_pm_recoverable_bail(ctx, pc, 0))
            return 2;
        ctx->pc = next; ctx->stop_code = (uint32_t)I;
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
uint64_t g_spu_interp_budget = 0;   /* host-armed step ceiling, 0 = off */
int g_spu_interp_budget_hit = 0;
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
static uint32_t pm_gate_lw(spu_context* c, uint32_t a) {
    return ((uint32_t)c->ls[a & 0x3FFFCu] << 24)
         | ((uint32_t)c->ls[(a + 1) & 0x3FFFFu] << 16)
         | ((uint32_t)c->ls[(a + 2) & 0x3FFFFu] << 8)
         |  (uint32_t)c->ls[(a + 3) & 0x3FFFFu];
}
extern void spu_halt(spu_context*);   /* spu_channels.c (guard yield below) */


uint32_t spu_interp_run(spu_context* ctx, uint32_t start_lsa) {
    return spu_interp_run_until(ctx, start_lsa, 0);
}

static uint32_t interp_run_until_impl(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa, uint64_t local_budget, int native_call) {
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
    /* SPU_KH_TRACE=1: see the [kh] hook below. */
    static int s_kh=-1; if(s_kh<0){const char*e=getenv("SPU_KH_TRACE");s_kh=(e&&e[0]=='1')?1:0;}
    static int s_khn=0;
    for (;;) {
        if (local_budget && steps>=local_budget) { g_spu_interp_steps=steps; return ctx->pc; }
        /* An enable/disable window may span only a few instructions. Deliver
         * an unmasked pending interrupt before the next instruction, including
         * a kernel-service transfer; a periodic scheduler poll can miss it. */
        if (ctx->int_enable && !ctx->irq_saved &&
            (ctx->event_status & ctx->event_mask))
            spu_take_interrupt(ctx, NULL);
        /* Match the lifted path: only synthetic HLE tasks may bypass the
         * resident policy. Real tasksets also store 0xA70 in their API table.
         * Preserve the legacy CRI return convention while invoking the HLE.
         * The third arm mirrors spu_indirect_branch's "standalone" rule: a
         * non-policy context (MS1's img-2 ATRAC task and its interpreted
         * codec) whose syscallAddr is 0xA70 and with nothing lifted there
         * takes the HLE too. */
        if (ctx->pc == 0xA70u) {
            uint32_t sc = ((uint32_t)ctx->ls[0x27C4]<<24)|((uint32_t)ctx->ls[0x27C5]<<16)
                        | ((uint32_t)ctx->ls[0x27C6]<<8) | ctx->ls[0x27C7];
            if (ctx->image_id == 22 || (ctx->policy_mode && sc == 0xA70u) ||
                (sc == 0xA70u && !ctx->policy_mode && !spu_lifted_lookup(ctx, 0xA70u))) {
                extern void spu_spurs_taskset_syscall(spu_context*);
                int _save = ctx->image_id; ctx->image_id = 22;
                spu_spurs_taskset_syscall(ctx);
                ctx->image_id = _save;
                ctx->pc = ctx->gpr[0]._u32[0] & SPU_LS_MASK;   /* resume at link r0 */
                continue;
            }
        }

        /* SPURS kernel services: exit-to-kernel / select-workload sit at
         * reserved LS addresses the HLE resolver intercepts -- there is no
         * code at them in a policy lane's LS (the real kernel would be there).
         * Interpreted policy flow MUST hand back instead of executing the
         * zeros: the wwsjob PM's select bisl stepped 0x9D0->0x9D4 and the
         * synthesised stop there killed every Execute phase. */
        if (ctx->policy_mode &&
            (ctx->pc == SPURS_PM_EXIT_TO_KERNEL_LS ||
             ctx->pc == SPURS_PM_SELECT_WORKLOAD_LS)) {
            g_spu_interp_steps = steps; g_spu_interp_last_pc = ctx->pc;
            /* Round 30: the ONLY legit wi_state clearer besides
             * spu_context_init. Counted so a run can verify no other site
             * zeroes it behind our back (bg_287's wi==0-at-pass-entry
             * mystery). */
            { static int s_kh = 0;
              if (s_kh++ < 8 || (s_kh % 4096) == 0)
                  fprintf(stderr, "[walkin] HANDOFF wi=%d->0 img=%d #%d\n",
                          ctx->wi_state, ctx->wi_img, s_kh),
                  fflush(stderr); }
            ctx->pm_designed = 1;   /* designed cooperative handoff (R33) */
            ctx->wi_link_pending = 0;   /* kernel cycle over (R34) */
            if (ctx->pm_win_touched) {
                static int s_wh = 0;
                if (s_wh++ < 24)
                    fprintf(stderr, "[pm-window] EXIT class=handoff pc=0x%05X "
                            "wi=%d 12A0=0x%08X 12C0=0x%08X 12D0=0x%08X\n",
                            (uint32_t)ctx->pc & SPU_LS_MASK, ctx->wi_state,
                            pm_gate_lw(ctx, 0x12A0), pm_gate_lw(ctx, 0x12C0),
                            pm_gate_lw(ctx, 0x12D0)),
                    fflush(stderr);
            }
            return ctx->pc;
        }
        /* Round 14 [pm-gate]: the wwsjob PM's state-2->3 advance decoded
         * from spurspm_46EAABFB56B7B1B5_11136.bin:
         *   0x2BD8: handler 0x21A0(8) must return nonzero, then 0x1A08
         *           scans (12D0==0 ? 2 : 12D0-1) slot words at a table
         *           derived from the 0xC00 descriptor; ZERO slots are
         *           SKIPPED (brz 0x1A2C); a nonzero slot -> 0x2C18 launch.
         *           ALL-ZERO table -> r3==0 -> 0x12A0 never becomes 3
         *           (= the kExecuteCommands park; bg_269 invariant).
         *   0x2940: launch scan over the 0xEB0 table tests bit 0x40000000
         *           per entry; none set -> no claim (img 210 never
         *           dispatches).
         * Log the decision points while interpreted. */
        if (ctx->policy_mode) {
            static int _gt = -1;
            if (_gt < 0) { const char* e = getenv("SPU_PM_GATE_TRACE");
                           _gt = (e && *e == '0') ? 0 : 1; }
            /* Round 28: walk-in span tracking, outside _gt so the retire
             * guard runs with gate tracing off. wi_state transitions on
             * every fetch: 1 while pc is inside a staged module span, 2
             * once it has left toward PM code, 0 at kernel handoff (above).
             * A retire/prep (0x2EF0) reached with wi_state==2 means the PM
             * is about to re-prepare a slot while a walk-in module from
             * this cycle is still logically in flight -- on hardware the
             * kernel would not preempt the SPU mid-job here; our
             * per-quantum restart does. SPU_PM_RETIRE_GUARD=0 rolls back. */
            { uint32_t wpc28 = (uint32_t)ctx->pc & SPU_LS_MASK;
              int win28 = 0, wslot28 = -1;
              for (int i = 0; i < 4; i++)
                  if (ctx->resident_code[i].size
                      && wpc28 >= ctx->resident_code[i].lsa
                      && wpc28 < ctx->resident_code[i].lsa
                                   + ctx->resident_code[i].size) {
                      win28 = 1; wslot28 = i; break;
                  }
              if (wpc28 >= 0x2EF0u && wpc28 < 0x3078u) ctx->pm_win_touched = 1;
              if (wpc28 == 0x3064u) {
                  static int s_ml = 0;
                  if (s_ml++ < 8 || (s_ml % 1024) == 0)
                      fprintf(stderr, "[pm-yield] 0x3064 LAUNCH bisl "
                              "target=0x%05X wi=%d 12A0=0x%08X #%d\n",
                              (uint32_t)ctx->gpr[5]._u32[0] & SPU_LS_MASK,
                              ctx->wi_state, pm_gate_lw(ctx, 0x12A0), s_ml),
                      fflush(stderr);
              }
              if (win28) {
                  static int s_went = 0;
                  if (ctx->wi_state != 1) {
                      ctx->wi_state = 1;
                      /* Round 34: capture the bisl link (prev fetch + 4)
                       * at span entry -- the PM continuation the module's
                       * stop must return to (0x3068-class). */
                      ctx->wi_resume_link = (ctx->wi_prev_pc + 4u) & SPU_LS_MASK;
                      if (wslot28 >= 0) {
                          ctx->wi_img = ctx->resident_code[wslot28].image_id;
                          ctx->wi_span_lo = ctx->resident_code[wslot28].lsa;
                          ctx->wi_span_hi = ctx->resident_code[wslot28].lsa
                                          + ctx->resident_code[wslot28].size;
                      }
                      if (s_went++ < 16 || (s_went % 4096) == 0)
                          fprintf(stderr, "[walkin] ENTER pc=0x%05X img=%d "
                                  "span=[0x%05X,0x%05X) link=0x%05X #%d\n",
                                  wpc28, ctx->wi_img,
                                  ctx->wi_span_lo, ctx->wi_span_hi,
                                  ctx->wi_resume_link, s_went),
                          fflush(stderr);
                  }
              } else if (ctx->wi_state == 1) {
                  static int s_wxit = 0;
                  ctx->wi_state = 2;
                  /* Round 30 (c): ret-to = gpr0 (bi $r0's target = where
                   * the PM resumes), r4 = the module's job parameter at
                   * exit. bg_288 class: the 210 entry stub no-ops and
                   * returns instantly when its parameter is 0 (brz r81
                   * -> low-LS job-done vector at module+0x24 path) --
                   * r4==0 at EXIT names a launch-parameter defect. */
                  if (s_wxit++ < 16 || (s_wxit % 1024) == 0)
                      fprintf(stderr, "[walkin] EXIT pc=0x%05X img=%d "
                              "ret-to=0x%05X r3=0x%08X r4=0x%08X "
                              "(pc left span toward PM) #%d\n",
                              wpc28, ctx->wi_img,
                              (uint32_t)(ctx->gpr[0]._u32[0] & SPU_LS_MASK),
                              ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0],
                              s_wxit),
                      fflush(stderr);
                  /* Round 31: bg_288's smoking EXIT (pc=0x2D68, r3=0) is
                   * the module calling a PM service IN THE SAME LS (bi
                   * indirect; SPURS jobmods branch straight into the
                   * kernel's low-LS code) and the service failing. 0x2D40+
                   * feeds brsl 0x2C18, the launch walk over the
                   * 0xDF0/0x11B0/0xEB0 tables -- r3=0 means the walk found
                   * nothing claimable. The module's output-publish path
                   * aborts on that (jobstop=0 forever). Dump the walk's
                   * exact inputs at the failing return so the empty table
                   * names itself. */
                  if (ctx->gpr[3]._u32[0] == 0
                      && (ctx->gpr[0]._u32[0] & SPU_LS_MASK) >= ctx->wi_span_lo
                      && (ctx->gpr[0]._u32[0] & SPU_LS_MASK) <  ctx->wi_span_hi) {
                      static int s_sf = 0;
                      if (s_sf++ < 8 || (s_sf % 256) == 0) {
                          uint32_t r0 = (uint32_t)(ctx->gpr[0]._u32[0] & SPU_LS_MASK);
                          fprintf(stderr, "[svc-fail] pm=0x%05X ret-to=0x%05X "
                                  "img=%d: walk inputs -- 1290(ticket)=0x%08X "
                                  "12D0=0x%08X 12C0=0x%08X 1300=0x%08X "
                                  "12A0=0x%08X\n",
                                  wpc28, r0, ctx->wi_img,
                                  pm_gate_lw(ctx, 0x1290),
                                  pm_gate_lw(ctx, 0x12D0),
                                  pm_gate_lw(ctx, 0x12C0),
                                  pm_gate_lw(ctx, 0x1300),
                                  pm_gate_lw(ctx, 0x12A0));
                          fprintf(stderr, "[svc-fail] EB0[0..3]=");
                          for (int _e = 0; _e < 4; _e++)
                              fprintf(stderr, " %08X", pm_gate_lw(ctx, 0xEB0 + 8*_e));
                          fprintf(stderr, " 11B0w0=0x%08X DF0w0=0x%08X "
                                  "1420=0x%08X\n",
                                  pm_gate_lw(ctx, 0x11B0),
                                  pm_gate_lw(ctx, 0x0DF0),
                                  pm_gate_lw(ctx, 0x1420));
                          fflush(stderr);
                      }
                  }
              }
              ctx->wi_prev_pc = wpc28;
              if (wpc28 == 0x2EF0u) {
                  static int s_rg = -1;
                  if (s_rg < 0) { const char* e = getenv("SPU_PM_RETIRE_GUARD");
                                  s_rg = (e && e[0] == '0') ? 0 : 1; }
                  if (s_rg) {
                      /* Round 30: the round-28 stall on wi==2 was WRONG and
                       * wedge-shaped. wi==2 at 0x2EF0 means the module
                       * returned to PM flow THIS cycle (span exit already
                       * happened); that retire is legit NEXT-job prep, and
                       * stalling it froze the machine at this pc forever
                       * (bg_287: retire_stall lines climbing, zero jobstop).
                       * pc can never be inside the module span here
                       * (0x2EF0 < span base 0x4000), so "retire while the
                       * module is mid-run" is not even representable at this
                       * site -- a mid-run park ends the pass by halt with
                       * wi_state==1, and the round-29 pc-RESUME is what
                       * continues it. Guard is now diagnostic-only: log the
                       * decision inputs, never stall. SPU_PM_RETIRE_GUARD=0
                       * silences the log too. */
                      static int s_rn = 0;
                      if (s_rn++ < 24 || (s_rn % 256) == 0) {
                          extern uint8_t* vm_base;
                          uint32_t qea = pm_gate_lw(ctx, 0x13F0);
                          uint32_t gw = 0;
                          if (vm_base && qea)
                              gw = __builtin_bswap32(
                                  *(const uint32_t*)(vm_base + (qea & 0x3FFFFFFFu) + 0x10));
                          fprintf(stderr, "[pm-gate] TIMEOUT-RETIRE "
                                  "12A0=0x%08X 12D0=0x%08X 1420=0x%08X "
                                  "guest@0x%08X+0x10=0x%08X wi=%d img=%d "
                                  "-- retire proceeds (guard diagnostic)\n",
                                  pm_gate_lw(ctx, 0x12A0), pm_gate_lw(ctx, 0x12D0),
                                  pm_gate_lw(ctx, 0x1420), qea, gw,
                                  ctx->wi_state, ctx->wi_img);
                          fflush(stderr);
                      }
                  }
              }
              /* Round 32: bg_289 froze at {12A0=0 (post-0x3014 fsmbi reset),
               * 12C0=0xFFFFFFF1 PARKED, 12D0=-1} with live bit-30 0xEB0
               * entries -- the claim cycle never re-runs. Decode:
               *   0x1900 ori r0,r0,-16; 0x1904 stqa r0,0x12C0
               *     = THE PARK (12C0 |= ~15, keeps low nibble = buffer
               *     count; makes 12C0 signed-negative). Taken at 0x18F8
               * when the 0x2B88 decrementer helper returns r3==0.
               *   0x2B04-0x2B0C: state-0 handler claims ONLY if
               *     cgti(12C0,-1) -- parked = claim unreachable.
               *   Un-park writers: ONLY 0x2F2C (prep, writes slot+1,
               *     reached via the claim itself) and 0x2A34 (attach
               *     re-init). Once parked in state 0, no in-module path
               *     back except attach re-init. Parked + state-0 is the
               *     wedge signature. */
              if (wpc28 == 0x1904u) {
                  static int s_pk = 0;
                  if (s_pk++ < 12 || (s_pk % 512) == 0) {
                      fprintf(stderr, "[pm-park] 0x1904 writes 12C0=0x%08X "
                              "(old=0x%08X) 12A0=0x%08X 12D0=0x%08X "
                              "12F0=0x%08X EB0w0=0x%08X dec=0x%08X "
                              "#%d\n",
                              ctx->gpr[0]._u32[0], pm_gate_lw(ctx, 0x12C0),
                              pm_gate_lw(ctx, 0x12A0), pm_gate_lw(ctx, 0x12D0),
                              pm_gate_lw(ctx, 0x12F0), pm_gate_lw(ctx, 0xEB0),
                              ctx->gpr[5]._u32[0], s_pk);
                      fflush(stderr);
                  }
              }
              if (wpc28 == 0x2B0Cu) {
                  /* claim gate decision: pc just arrived here with 12C0 in
                   * r2 (loaded 0x2B04). Log when WORK IS AVAILABLE (EB0[0]
                   * bit30) but the gate blocks -- the wedge discriminator. */
                  static int s_cg = 0;
                  uint32_t eb0 = pm_gate_lw(ctx, 0xEB0);
                  if ((eb0 & 0x40000000u)
                      && (ctx->gpr[2]._u32[0] & 0x80000000u)) {
                      if (s_cg++ < 12 || (s_cg % 512) == 0) {
                          fprintf(stderr, "[pm-gate] CLAIM-BLOCKED 12C0="
                                  "0x%08X (parked) 12A0=0x%08X "
                                  "EB0w0=0x%08X -- work posted, claim "
                                  "disabled #%d\n",
                                  ctx->gpr[2]._u32[0],
                                  pm_gate_lw(ctx, 0x12A0), eb0, s_cg);
                          fflush(stderr);
                      }
                  }
              }
            }
             if (_gt) {
            { /* Round 23: walk-in probe. The PM launches staged jobmods by
               * sequentially walking into their bytes (no indirect branch,
               * so resident-code DISPATCH never fires). On FIRST entry into
               * any resident_code span, compare LS bytes at pc against the
               * module's guest source and the PM's own image (staged from
               * the SPURS instance ELF): match=module is the healthy
               * launch, match=NEITHER means the span was overwritten
               * between staging and walk-in (restage race). Capped 16. */
              static int s_inspan = 0, s_win = 0;
              uint32_t wpc = (uint32_t)ctx->pc & SPU_LS_MASK;
              int win = 0, wslot = -1;
              for (int i = 0; i < 4; i++)
                  if (ctx->resident_code[i].size
                      && wpc >= ctx->resident_code[i].lsa
                      && wpc < ctx->resident_code[i].lsa
                                   + ctx->resident_code[i].size) {
                      win = 1; wslot = i; break;
                  }
              if (win && !s_inspan && s_win < 16) {
                  s_win++;
                  extern uint8_t* vm_base;
                  const char* verdict = "no-source";
                  uint32_t wls[4] = {0,0,0,0}, wmod[4] = {0,0,0,0};
                  if (wslot >= 0) {
                      uint32_t msrc = ctx->resident_code[wslot].source_ea
                                    + (wpc - ctx->resident_code[wslot].lsa);
                      const uint8_t* a = ctx->ls + wpc;
                      const uint8_t* b = vm_base + msrc;
                      int mod_ok = vm_base && memcmp(a, b, 16) == 0;
                      int pm_ok = vm_base
                          && memcmp(a, vm_base + 0x10135E00u + (wpc - 0xA00u),
                                    16) == 0;
                      verdict = mod_ok ? "match=module"
                              : pm_ok ? "match=PM" : "match=NEITHER";
                      for (int k = 0; k < 4; k++) {
                          wls[k]  = __builtin_bswap32(
                              *(const uint32_t*)(a + 4*k));
                          wmod[k] = vm_base ? __builtin_bswap32(
                              *(const uint32_t*)(b + 4*k)) : 0;
                      }
                  }
                  fprintf(stderr, "[walkin] pc=0x%05X img=%d %s "
                          "ls=%08X %08X mod=%08X %08X\n", wpc,
                          wslot >= 0 ? ctx->resident_code[wslot].image_id : -1,
                          verdict, wls[0], wls[1], wmod[0], wmod[1]);
                  fflush(stderr);
              }
              if (win && !s_inspan && wslot >= 0) {
                  /* Round 24 (c): walk-in native handoff. Sequential entry
                   * into a staged jobmod span never passes an indirect
                   * branch, so the resident-code fast path in
                   * spu_indirect_branch never ran and the module has been
                   * interpreted (~100ms/quantum) -- slower than the PM's
                   * retire window, so the PM retires+republishes livelocked
                   * jobs that were still running. Exit the interpreter with
                   * the resolver trampoline armed: spu_indirect_branch
                   * resolves pc in the resident_code span and tail-calls
                  * the lift, running the module at native speed; the lift
                  * stop/return paths (synthesised-stop, link-honoring
                  * staged-span exit) hand control back exactly as they do
                  * for bisl-launched staged codecs. MISS keeps the
                  * interpreter (status quo). SPU_WALKIN_NATIVE=1 opts IN:
                  * bg_281 regression -- default ON produced 7.8M native
                  * re-dispatches, FPS 0, jobstop 0. Even at REAL function
                  * starts (0x40B0/0x4110 are registered) the lift re-runs
                  * the prologue on mid-module guest state and its return
                  * never advances the walk-in site. Sequential walk-ins are
                  * NOT bisl launches. */
                  static int s_nat = -1;
                  if (s_nat < 0) { const char* e = getenv("SPU_WALKIN_NATIVE");
                                  s_nat = (e && e[0] == '1' && !e[1]) ? 1 : 0; }
                  if (s_nat) {
                      extern int spu_walkin_lift(uint32_t pc, int img, int rel);
                      if (spu_walkin_lift(wpc,
                              ctx->resident_code[wslot].image_id,
                              ctx->resident_code[wslot].rel)) {
                          static int s_hd = 0;
                          if (s_hd++ < 8 || (s_hd % 64) == 0)
                              fprintf(stderr, "[walkin] NATIVE handoff pc=0x%05X "
                                      "img=%d (lift hit -- module runs compiled)\n",
                                      wpc, ctx->resident_code[wslot].image_id),
                              fflush(stderr);
                          g_spu_trampoline_fn = spu_indirect_branch;
                          g_spu_interp_steps = steps;
                          return ctx->pc;
                      }
                  }
              }
              s_inspan = win; }
                if (ctx->pc == 0x2C08u) {
                    static int _n2 = 0;
                    if (_n2++ < 40) {
                        uint32_t cur = ctx->gpr[2]._u32[0] & SPU_LS_MASK;
                        fprintf(stderr, "[pm-gate] 0x2C08 verdict r3=0x%08X "
                                "slot[-2]=0x%08X slot[-1]=0x%08X (cur=0x%05X) "
                                "12D0=0x%08X 12A0=0x%08X C00w0=0x%08X%s\n",
                                ctx->gpr[3]._u32[0],
                                pm_gate_lw(ctx, cur - 16), pm_gate_lw(ctx, cur - 8), cur,
                                pm_gate_lw(ctx, 0x12D0), pm_gate_lw(ctx, 0x12A0),
                                pm_gate_lw(ctx, 0xC00),
                                ctx->gpr[3]._u32[0] ? " -> ADVANCE 12A0=3"
                                                    : " -> STAY 2 (park)");
                        fflush(stderr);
                    }
                } else if (ctx->pc == 0x2BD8u) {
                    static int _n0 = 0;
                    if (_n0++ < 16)
                        fprintf(stderr, "[pm-gate] state2 entry 12D0=0x%08X "
                                "12A0=0x%08X C00w0=0x%08X\n",
                                pm_gate_lw(ctx, 0x12D0), pm_gate_lw(ctx, 0x12A0),
                                pm_gate_lw(ctx, 0xC00)), fflush(stderr);
                } else if (ctx->pc == 0x2BE4u) {
                    /* return of the 0x21A0(8) tag-drain: r3==0 means the
                     * PM believes a DMA tag never completed -- a HOST
                     * tag-writeback defect candidate (bg_270 mission b). */
                    static int _nt = 0;
                    if (_nt++ < 32) {
                        fprintf(stderr, "[pm-gate] tagdrain ret r3=0x%08X "
                                "12A0=0x%08X%s\n", ctx->gpr[3]._u32[0],
                                pm_gate_lw(ctx, 0x12A0),
                                ctx->gpr[3]._u32[0] ? "" : " -- DRAIN FAILED");
                        fflush(stderr);
                    }
                } else if (ctx->pc == 0x29A4u) {
                    /* 0x29A4 brz $r73: r73 = word & 0x40000000 (the
                     * launchable-bit test), r5 = raw entry word, r68 =
                     * current entry address in the 0xEB0 table. */
                    static int _n4 = 0;
                    if (_n4++ < 64) {
                        fprintf(stderr, "[pm-gate] 0xEB0 scan el@0x%05X "
                                "word=0x%08X bit30=%s\n",
                                ctx->gpr[68]._u32[0] & SPU_LS_MASK,
                                ctx->gpr[5]._u32[0],
                                (ctx->gpr[73]._u32[0] & 0x40000000u) ? "SET"
                                                                     : "clear");
                        fflush(stderr);
                    }
                } else if (ctx->pc == 0x2224u || ctx->pc == 0x2ECCu
                           || ctx->pc == 0x3014u) {
                    /* the three other writers of the state cell 12A0
                     * (0x2C0C covered above): 0x2224 := 2 (SN handler
                     * retry), 0x2ECC := claim-helper return code (!=1),
                     * 0x3014 := 0/FF mask -- the WEDGE regression, reached
                     * after the retire routine zeroed the 0xEB0 table,
                     * FF-filled 0x11B0 and re-issued the gather via
                     * 0x26E4 (call site 0x2F5C). */
                    static int _nw = 0;
                    if (_nw++ < 64) {
                        unsigned wr = (ctx->pc == 0x2224u) ? 2
                                  : (ctx->pc == 0x2ECCu) ? 0
                                  : (unsigned)-1;
                        fprintf(stderr, "[pm-gate] STATE pc=0x%05X old=0x%08X "
                                "-> %s (12D0=0x%08X 12C0=0x%08X)\n",
                                ctx->pc, pm_gate_lw(ctx, 0x12A0),
                                wr == 2 ? "2 (SN retry)"
                                  : wr == 0 ? "claim-ret-code"
                                            : "0/FF (RETIRE+REGRESS)",
                                pm_gate_lw(ctx, 0x12D0),
                                pm_gate_lw(ctx, 0x12C0));
                        fflush(stderr);
                    }
                } else if (ctx->pc == 0x2F5Cu) {
                    /* retire path: about to call the gather 0x26E4 and
                     * then wipe 0xEB0 / re-FF 0x11B0. */
                    static int _nr = 0;
                    if (_nr++ < 32)
                        fprintf(stderr, "[pm-gate] RETIRE slot: gather call "
                                "0x26E4 (12A0=0x%08X)\n",
                                pm_gate_lw(ctx, 0x12A0)), fflush(stderr);
                } else if (ctx->pc == 0x2710u || ctx->pc == 0x1570u) {
                    /* 0x2710: busy-bit ALLOC on LS 0x1290 (first clear
                     * bit; all-ones -> error path 0x2788). 0x1570: FREE
                     * bit r3 -- only the job-COMPLETION path reaches it.
                     * If 0x1290 marches toward 0xFFFFFFFF with no frees,
                     * claimed-but-never-run jobs leak the 32-slot ticket
                     * space: the allocator wedges FULL forever. */
                    static unsigned _alloc = 0, _free = 0;
                    static int _na = 0;
                    if (_na++ < 48) {
                        if (ctx->pc == 0x2710u) _alloc++; else _free++;
                        fprintf(stderr, "[pm-gate] %s 0x1290=0x%08X "
                                "(allocs=%u frees=%u)%s\n",
                                ctx->pc == 0x2710u ? "ALLOC" : "FREE ",
                                pm_gate_lw(ctx, 0x1290), _alloc, _free,
                                pm_gate_lw(ctx, 0x1290) == 0xFFFFFFFFu
                                    ? " FULL!" : "");
                        fflush(stderr);
                    }
                } else if (ctx->pc == 0x3244u) {
                    /* Round 18: the atomic CLAIM on the guest joblist slot
                     * (0x3244..0x32AC: GETLLAR line to LS 0x80 at EA $r7,
                     * then PUTLLC 0xB4). The launch scan's bit30 comes from
                     * THIS line's words: builder 0x1C68 copies input bit30
                     * into the 0xEB0 entry. Log the guest 16B at claim time
                     * so the healthy->wedge transition of the producer's
                     * descriptors (0x4/0xC nibble = launchable vs 0x8 =
                     * standby-only) is directly visible. */
                    { extern uint8_t* vm_base;
                      uint32_t ce = ((uint32_t)ctx->gpr[7]._u8[0] << 24)
                                  | ((uint32_t)ctx->gpr[7]._u8[1] << 16)
                                  | ((uint32_t)ctx->gpr[7]._u8[2] << 8)
                                  | (uint32_t)ctx->gpr[7]._u8[3];
                      static int _nc = 0;
                      if (_nc++ < 24 || (_nc % 512) == 0) {
                          fprintf(stderr, "[pm-gate] CLAIM guest 0x%08X:", ce);
                          if (vm_base && ce > 0x10000u && ce < 0xF0000000u) {
                              /* EA bit31 MFC alias: dump the NORMALIZED
                               * target (what the transfer actually hits). */
                              const unsigned char* g = vm_base + (0x40000000u | (ce & 0x3FFFFFFFu));
                              for (int k = 0; k < 16; k += 4)
                                  fprintf(stderr, " %02X%02X%02X%02X",
                                          g[k], g[k+1], g[k+2], g[k+3]);
                          } else fprintf(stderr, " <bad ea>");
                          fprintf(stderr, "\n"); fflush(stderr);
                      } }
                }
            }
        }
        /* Rejoin the compiled fast path only for images that HAVE lifted
         * functions (image_id >= 0). image_id < 0 = pure interpretation: an
         * un-lifted image (e.g. a title's raw SPU jobs) must never rejoin
         * another image's functions that happen to share an LS address. */
        /* ponytail: pc-only match; a recursive call through the same site would stop early -- compare r1 too if one shows up. */
        int in_span = ctx->interp_span_lo < ctx->interp_span_hi &&
                      ctx->pc >= ctx->interp_span_lo && ctx->pc < ctx->interp_span_hi;
        /* M18: in the SPURS policy kernel-home window ([img_ls_base,
         * img_ls_base+0xA00)) the img_ls_base-relative lift entries are the
         * kernel stubs, and their lifted bodies exit in LABEL space (pc=0x80
         * for guest 0xA80) -- rejoining there derails the wwsjob SN handler
         * into guest 0x80 garbage and the list-stall ack is never written
         * (1500+ observed cycles of exactly two interpreted steps). Keep the
         * interpreter in charge inside that window. */
        int in_kh = ctx->policy_mode && ctx->img_ls_base &&
                    ctx->pc >= ctx->img_ls_base &&
                    ctx->pc <  ctx->img_ls_base + 0xA00u;
        /* in_span: inside a runtime-staged module the same-LS lifts of another
         * image are stale bytes -- keep interpreting instead of rejoining. */
        static int s_forceint = -1;
        if (s_forceint < 0) s_forceint = getenv("SPU_PM_INTERP") ? 1 : 0;
        /* Explicit native leaves preserve the existing rejoin and forced-PM policy.
         * The legacy global ceiling reports an attempted step before decoding;
         * reserve that final step rather than changing its boundary convention. */
        if (native_count && ctx->pc<0x40000 && !(ctx->pc&3) &&
            (native_entries[ctx->pc>>7] & (1u<<((ctx->pc>>2)&31))) &&
            !_tr && !_cap && !_g1 && !s_kh && !g_spu_ls_watch_n &&
            g_spu_ls_probe<=0 && g_spu_smc_watch<=0 &&
            !native_instrumented) {
            uint64_t remaining=(local_budget ? local_budget : UINT64_MAX)-steps;
            if (g_spu_interp_budget) {
                uint64_t legacy=g_spu_interp_budget>steps ? g_spu_interp_budget-steps-1 : 0;
                if (legacy<remaining) remaining=legacy;
            }
            uint32_t last=g_spu_interp_last_pc;
            if (native_try(ctx,stop_lsa,remaining,local_budget || g_spu_interp_budget,&last,&steps)) {
                g_spu_interp_last_pc=last;
                continue;
            }
        }
        int owned_mode=spu_native_owned_policy(ctx);
        int owned_rejoin=owned_mode && !in_kh && !local_budget &&
            (!stop_lsa || (native_call && ctx->host_depth>0 && stop_lsa==ctx->drain_ret_pc)) &&
            !g_spu_interp_budget && !_tr && !_cap && !_g1 && !s_kh &&
            !native_instrumented && spu_native_owned_can_run(ctx,ctx->pc);
        int _rej = owned_mode
            ? (owned_rejoin && spu_lifted_lookup(ctx,ctx->pc))
            : (!in_span && !in_kh && ctx->image_id >= 0 &&
               !s_forceint && spu_lifted_lookup(ctx,ctx->pc));
        if (((stop_lsa && ctx->pc == (stop_lsa & SPU_LS_MASK)) || _rej)) {
            /* M21: a rejoin hands control back to the DISPATCHER -- re-arm the
             * trampoline like the SMC microstep exits do. Without this the
             * SPU_DRAIN loop (which clears g_spu_trampoline_fn before each
             * dispatch) sees a NULL trampoline when the resolver returns and
             * ends the run at the rejoin pc (observed: kh handler's 0xA8C
             * brsl->0x2210 stall-ack service never dispatched, 487 run-end
             * cycles at pc=0x2210 status=RUNNING). The stop_lsa disjunct stays
             * unarmed: that return pops the caller's drain frame. */
            if (_rej && !(stop_lsa && ctx->pc == (stop_lsa & SPU_LS_MASK))) g_spu_trampoline_fn = spu_indirect_branch;
            g_spu_interp_steps = steps; g_spu_interp_last_pc = ctx->pc; return ctx->pc; }
        { static int _rj = 0; /* M21: trace the kh handler's 0xA8C brsl->0x2210 rejoin decision */
          if (ctx->policy_mode && ctx->image_id == 200 && ctx->pc >= 0x2000 && ctx->pc < 0x3000 && _rj++ < 40)
            fprintf(stderr, "[rejoin] pc=0x%05X span=%d kh=%d img=%d lu=%d stop=0x%X\n",
                    ctx->pc, in_span, in_kh, ctx->image_id,
                    !!(spu_lifted_lookup(ctx, ctx->pc)), stop_lsa); }
        g_spu_interp_last_pc = ctx->pc;
        if (s_kh && ctx->policy_mode && ctx->pc >= ctx->img_ls_base && ctx->pc < ctx->img_ls_base + 0xA00u) {
            /* SPU_KH_TRACE: per-step trace of interpreted SPURS kernel-home
             * code (vector handler + scheduler). The wwsjob SN handler's
             * Load->Run advance (ceqhi on loadJobState) lives here; log op +
             * key register lanes so the failing compare is visible. */
            uint32_t _m = ctx->pc & SPU_LS_MASK;
            uint32_t _iw = ((uint32_t)ctx->ls[_m] << 24) | ((uint32_t)ctx->ls[_m+1] << 16) |
                           ((uint32_t)ctx->ls[_m+2] << 8) | ctx->ls[_m+3];
            spu_ins _d; spu_decode1(_iw, _m, &_d);
            { static int s_khmax = -1; if (s_khmax < 0) { const char* e = getenv("SPU_KH_MAX"); s_khmax = e ? atoi(e) : 6000; } if (s_khn++ < (uint64_t)s_khmax)
                fprintf(stderr, "[kh] %05X %-8s r3=%08X r8=%08X r13=%08X r15=%08X\n",
                        _m, spu_op_name[_d.op],
                        ctx->gpr[3]._u32[0], ctx->gpr[8]._u32[0],
                        ctx->gpr[13]._u32[0], ctx->gpr[15]._u32[0]); }
        }
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
        /* Host-armed step ceiling (g_spu_interp_budget): differential
         * harnesses bound interpreted runs so a task spinning on a host-
         * absent producer cannot interpret forever. 0 (default) = off and
         * behavior is unchanged. */
        if (g_spu_interp_budget && steps >= g_spu_interp_budget) {
            g_spu_interp_budget_hit = 1;
            g_spu_interp_steps = steps;
            return ctx->pc;
        }
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
        int _r = spu_step(ctx);
        if (_r == 2) {  /* iret bail: the host must restore the saved
            register file (spu_irq_regs_maybe_restore) and re-dispatch at
            srr0 -- interpreting the continuation in-place ran it with the
            handler's registers (wwsjob PM wedge). Arm the dispatch so
            whichever host frame regains control (drain loop or the driver's
            SPU_DRAIN) resumes at pc instead of unwinding to run end. */
            { static int _bn = 0; if (_bn++ < 16) {
                fprintf(stderr, "[pm-iret] interp bail -> pc=0x%05X srr0=0x%05X ie=%d steps=%llu\n",
                        ctx->pc & 0x3FFFC, ctx->srr0 & SPU_LS_MASK, (int)ctx->int_enable,
                        (unsigned long long)steps);
                if (_bn <= 3 || (_bn % 64) == 0) {   /* name the loop + state */
                    fprintf(stderr, "[pm-iret] last %d PCs:", rn);
                    for (int k = rn; k > 0; k--) fprintf(stderr, " %05X", ring[(rc - k) & 63]);
                    fprintf(stderr, "\n");
                    {
                    #define HB(o) (((uint32_t)ctx->ls[(o)]<<24)|((uint32_t)ctx->ls[(o)+1]<<16)|((uint32_t)ctx->ls[(o)+2]<<8)|ctx->ls[(o)+3])
                    fprintf(stderr, "[pm-iret] LS state: 12A0=%08X 12C0=%08X 12D0=%08X 12E0=%08X 12F0=%08X 14E0=%08X 14F0=%08X 14F8=%08X\n",
                        HB(0x12A0),HB(0x12C0),HB(0x12D0),HB(0x12E0),HB(0x12F0),HB(0x14E0),HB(0x14F0),HB(0x14F8));
                    #undef HB
                    }
                    { extern uint8_t* vm_base;
                      if (vm_base) { const uint8_t* g = vm_base + 0x40244380u;
                        fprintf(stderr, "[pm-iret] guest 0x40244380 @bail:");
                        for (int o = 0; o < 0x30; o += 8)
                            fprintf(stderr, " %02X%02X%02X%02X%02X%02X%02X%02X",
                                g[o],g[o+1],g[o+2],g[o+3],g[o+4],g[o+5],g[o+6],g[o+7]);
                        fprintf(stderr, "\n"); fflush(stderr); } } } } }
            /* Equal IRET continuation PCs do not establish idleness: guest
             * instructions can progress between returns. Preserve normal IRQ
             * register restoration and redispatch without a synthetic halt. */
            { extern volatile unsigned g_spu_iret_bails; g_spu_iret_bails++; }
            g_spu_trampoline_fn = spu_indirect_branch;   /* pending re-dispatch */
            g_spu_interp_steps = steps; g_spu_interp_last_pc = ctx->pc;
            return ctx->pc; }
        if (_r) {
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
            { /* Round 24(a)/26: stop-and-signal codes from inside a staged
               * jobmod span -- resident slots regardless of lift presence
               * (round 25: most walk-in code sits in UNREGISTERED spans, so
               * the lifted flag says which; silence overall = module never
               * stops in-span, exits by link-return or is retired mid-run).
               * Compare healthy job-end codes vs the last before a retire. */
              uint32_t _jp = (uint32_t)ctx->pc & SPU_LS_MASK;
              for (int _i = 0; _i < 4; _i++)
                  if (ctx->resident_code[_i].size
                      && _jp >= ctx->resident_code[_i].lsa
                      && _jp < ctx->resident_code[_i].lsa
                               + ctx->resident_code[_i].size) {
                      extern int spu_walkin_lift(uint32_t, int, int);
                      int _lf = spu_walkin_lift(
                          _jp, ctx->resident_code[_i].image_id,
                          (int)ctx->resident_code[_i].rel);
                      static int _jn = 0;
                      if (_jn++ < 32 || (_jn % 128) == 0)
                          fprintf(stderr, "[jobstop] img=%d lifted=%d code=0x%04X "
                                  "pc=0x%05X r3=0x%08X r4=0x%08X\n",
                                  ctx->resident_code[_i].image_id, _lf,
                                  ctx->stop_code, _jp,
                                  ctx->gpr[3]._u32[0], ctx->gpr[4]._u32[0]),
                          fflush(stderr);
                      break;
                  } }
            /* Round 34: a module STOP inside a walk-in span is the job's
             * completion signal -- on HW the return lands at the bisl link
             * (0x3068) and the PM window completes. Flag the link resume
             * so the next pass continues the PM flow instead of restarting
             * at the entry and stranding the window (bg_291). */
            if (ctx->policy_mode && ctx->wi_state
                && !ctx->wi_link_pending) {
                ctx->wi_link_pending = 1;
                { static int s_ys = 0;
                  if (s_ys++ < 8 || (s_ys % 256) == 0)
                      fprintf(stderr, "[pm-yield] module STOP code=0x%04X "
                              "pc=0x%05X link=0x%05X wi=%d #%d\n",
                              ctx->stop_code,
                              (uint32_t)ctx->pc & SPU_LS_MASK,
                              ctx->wi_resume_link, ctx->wi_state, s_ys),
                      fflush(stderr); }
            }
            if (ctx->pm_win_touched) {
                static int s_ws = 0;
                if (s_ws++ < 24)
                    fprintf(stderr, "[pm-window] EXIT class=stop code=0x%04X "
                            "pc=0x%05X wi=%d pend=%d 12A0=0x%08X 12C0=0x%08X\n",
                            ctx->stop_code, (uint32_t)ctx->pc & SPU_LS_MASK,
                            ctx->wi_state, ctx->wi_link_pending,
                            pm_gate_lw(ctx, 0x12A0), pm_gate_lw(ctx, 0x12C0)),
                    fflush(stderr);
            }
            ctx->pm_designed = 1;   /* genuine stop/halt exit: designed (R33) */
            return ctx->stop_code; }
    }
}

#ifdef _WIN32
typedef struct {
    int enabled, depth, policy;
    LARGE_INTEGER frequency, begin;
    double epoch, policy_ms, other_ms, max_ms;
    unsigned long long policy_calls, other_calls, unwind_calls;
} InterpStats;
static _Thread_local InterpStats s_interp_stats = { .enabled = -1 };

static void interp_stats_finish(int unwound)
{
    InterpStats* s = &s_interp_stats;
    LARGE_INTEGER end;
    QueryPerformanceCounter(&end);
    double now = (double)end.QuadPart / (double)s->frequency.QuadPart;
    double elapsed = (double)(end.QuadPart - s->begin.QuadPart) * 1000.0 /
                     (double)s->frequency.QuadPart;
    if (!s->epoch) s->epoch = (double)s->begin.QuadPart / (double)s->frequency.QuadPart;
    if (s->policy) { s->policy_ms += elapsed; s->policy_calls++; }
    else { s->other_ms += elapsed; s->other_calls++; }
    if (elapsed > s->max_ms) s->max_ms = elapsed;
    s->unwind_calls += unwound != 0;
    s->depth = 0;
    if (now - s->epoch >= 1.0) {
        fprintf(stderr, "[SPUINTERP] tid=%lu window_ms=%.3f policy_ms=%.3f"
                " other_ms=%.3f policy_calls=%llu other_calls=%llu max_ms=%.3f"
                " unwind_calls=%llu\n",
                GetCurrentThreadId(), (now - s->epoch) * 1000.0, s->policy_ms,
                s->other_ms, s->policy_calls, s->other_calls, s->max_ms, s->unwind_calls);
        s->epoch = now; s->policy_ms = s->other_ms = s->max_ms = 0;
        s->policy_calls = s->other_calls = s->unwind_calls = 0;
    }
}
#endif

uint32_t spu_interp_stats_mark(void)
{
#ifdef _WIN32
    return (uint32_t)s_interp_stats.depth;
#else
    return 0;
#endif
}

void spu_interp_stats_unwind(uint32_t mark)
{
#ifdef _WIN32
    /* A nested driver must not close the still-live outer interpreter call. */
    if ((uint32_t)s_interp_stats.depth > mark) interp_stats_finish(1);
#else
    (void)mark;
#endif
}

uint32_t spu_interp_run_until(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa)
{
    return spu_interp_run_until_budget(ctx,start_lsa,stop_lsa,0);
}

static uint32_t spu_interp_run_until_budget_scoped_body(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa, uint64_t local_budget, int native_call)
{
#ifdef _WIN32
    /* Wall time includes channel waits. The driver closes samples on longjmp.
     * Count only outer calls to avoid recursive overlap. */
    InterpStats* s = &s_interp_stats;
    if (s->enabled < 0) { const char* e = getenv("SPU_INTERP_STATS");
        s->enabled = e && atoi(e) != 0; }
    if (!s->enabled || s->depth) return interp_run_until_impl(ctx, start_lsa, stop_lsa, local_budget, native_call);
    if (!s->frequency.QuadPart) QueryPerformanceFrequency(&s->frequency);
    QueryPerformanceCounter(&s->begin);
    s->policy = ctx->policy_mode != 0;
    s->depth = 1;
    uint32_t result = interp_run_until_impl(ctx, start_lsa, stop_lsa, local_budget, native_call);
    interp_stats_finish(0);
    return result;
#else
    return interp_run_until_impl(ctx, start_lsa, stop_lsa, local_budget, native_call);
#endif
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

uint32_t spu_interp_run_until_budget(spu_context* ctx, uint32_t start_lsa, uint32_t stop_lsa, uint64_t local_budget)
{
    uint64_t saved = spu_sf_simd_enter();
    uint32_t result = spu_interp_run_until_budget_scoped_body(ctx, start_lsa, stop_lsa, local_budget, 0);
    spu_sf_simd_leave(saved);
    return result;
}

/* Permission belongs to this invocation, never TLS; retain stats/unwind and SIMD scopes. */
uint32_t spu_interp_run_native_call(spu_context* ctx,uint32_t start,uint32_t stop) {
    int authorized=spu_native_owned_policy(ctx) && ctx->host_depth>0 &&
        stop && stop==ctx->drain_ret_pc;
    uint64_t saved=spu_sf_simd_enter();
    uint32_t result=spu_interp_run_until_budget_scoped_body(ctx,start,stop,0,authorized);
    spu_sf_simd_leave(saved);
    return result;
}
