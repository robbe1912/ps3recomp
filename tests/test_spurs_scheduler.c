/* Manufactured policy modules exercise the real initializer, scheduler and
 * SPU policy execution. No game code, addresses or completion substitutions. */
#include "spurs_scheduler_observer_fixture.h"
static atomic_ullong fixture_poll_time;
static atomic_int fixture_poll_clock;
static atomic_uintptr_t fixture_park_timer;
static atomic_uint fixture_parked, fixture_stop_wakes;
/* With fixture_poll_clock set, the performance counter of the coordinator
 * deadline (spurs_notify_wait.h) reads fixture_poll_time in milliseconds. */
static BOOL fixture_qpc(LARGE_INTEGER* counter) {
    if (atomic_load(&fixture_poll_clock)) { counter->QuadPart=(LONGLONG)atomic_load(&fixture_poll_time)*10000; return TRUE; }
    return QueryPerformanceCounter(counter);
}
static BOOL fixture_qpf(LARGE_INTEGER* frequency) {
    QueryPerformanceFrequency(frequency);
    if (atomic_load(&fixture_poll_clock)) frequency->QuadPart=10000000;
    return TRUE;
}
/* The coordinator wait of the notify mode is one waitable timer. The fakes
 * mark its entry and the wake that stops it. */
static DWORD fixture_wait_single(HANDLE handle, DWORD ms) {
    if ((uintptr_t)handle == atomic_load(&fixture_park_timer)) atomic_store(&fixture_parked, 1);
    return WaitForSingleObject(handle, ms);
}
static BOOL fixture_set_timer(HANDLE handle, const LARGE_INTEGER* due, LONG period,
                              PTIMERAPCROUTINE completion, LPVOID arg, BOOL resume) {
    if ((uintptr_t)handle == atomic_load(&fixture_park_timer) && due->QuadPart == -1)
        atomic_fetch_add(&fixture_stop_wakes, 1);
    return SetWaitableTimer(handle, due, period, completion, arg, resume);
}
#define QueryPerformanceCounter fixture_qpc
#define QueryPerformanceFrequency fixture_qpf
#define WaitForSingleObject fixture_wait_single
#define SetWaitableTimer fixture_set_timer
#define spu_workload_dispatch_async fixture_unbounded_dispatch
#include "../libs/spurs/cellSpurs.c"
#undef spu_workload_dispatch_async
#undef SetWaitableTimer
#undef WaitForSingleObject
#undef QueryPerformanceFrequency
#undef QueryPerformanceCounter
/* Keep lock witnesses active for authored policies that use ef_line_end,
 * whose paired unlock lives in the actual production implementation. */
#include <assert.h>
#include <stdatomic.h>
#include "../runtime/spu/spu_coherency.h"

void spu_indirect_branch(spu_context*);
void spu_halt(spu_context*);
uint8_t* vm_base;
uint32_t ppu_vm_size=0x20000;
int g_resv_store_active;
uint32_t g_ww_lo,g_ww_hi,g_barrier_sync_watch;
uint32_t g_spu_image_src_ea,g_spu_image_ls_start,g_spu_image_span;
unsigned long long ps3_ms_now(void) { LARGE_INTEGER n,f; QueryPerformanceCounter(&n); QueryPerformanceFrequency(&f); return n.QuadPart*1000/f.QuadPart; }
void ppu_resv_break_store(uint64_t ea) {(void)ea;}
void ps3_ww_report_inline(uint32_t a,uint64_t v,int w){(void)a;(void)v;(void)w;}
static atomic_uint canceled_queues;
void sys_event_queue_cancel_by_id(u32 id) { assert(id==3 || id==4);atomic_fetch_or(&canceled_queues,1u<<id); }
void sys_event_queue_uncancel_by_id(u32 id) {(void)id;}
/* COFF linkers resolve references in unrelated HLE entry points before dead
 * stripping. Those APIs are outside this policy-scheduler fixture: trap if
 * reached, rather than fabricating task or event completion. */
static void unexpected_api(const char* name)
{
    fprintf(stderr,"FAIL: unexpected out-of-scope API: %s\n",name);abort();
}
void spurs_taskset_init(uint32_t ea,uint32_t spurs,uint64_t args,uint32_t wid,
                       uint32_t size,uint32_t evf1,uint32_t evf2)
{ unexpected_api("taskset init"); }
void spurs_taskset_add_task(uint32_t ea,uint32_t id,uint64_t elf,uint64_t context,
                           const uint32_t args[4],const uint32_t pattern[4])
{ unexpected_api("taskset add task"); }
int sys_event_queue_push_by_id(uint32_t id,uint64_t source,uint64_t a,uint64_t b,uint64_t c)
{ unexpected_api("job-chain event push");abort(); }
uint64_t spurs_pm_build_context(uint8_t* ls,uint32_t ea,uint32_t id,uint32_t a,uint32_t b)
{ unexpected_api("taskset context build");abort(); }
void ydkj_wake_all_event_flags(void)
{ unexpected_api("unrelated event-flag wake"); }
/* Observe the legacy bypass without substituting any policy completion. */
static atomic_uint unbounded_dispatches;
int fixture_unbounded_dispatch(const uint8_t* pm,uint32_t size,uint32_t data)
{ (void)pm;(void)size;(void)data;atomic_fetch_add(&unbounded_dispatches,1);return 0; }
static atomic_uint entered, produced, finished, release_consumer;
static atomic_uint consumer_lane,producer_lane;
static void finish(spu_context* c)
{
    trace_check_context(c);
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    /* The manufactured guest removes itself from the runnable set. */
    vm_write8(ea+0x80+wid,3);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void consumer(spu_context* c)
{
    spu_lockline_lock();spu_coh_reserve(c,spu_ls_read32(c,0x1C4));spu_lockline_unlock();
    atomic_store(&consumer_lane,spu_ls_read32(c,0x1C8));
    atomic_fetch_add(&entered,1);
    while (!atomic_load(&produced) && !atomic_load(&release_consumer)) Sleep(1);
    assert(atomic_load(&produced));
    atomic_fetch_add(&finished,1);finish(c);
}
static void producer(spu_context* c)
{
    atomic_store(&producer_lane,spu_ls_read32(c,0x1C8));
    atomic_store(&produced,1);finish(c);
}
static void wait_for(atomic_uint* p,unsigned value,const char* what)
{
    const unsigned long long deadline=ps3_ms_now()+3000;
    do { if(atomic_load(p)>=value)return;Sleep(1); } while(ps3_ms_now()<deadline);
    fprintf(stderr,"FAIL: %s: runnable producer could not run while consumer waited\n",what);exit(1);
}
static void finalize(u32 ea)
{
    const unsigned long long deadline=ps3_ms_now()+3000;
    do {
        s32 rc=cellSpursFinalize((CellSpurs*)(uintptr_t)ea);
        if (!rc) return;
        assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY);Sleep(1);
    } while(ps3_ms_now()<deadline);
    assert(!"finalize failed to join after real policy return");
}
static atomic_uint held[16],done[16],gate,active_count,peak_count,lane_mask;
static void held_policy(spu_context* c)
{
    unsigned w=spu_ls_read32(c,0x1DC),lane=spu_ls_read32(c,0x1C8);
    assert(lane<3);
    atomic_fetch_or(&lane_mask,1u<<lane);
    unsigned n=atomic_fetch_add(&active_count,1)+1,peak=atomic_load(&peak_count);
    while(peak<n && !atomic_compare_exchange_weak(&peak_count,&peak,n)) {}
    atomic_fetch_add(&held[w],1);
    while(!atomic_load(&gate)) Sleep(1);
    atomic_fetch_sub(&active_count,1);atomic_fetch_add(&done[w],1);finish(c);
}
static atomic_uint fast_visits,late_seen;
static void fast_policy(spu_context* c)
{
    trace_check_context(c);
    atomic_fetch_add(&fast_visits,1);
    if (atomic_load(&fixture_poll_clock)) atomic_fetch_add(&fixture_poll_time,10000);
    c->pc=SPURS_PM_EXIT_TO_KERNEL_LS;spu_indirect_branch(c);
}
static void late_policy(spu_context* c) { atomic_store(&late_seen,1);finish(c); }
static void stopped_policy(spu_context* c)
{
    trace_check_context(c);
    vm_write8(spu_ls_read32(c,0x1C4)+0x80+spu_ls_read32(c,0x1DC),3);
    atomic_store(&late_seen,1);spu_halt(c);
}
static atomic_uint direct_enter,direct_leave,direct_exit;
static atomic_uintptr_t direct_ctx[2];
static int direct_rc[2];
static void resident_policy(spu_context* c)
{
    unsigned id=c->gpr[4]._u32[1]-1;   /* low half of the u64 argument (#309) */
    assert(id<2);atomic_store(&direct_ctx[id],(uintptr_t)c);
    spu_ls_write32(c,0x4000,0x12340000u+id);
    atomic_fetch_add(&direct_enter,1);
    while(atomic_load(&direct_enter)<2) Sleep(1);
    assert(spu_ls_read32(c,0x4000)==0x12340000u+id);
    atomic_fetch_add(&direct_leave,1);finish(c);
}
static void exit_policy(spu_context* c) { atomic_store(&direct_exit,1);finish(c); }
static void error_policy(spu_context* c)
{
    while(!atomic_load(&direct_exit)) Sleep(1);
    Sleep(10);spu_halt(c);
}
static DWORD WINAPI direct(LPVOID arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    direct_rc[id]=spu_run_policy_module(resident_policy,79,vm_base+0x9400,16,id+1,3,0x11000+id*0x1000,0);
    return 0;
}
static DWORD WINAPI error_direct(LPVOID arg)
{
    unsigned id=(unsigned)(uintptr_t)arg;
    direct_rc[id]=spu_run_policy_module(id?exit_policy:error_policy,79,vm_base+0x9400,16,id+1,3,0x11000+id*0x1000,0);
    return 0;
}
static void run_pair(LPTHREAD_START_ROUTINE entry)
{
    HANDLE threads[2];
    for(unsigned i=0;i<2;i++) {
        threads[i]=CreateThread(NULL,0,entry,(LPVOID)(uintptr_t)i,0,NULL);
        assert(threads[i]);
    }
    for(unsigned i=0;i<2;i++) {
        assert(WaitForSingleObject(threads[i],INFINITE)==WAIT_OBJECT_0);
        assert(CloseHandle(threads[i]));
    }
}
static atomic_uint incarnation_runs,eligible_lane;
static void incarnation_policy(spu_context* c)
{
    assert(spu_ls_read32(c,0x4000)==0);
    spu_ls_write32(c,0x4000,0x9999);
    atomic_fetch_add(&incarnation_runs,1);finish(c);
}
static void eligible_policy(spu_context* c)
{
    atomic_store(&eligible_lane,spu_ls_read32(c,0x1C8)+1);finish(c);
}
static atomic_uint requested_before,requested_after;
static void requested_lanes_policy(spu_context* c)
{
    u32 ea=spu_ls_read32(c,0x1C4),wid=spu_ls_read32(c,0x1DC);
    /* Ready count is requested demand. The policy's actual claim changes it;
     * choosing a host worker must not pre-consume that same request. */
    spu_lockline_lock();
    unsigned requested=vm_read8(ea+SPURS_WKL_READY1+wid);
    atomic_store(&requested_before,requested);
    vm_base[ea+SPURS_WKL_READY1+wid]=(u8)(requested?requested-1:0);
    ef_line_end(ea);
    atomic_store(&requested_after,vm_read8(ea+SPURS_WKL_READY1+wid));
    finish(c);
}
static void register_pm(unsigned ea,unsigned byte,spu_lifted_entry_fn fn,int image)
{
    memset(vm_base+ea,byte,16);
    spu_workload_register_img(spu_workload_fingerprint(vm_base+ea,16),fn,image,"manufactured scheduler fixture");
}
/* SPURS_KERNEL_NOTIFY=1 with a timed pass far beyond every deadline below
 * (built with SPURS_KERNEL_POLL_MS=60000): only an explicit publication or a
 * returned lane can admit work, and stop must still join the coordinator. */
static int fairness_main(void) {
    atomic_store(&fixture_poll_clock,1);
    register_pm(0x9300,0x44,fast_policy,80);register_pm(0x9400,0x55,late_policy,81);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9300,16,0,NULL,1,1)==0);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,1)==0);
    wait_for(&fast_visits,100,"continuously notified ready work");
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    wait_for(&late_seen,1,"elapsed fallback starved by notifications");
    vm_write8(0x1080,3);finalize(0x1000);
    puts("SPURS elapsed deadline admits fallback under continuous kicks PASS");
    free(vm_base);return 0;
}
static int notify_main(void)
{
    assert(SPURS_KERNEL_POLL_MS >= 10000 && spurs_kernel_notify());
    register_pm(0x9200,0x33,held_policy,79);register_pm(0x9400,0x55,late_policy,81);
    /* A publication admits kicked demand without a timed pass. */
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
    Sleep(50);assert(!atomic_load(&held[0]) && "untimed pass admitted unkicked work");
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,1)==0);
    wait_for(&held[0],1,"ReadyCountStore notification");
    /* A notified pass admits only kicked workloads, even with a lane free. */
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,1)==0);   /* kick; wid 0 is at maxContention */
    Sleep(50);assert(!atomic_load(&late_seen) && "untimed pass admitted unkicked work");
    /* Fill the second lane, publish demand, then return lanes: the returned
     * lane, not a timed pass, admits it. */
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,2,1)==0);
    wait_for(&held[2],1,"second-lane publication");
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,1,1)==0);
    Sleep(50);assert(!atomic_load(&late_seen) && "busy lanes admitted a third policy");
    atomic_store(&gate,1);
    wait_for(&late_seen,1,"returned-lane notification");
    /* A fresh instance has no workers returning or pending publications. */
    finalize(0x1000);
    assert(cellSpursInitialize((CellSpurs*)0x3000,1,0,0,0)==0);
    struct SpursInst* parked = spurs_inst_find(0x3000);
    AcquireSRWLockExclusive(&parked->lock);
    atomic_store(&fixture_park_timer,(uintptr_t)parked->poll_timer);
    spurs_poll_wake(parked->poll_timer);
    ReleaseSRWLockExclusive(&parked->lock);
    wait_for(&fixture_parked,1,"coordinator wait entry");
    /* Acquiring this lock proves the wait released it atomically. */
    AcquireSRWLockExclusive(&parked->lock);
    assert(!parked->kicks && !parked->slots[0].busy);
    ReleaseSRWLockExclusive(&parked->lock);
    const unsigned long long before=ps3_ms_now();
    finalize(0x3000);
    assert(atomic_load(&fixture_stop_wakes) && "stop did not wake the parked coordinator");
    assert(ps3_ms_now()-before<3000 && "finalize waited for a timed pass");
    puts("SPURS notify: publication admission / returned-lane admission / prompt stop PASS");
    free(vm_base);return 0;
}
int main(int argc,char** argv)
{
    trace_setup();
    vm_base=calloc(1,ppu_vm_size);assert(vm_base);
    if(argc>1 && !strcmp(argv[1],"fairness")) return fairness_main();
    if(argc>1 && !strcmp(argv[1],"notify")) return notify_main();
    /* The ninth integer argument occupies the guest stack parameter area.
     * Asymmetric high/low words distinguish BE decoding from host word reads. */
    for (u32 requested=1; requested<=6; ++requested) {
        ppu_context ctx={0};
        ctx.gpr[1]=0x19000; ctx.gpr[3]=0x18000; ctx.gpr[4]=0x12345678;
        ctx.gpr[5]=0x87654321; ctx.gpr[6]=0x11223344; ctx.gpr[7]=0x55667788;
        ctx.gpr[8]=0x123456789abcdef0ULL; ctx.gpr[9]=0x18100; ctx.gpr[10]=3;
        for (u32 i=0;i<8;i++) vm_write8(0x18100+i,(u8)(i+1));
        vm_write64(0x19070,0xa1b2c3d400000000ULL | requested);
        ppu_context before=ctx;
        ps3_spurs_workload_attribute_initialize(&ctx);
        assert(ctx.gpr[3]==0);
        ctx.gpr[3]=before.gpr[3];
        assert(!memcmp(&ctx,&before,sizeof(ctx)));
        assert(vm_read32(0x18000+WKATTR_REVISION)==0x12345678);
        assert(vm_read32(0x18000+WKATTR_SDKVERSION)==0x87654321);
        assert(vm_read32(0x18000+WKATTR_PM)==0x11223344);
        assert(vm_read32(0x18000+WKATTR_SIZE)==0x55667788);
        assert(vm_read64(0x18000+WKATTR_DATA)==0x123456789abcdef0ULL);
        assert(vm_read32(0x18000+WKATTR_MIN_CONT)==3);
        assert(vm_read32(0x18000+WKATTR_MAX_CONT)==requested);
        for (u32 i=0;i<8;i++) assert(vm_read8(0x18000+WKATTR_PRIORITY+i)==i+1);
    }
    { ppu_context ctx={0}; ctx.gpr[1]=0xffffff00u; ctx.gpr[3]=0;
      ps3_spurs_workload_attribute_initialize(&ctx);
      assert(ctx.gpr[3]==(u64)(int64_t)(s32)CELL_SPURS_CORE_ERROR_NULL_POINTER); }
    { ppu_context ctx={0}; ctx.gpr[1]=0x19000; ctx.gpr[3]=0x18000;
      vm_write64(0x19070,0xa1b2c3d400000000ULL);
      ps3_spurs_workload_attribute_initialize(&ctx); assert(ctx.gpr[3]==0);
      assert(vm_read32(0x18000+WKATTR_MAX_CONT)==0);
      for (u32 i=0;i<8;i++) assert(vm_read8(0x18000+WKATTR_PRIORITY+i)==0); }
    register_pm(0x9000,0x11,consumer,77);register_pm(0x9100,0x22,producer,78);
    assert((u32)cellSpursInitialize((CellSpurs*)0x1000,7,0,0,0)==CELL_SPURS_CORE_ERROR_INVAL);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)0x1000,3,(u8*)0x8010,1)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9000,16,0,NULL,1,1)==0);
    wait_for(&entered,1,"consumer admission");
    for(unsigned i=0;i<8;i++) assert(cellSpursWakeUp((CellSpurs*)0x1000)==0);
    assert(!atomic_load(&unbounded_dispatches));
    unsigned char before[0xC0];memcpy(before,vm_base+0x1000,sizeof(before));
    assert((u32)cellSpursFinalize((CellSpurs*)0x1000)==CELL_SPURS_CORE_ERROR_BUSY);
    assert(!atomic_load(&canceled_queues) && !memcmp(before,vm_base+0x1000,sizeof(before)));
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9100,16,0,NULL,1,1)==0);
    wait_for(&finished,1,"cross-workload dependency");
    assert(atomic_load(&consumer_lane)<2 && atomic_load(&producer_lane)<2);
    assert(atomic_load(&consumer_lane)!=atomic_load(&producer_lane));
    unsigned port3=vm_read8(0x8010);
    finalize(0x1000);assert(atomic_load(&canceled_queues)==(1u<<3));
    assert(!spurs_port_queue(port3));
    puts("SPURS runnable producer / blocked consumer / BUSY immutability / join PASS");

    /* Reinitialize the same EA after all handles and resident contexts retire. */
    register_pm(0x9200,0x33,held_policy,79);
    assert(cellSpursInitialize((CellSpurs*)0x1000,3,0,0,0)==0);
    for(unsigned i=0;i<4;i++) {
        assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
        assert(vm_read32(0x8000)==i);
    }
    for(unsigned i=0;i<3;i++)wait_for(&held[i],1,"configured capacity");
    Sleep(20);assert(atomic_load(&peak_count)==3 && atomic_load(&lane_mask)==7);
    assert((u32)cellSpursRemoveWorkload((CellSpurs*)0x1000,0)==CELL_SPURS_CORE_ERROR_BUSY);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,0)==0);
    assert(vm_read8(0x1000+SPURS_WKL_MAXCONT)==0);
    assert(!atomic_load(&held[3]));
    for(unsigned i=0;i<3;i++)assert(atomic_load(&held[i])==1);
    atomic_store(&gate,1);
    for(unsigned i=0;i<4;i++)wait_for(&done[i],1,"release and next admission");
    finalize(0x1000);puts("SPURS configured capacity / maxContention / stable physical slots / reinit PASS");

    atomic_store(&gate,0);atomic_store(&held[0],0);atomic_store(&done[0],0);
    atomic_store(&peak_count,0);atomic_store(&lane_mask,0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,3,0,0,0)==0);
    { ppu_context ctx={0}; ctx.gpr[1]=0x19000; ctx.gpr[3]=0x18000;
      ctx.gpr[6]=0x9200; ctx.gpr[7]=16; ctx.gpr[9]=0x18100; ctx.gpr[10]=1;
      vm_write64(0x19070,0xa1b2c3d400000002ULL);
      ps3_spurs_workload_attribute_initialize(&ctx); assert(ctx.gpr[3]==0);
      assert(cellSpursAddWorkloadWithAttribute((CellSpurs*)0x1000,(u32*)0x8000,
             (CellSpursWorkloadAttribute*)0x18000)==0); }
    wait_for(&held[0],2,"maxContention two");Sleep(20);
    assert(atomic_load(&held[0])==2 && atomic_load(&peak_count)==2);
    assert(vm_read8(0x1000+SPURS_WKL_CURCONT)==2);
    assert(cellSpursSetMaxContention((CellSpurs*)0x1000,0,0)==0);
    Sleep(20);assert(atomic_load(&held[0])==2);
    atomic_store(&gate,1);wait_for(&done[0],2,"lowered limit preserves admitted work");
    finalize(0x1000);puts("SPURS one workload has two physical lanes and obeys lowered admission limit PASS");

    register_pm(0x9600,0x66,incarnation_policy,83);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    for(unsigned incarnation=0;incarnation<2;incarnation++) {
        assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9600,16,0,NULL,1,1)==0);
        wait_for(&incarnation_runs,incarnation+1,"same policy/data workload incarnation");
        for(unsigned i=0;;i++) {
            s32 rc=cellSpursRemoveWorkload((CellSpurs*)0x1000,0);
            if(!rc)break;assert((u32)rc==CELL_SPURS_CORE_ERROR_BUSY && i<3000);Sleep(1);
        }
    }
    finalize(0x1000);puts("SPURS removal retires resident LS even with unchanged policy/data PASS");

    register_pm(0x9700,0x77,eligible_policy,84);
    assert(cellSpursInitialize((CellSpurs*)0x1000,2,0,0,0)==0);
    memset(vm_base+0x8020,0,8);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9700,16,0,(u8*)0x8020,1,1)==0);
    Sleep(20);assert(!atomic_load(&eligible_lane));
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,5)==0);
    vm_write16(0x1000+SPURS_WKL_SIGNAL1,0x8000);
    assert(cellSpursRemoveWorkload((CellSpurs*)0x1000,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9700,16,0,(u8*)0x8020,1,1)==0);
    assert(cellSpursReadyCountSwap((CellSpurs*)0x1000,0,(u32*)0x8030,0)==0);
    assert(vm_read32(0x8030)==0 && !vm_read16(0x1000+SPURS_WKL_SIGNAL1));
    assert(cellSpursReadyCountCompareAndSwap((CellSpurs*)0x1000,0,(u32*)0x8030,5,9)==0);
    assert(vm_read32(0x8030)==0 && vm_read8(0x1000+SPURS_WKL_READY1)==0);
    assert(cellSpursReadyCountCompareAndSwap((CellSpurs*)0x1000,0,(u32*)0x8030,0,3)==0);
    assert(vm_read8(0x1000+SPURS_WKL_READY1)==3);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,0)==0);

    vm_base[0x8021]=1;
    assert(cellSpursSetPriorities((CellSpurs*)0x1000,0,(u8*)0x8020)==0);
    wait_for(&eligible_lane,1,"zero-priority physical lane exclusion");
    assert(atomic_load(&eligible_lane)==2);finalize(0x1000);
    puts("SPURS explicit zero priority excludes physical lane / SetPriorities guest publication PASS");

    register_pm(0x9300,0x44,fast_policy,80);register_pm(0x9400,0x55,late_policy,81);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9300,16,0,NULL,1,1)==0);
    wait_for(&fast_visits,2,"continuously runnable workload");
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    wait_for(&late_seen,1,"round-robin admission");
    vm_write8(0x1080,3);finalize(0x1000);puts("SPURS continuously runnable workload does not starve later producer PASS");

    /* Public API retains global workload IDs, while instances own admission,
     * worker handles, guest state and attached-queue teardown independently. */
    atomic_store(&gate,0);atomic_store(&held[0],0);atomic_store(&done[0],0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9200,16,0,NULL,1,1)==0);
    wait_for(&held[0],1,"first instance active");
    atomic_store(&late_seen,0);register_pm(0x9500,0x66,stopped_policy,82);
    assert(cellSpursInitialize((CellSpurs*)0x3000,1,0,0,0)==0);
    assert(cellSpursAttachLv2EventQueue((CellSpurs*)0x3000,4,(u8*)0x8010,1)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x3000,(u32*)0x8000,(void*)0x9500,16,0,NULL,1,1)==0);
    assert(vm_read32(0x8000)==1);wait_for(&late_seen,1,"independent instance stop/error execution");
    finalize(0x3000);
    assert(s_workloads[0].in_use && s_workloads[0].spurs_ea==0x1000);
    assert(vm_read8(0x1020)==1);
    atomic_store(&gate,1);wait_for(&done[0],1,"surviving instance completion");finalize(0x1000);
    puts("SPURS independent instance admission / stop return / queue ownership PASS");

    run_pair(direct);
    assert(direct_rc[0]==0 && direct_rc[1]==0 && atomic_load(&direct_leave)==2);
    assert(atomic_load(&direct_ctx[0])!=atomic_load(&direct_ctx[1]));
    spurs_policy_release_instance(0x11000);spurs_policy_release_instance(0x12000);
    run_pair(error_direct);assert(direct_rc[0]==-2 && direct_rc[1]==0);
    spurs_policy_release_instance(0x11000);spurs_policy_release_instance(0x12000);
    puts("SPURS same workload/lane across instances retains separate LS and TLS exit classification PASS");
    register_pm(0x9800,0x88,requested_lanes_policy,85);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    memset(vm_base+0x8020,0,8);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9800,16,0,(u8*)0x8020,1,1)==0);
    assert(cellSpursReadyCountStore((CellSpurs*)0x1000,0,5)==0);
    vm_base[0x8020]=1;
    assert(cellSpursSetPriorities((CellSpurs*)0x1000,0,(u8*)0x8020)==0);
    wait_for(&requested_after,1,"policy-owned demand");
    assert(atomic_load(&requested_before)==5 && "scheduler consumed policy-owned requested lanes");
    assert(atomic_load(&requested_after)==4);
    finalize(0x1000);
    puts("SPURS admission preserves requested lanes until actual policy claim PASS");

    g_spurs_pm_polls=7;g_spurs_pm_exited=1;g_wws_batch_gets=9;
    assert(spu_run_policy_module(NULL,0,vm_base+0x9800,16,0,0,0x1000,0)==-1);
    assert(!g_spurs_pm_polls && !g_spurs_pm_exited && !g_wws_batch_gets &&
           "rejected policy retained preceding worker productivity");
    puts("SPURS rejected policy clears runner-local counters before admission PASS");
    trace_validate();
#if PS3_SPURS_SCHEDULER_OBSERVATION
    /* Each phase starts after genuine joins, never a live observer reinstall. */
    atomic_store(&trace_phase,1);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);finalize(0x1000);
    atomic_store(&trace_phase,2);
    unsigned trace_before=atomic_load(&trace_count);
    atomic_store(&late_seen,0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    wait_for(&late_seen,1,"filtered observation preserves actual work");finalize(0x1000);
    assert(atomic_load(&trace_count)==trace_before);
    atomic_store(&trace_phase,3);
    s_trace_instance_epoch=s_trace_workload_epoch=UINT64_MAX;
    atomic_store(&late_seen,0);
    assert(cellSpursInitialize((CellSpurs*)0x1000,1,0,0,0)==0);
    struct SpursInst* trace_instance=spurs_inst_find(0x1000);
    AcquireSRWLockExclusive(&trace_instance->lock);
    trace_instance->trace_dispatch=UINT64_MAX;
    ReleaseSRWLockExclusive(&trace_instance->lock);
    assert(cellSpursAddWorkload((CellSpurs*)0x1000,(u32*)0x8000,(void*)0x9400,16,0,NULL,1,1)==0);
    wait_for(&late_seen,1,"identity exhaustion preserves actual work");finalize(0x1000);
    assert(s_trace_instance_epoch==UINT64_MAX && s_trace_workload_epoch==UINT64_MAX);
    assert(ps3_spurs_scheduler_observer_install(0));
    puts("SPURS actual admission intervals / lifetime reuse / TLS / lock release / invalid clock / exhaustion PASS");
#endif
    free(vm_base);return 0;
}
