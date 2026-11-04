/* Actual pre-fix production fragments from fork combined b7c1ff3f. */
spu_lifted_fn spu_lifted_lookup(const spu_context* ctx, uint32_t lsa)
{
    /* Streamed code owns its LS span even when the primary image also has a
     * function at that address. Match indirect dispatch's first-owner rule;
     * an owner miss must not rejoin the primary image's overwritten code. */
    if (ctx) {
        for (unsigned slot = 0; slot < 4; ++slot) {
            if (ctx->resident_code[slot].image_id &&
                lsa >= ctx->resident_code[slot].lsa &&
                lsa - ctx->resident_code[slot].lsa < ctx->resident_code[slot].size)
                return (spu_lifted_fn)spu_lookup(lsa, ctx->resident_code[slot].image_id);
        }
    }
    return (spu_lifted_fn)spu_lookup(lsa, ctx ? ctx->image_id : 0);
}
/* LEGACY_MATCH */
    /* WWS job-code overlay match: the PM (image 2) streams a job module into the
     * code buffer (base = LS[0x1320]) and branches to base+entryOffset. Its 16-byte
     * ila header identifies which of the 89 lifted job modules it is; match it and
     * mark that overlay resident so the module's lifted functions dispatch. The
     * module arrives via a DMA LIST, so the single-GET note_get hook never fired. */
    if (!fn && ctx->image_id == 2 && ctx->pc >= 0x4000 && ctx->pc < 0x3E000) {
        uint32_t codeBase = ((uint32_t)ctx->ls[0x1320] << 24) | ((uint32_t)ctx->ls[0x1321] << 16) |
                            ((uint32_t)ctx->ls[0x1322] << 8) | ctx->ls[0x1323];
        if (codeBase + 16 <= SPU_LS_SIZE) {
            extern int spu_overlay_match_sig(const uint8_t hdr[16]);
            int img = spu_overlay_match_sig(&ctx->ls[codeBase]);
            if (img) {
                if (ctx->resident_ovl != img) {
                    ctx->resident_ovl = img;
                    static int _n = 0; if (_n++ < 32)
                        fprintf(stderr, "[spu-ovl] WWS job module resident: codeBase=0x%05X "
                                "pc=0x%05X -> overlay image %d\n", codeBase, ctx->pc & SPU_LS_MASK, img);
                }
                fn = spu_lookup(ctx->pc, img);
            }
        }
    }
