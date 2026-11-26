/* Link-only boundaries of the SPU runtime outside this build. The fixture runs
 * its own contexts; each boundary fails when it is reached. */
#include <stdint.h>
#include <stdlib.h>
void spu_interp_count_unexpected(const char* what)
{
    fprintf(stderr, "FAIL unexpected call: %s\n", what); abort();
}
#define UNEXPECTED(name, body) body { (void)0; spu_interp_count_unexpected(#name); abort(); }
UNEXPECTED(spurs_pm_build_context, uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t a, uint32_t b, uint32_t c, uint32_t d))
UNEXPECTED(spu_thread_publish_ctx, void spu_thread_publish_ctx(uint32_t tid, void* c))
UNEXPECTED(ppu_resv_break_store, void ppu_resv_break_store(uint64_t ea))
UNEXPECTED(ps3_ww_report_inline, void ps3_ww_report_inline(uint32_t a, uint64_t v, int w))
UNEXPECTED(ps3_ms_now, unsigned long long ps3_ms_now(void))
UNEXPECTED(spurs_ef_set_from_spu, void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits))
UNEXPECTED(ydkj_wake_all_event_flags, void ydkj_wake_all_event_flags(void))
UNEXPECTED(spu_thread_register_phantom, uint32_t spu_thread_register_phantom(void))
UNEXPECTED(mainspin_watch_n, unsigned int mainspin_watch_n(void))
UNEXPECTED(mainspin_watch_at, unsigned int mainspin_watch_at(int i))
UNEXPECTED(lbp_breadcrumb_dump, void lbp_breadcrumb_dump(const char* tag))
UNEXPECTED(ppu_log_host_chain, void ppu_log_host_chain(const char* tag))
UNEXPECTED(lv2_usec_deadline, int64_t lv2_usec_deadline(uint64_t usec))
UNEXPECTED(lv2_deadline_passed, int lv2_deadline_passed(int64_t deadline))
UNEXPECTED(ppu_thread_wait_begin, void ppu_thread_wait_begin(void* c, const char* kind, uint64_t a, uint64_t b))
void ps3_spu_job_complete_pending(void) {}
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
uint8_t* vm_base;   /* NULL: no guest memory; a DMA that needs it stops the lane */
uint32_t ppu_vm_size;
