/* Render-thread service for CPU loads from host-rendered guest surfaces. */
#include "ps3emu/guest_read_sync.h"
#include "rsx_surface_readback.h"
#include "rsx_cpu_surface_profile.h"
#include "rsx_cpu_surface_snapshot.h"
#include "rsx_fifo_boundary.h"
#include <stdatomic.h>
typedef struct CpuSurfaceRequest {
    struct CpuSurfaceRequest *next;
    u32 address, bytes;
    const VmReadSnapshotRange *snapshot_ranges;
    void *snapshot_staging;
    u32 snapshot_count, snapshot_output_bytes;
    int snapshot_result;
    u32 snapshot_trace_sequence;
    u32 client_thread;
    HANDLE done;
    int status;
    u64 queued_ms;
    RsxCpuSurfaceProfile profile;
} CpuSurfaceRequest;
static SRWLOCK s_cpu_surface_lock = SRWLOCK_INIT;
static _Atomic(CpuSurfaceRequest*) s_cpu_surface_head;
static CpuSurfaceRequest *s_cpu_surface_tail;
static volatile LONG s_cpu_surface_thread;
static ID3D12CommandAllocator *s_cpu_surface_alloc;
static ID3D12GraphicsCommandList *s_cpu_surface_list;
static ID3D12Fence *s_cpu_surface_fence;
static HANDLE s_cpu_surface_event;
static u64 s_cpu_surface_fence_value;
static int s_cpu_surface_live;
static int s_cpu_surface_recording;
static void cpu_surface_fail(const char *operation, HRESULT result) {
    fprintf(stderr,"[CPUSURFACE] %s failed: 0x%08lX\n",operation,(unsigned long)result);abort();
}
#define CPU_SURFACE_HR(call) do { HRESULT _result=(call); if(FAILED(_result))cpu_surface_fail(#call,_result); } while(0)
static int s_cpu_surface_profile;
static u64 s_cpu_surface_profile_frequency;
static RsxCpuSurfaceProfile *s_cpu_surface_profile_active;
static u64 cpu_surface_tick(void) {
    if(!s_cpu_surface_profile)return 0;
    LARGE_INTEGER t;if(!QueryPerformanceCounter(&t) || t.QuadPart<0)cpu_surface_fail("profile clock",E_FAIL);
    return (u64)t.QuadPart;
}
static void cpu_surface_profile_emit(CpuSurfaceRequest *request) {
    if(!s_cpu_surface_profile)return;
    RsxCpuSurfaceProfile *p=&request->profile;
    if(!rsx_cpu_surface_profile_valid(p))cpu_surface_fail("profile accounting",E_FAIL);
    u64 stages=p->allocation+p->prior_fence+p->commands+p->copy_fence+p->packing;
    fprintf(stderr,"[CPUSURFACE_PROFILE] address=%08X bytes=%u client=%lu renderer=%lu queued_at=%llu started_at=%llu finished_at=%llu returned_at=%llu frequency=%llu queue=%llu service=%llu return=%llu total=%llu allocation=%llu prior_fence=%llu commands=%llu copy_fence=%llu packing=%llu other=%llu matched=%llu hits=%llu copies=%llu host_bytes=%llu guest_bytes=%llu\n",
        request->address,request->bytes,(unsigned long)request->client_thread,(unsigned long)s_cpu_surface_thread,
        (unsigned long long)p->queued,(unsigned long long)p->started,(unsigned long long)p->finished,(unsigned long long)p->returned,
        (unsigned long long)s_cpu_surface_profile_frequency,
        (unsigned long long)(p->started-p->queued),(unsigned long long)(p->finished-p->started),
        (unsigned long long)(p->returned-p->finished),(unsigned long long)(p->returned-p->queued),
        (unsigned long long)p->allocation,(unsigned long long)p->prior_fence,(unsigned long long)p->commands,
        (unsigned long long)p->copy_fence,(unsigned long long)p->packing,
        (unsigned long long)(p->finished-p->started-stages),(unsigned long long)p->matched,
        (unsigned long long)p->hits,(unsigned long long)p->copies,
        (unsigned long long)p->host_bytes,(unsigned long long)p->guest_bytes);
    if(request->snapshot_trace_sequence)
        fprintf(stderr,"[CPUSNAPSHOT_RESULT] seq=%u status=%d client=%u renderer=%lu queued_at=%llu started_at=%llu finished_at=%llu\n",
            request->snapshot_trace_sequence,request->snapshot_result,request->client_thread,
            (unsigned long)s_cpu_surface_thread,(unsigned long long)p->queued,
            (unsigned long long)p->started,(unsigned long long)p->finished);
}
/* Client-visible surface views. A client thread serves a read itself when
 * guest memory already holds every write recorded for each surface the read
 * touches. Any other read takes the queued path.
 * The renderer owns one view for each slot of the surface pool. It is the
 * only writer of a view, and only between two stores of an odd sequence
 * number. A reader uses the fields only when it reads the same even sequence
 * number before and after them (a seqlock). Stores are release and loads are
 * acquire, so a reader that sees the new sequence number also sees the bytes
 * the renderer packed into guest memory first. x86-64 keeps load-load and
 * store-store order as other processors observe them. Every field is one
 * aligned 64-bit word.
 * The views form one table. s_cpu_surface_views holds the table address in
 * bits 0 to 46 and the entry count in bits 48 to 63, so a reader gets both
 * with one load. The renderer replaces the table only to add entries. It
 * never frees a table, because a reader can still hold an older address, and
 * it gives every entry of a replaced table an odd sequence number.
 * The request profile check compiles this block without <stdatomic.h>, so it
 * uses the C11 atomic builtins.
 * The renderer packs bytes only while its view is not ready. The load that
 * follows a served read is as current as the load that follows a queued hit:
 * both loads run after the handler returns. */
typedef struct CpuSurfaceView {
    _Atomic u64 sequence;   /* odd while the renderer changes this view */
    _Atomic u64 begin, end; /* guest byte range [begin, end), both 0: no surface */
    _Atomic u64 ready;      /* 1: a renderer copy of this surface would be a hit */
    _Atomic u64 noted;      /* 1 after a client read served this surface; the prefetch refresh takes it */
} CpuSurfaceView;
#define CPU_SURFACE_VIEW_ADDRESS 0x7FFFFFFFFFFFull   /* table address: bits 0 to 46 */
#define CPU_SURFACE_VIEW_COUNT 48                    /* entry count: bits 48 to 63 */
enum { CPU_SURFACE_VIEW_CHANGING, CPU_SURFACE_VIEW_CLEAR, CPU_SURFACE_VIEW_PENDING, CPU_SURFACE_VIEW_READY };
static _Atomic u64 s_cpu_surface_views;
static int s_cpu_surface_fast;      /* RSX_CPU_SURFACE_FAST=0 keeps every read queued */
static _Atomic u64 s_cpu_surface_fast_served;
static u64 cpu_surface_view_load(const _Atomic u64 *value) {
    return __c11_atomic_load(value,__ATOMIC_ACQUIRE);
}
static void cpu_surface_view_store(_Atomic u64 *value,u64 next) {
    __c11_atomic_store(value,next,__ATOMIC_RELEASE);
}
static u64 cpu_surface_view_exchange(_Atomic u64 *value,u64 next) {
    return __c11_atomic_exchange(value,next,__ATOMIC_ACQ_REL);
}
static u64 cpu_surface_view_increase(_Atomic u64 *value) {
    return __c11_atomic_fetch_add(value,1,__ATOMIC_RELAXED)+1;
}
/* One client read of guest bytes [address, address+bytes) against view v. */
static int cpu_surface_view_probe(const CpuSurfaceView *v,u32 address,u32 bytes) {
    u64 sequence=cpu_surface_view_load(&v->sequence);
    if(sequence&1)return CPU_SURFACE_VIEW_CHANGING;
    u64 begin=cpu_surface_view_load(&v->begin),end=cpu_surface_view_load(&v->end),ready=cpu_surface_view_load(&v->ready);
    if(cpu_surface_view_load(&v->sequence)!=sequence)return CPU_SURFACE_VIEW_CHANGING;
    if((u64)address>=end || (u64)address+bytes<=begin)return CPU_SURFACE_VIEW_CLEAR;
    return ready?CPU_SURFACE_VIEW_READY:CPU_SURFACE_VIEW_PENDING;
}
/* Client thread. Returns 1 when it served the read, so no request is queued.
 * A view that changes or waits for a copy makes the read take the queued
 * path. A read that touches no surface takes the queued path too. */
/* Hooks of rsx_deform_surface.inc, the CPU buffer of a paint target. They
 * are NULL while that file is not in the build, or until its init runs:
 * then every path below is as it was. */
static struct {
    int (*serve)(u32 address,u32 bytes);   /* 1: the CPU buffer holds every byte of the range */
    int (*owned)(u32 rsx_offset);          /* 1: level 0 of a listed surface, buffer mode on   */
    u64 (*sequence)(void);                 /* sum of the paint sequence numbers               */
} s_cpu_surface_deform;
static int cpu_surface_fast_request(u32 address,u32 bytes,u32 client_thread) {
    if(!s_cpu_surface_fast)return 0;
    if(s_cpu_surface_deform.serve&&s_cpu_surface_deform.serve(address,bytes))return 1;
    u64 views=cpu_surface_view_load(&s_cpu_surface_views);
    CpuSurfaceView *v=(CpuSurfaceView *)(uintptr_t)(views&CPU_SURFACE_VIEW_ADDRESS);
    u64 hits=0;
    for(u64 i=views>>CPU_SURFACE_VIEW_COUNT;i;i--,v++) {
        int state=cpu_surface_view_probe(v,address,bytes);
        if(state!=CPU_SURFACE_VIEW_CLEAR && state!=CPU_SURFACE_VIEW_READY)return 0;
        if(state==CPU_SURFACE_VIEW_READY){
            if(!cpu_surface_view_load(&v->noted))cpu_surface_view_store(&v->noted,1);
            hits++;
        }
    }
    if(!hits)return 0;
    CpuSurfaceRequest fast={0};fast.address=address;fast.bytes=bytes;fast.client_thread=client_thread;
    fast.profile.queued=fast.profile.started=fast.profile.finished=fast.profile.returned=cpu_surface_tick();
    fast.profile.matched=fast.profile.hits=hits;cpu_surface_profile_emit(&fast);
    u64 served=cpu_surface_view_increase(&s_cpu_surface_fast_served);
    if(s_cpu_surface_profile && !(served&1023))fprintf(stderr,"[CPUSURFACE_FAST] served=%llu\n",(unsigned long long)served);
    return 1;
}
/* Layout of the state that the snapshot range check reads twice: the view
 * table, the sum of its sequence numbers, the sequence number of the recorded
 * draws, and the number of surfaces that hold a range. */
enum { CPU_SURFACE_SNAPSHOT_TABLE, CPU_SURFACE_SNAPSHOT_SEQUENCES, CPU_SURFACE_SNAPSHOT_PENDING, CPU_SURFACE_SNAPSHOT_MATCHED, CPU_SURFACE_SNAPSHOT_SEEN };
static void cpu_surface_fence_wait(void) {
    if(s_cpu_surface_fence_value==UINT64_MAX)cpu_surface_fail("GPU fence counter overflow",E_FAIL);
    u64 value=++s_cpu_surface_fence_value;
    CPU_SURFACE_HR(s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue,s_cpu_surface_fence,value));
    u64 deadline=GetTickCount64()+30000;
    for(;;) {
        CPU_SURFACE_HR(s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device));
        u64 completed=s_cpu_surface_fence->lpVtbl->GetCompletedValue(s_cpu_surface_fence);
        if(completed==UINT64_MAX)cpu_surface_fail("GPU fence removed sentinel",E_FAIL);
        if(completed>=value)break;
        CPU_SURFACE_HR(s_cpu_surface_fence->lpVtbl->SetEventOnCompletion(s_cpu_surface_fence,value,s_cpu_surface_event));
        u64 now=GetTickCount64();
        if(now>=deadline)cpu_surface_fail("GPU fence deadline",E_FAIL);
        if(WaitForSingleObject(s_cpu_surface_event,(DWORD)(deadline-now))!=WAIT_OBJECT_0)
            cpu_surface_fail("GPU fence wait",E_FAIL);
        /* An older event registration may wake us. Only the target fence proves
         * that the copy is complete; keep the original bounded deadline. */
    }
}
/* GPU fence completion. A removed device reports UINT64_MAX as the completed
 * value of every fence; that is a failure, never a completion. */
static int cpu_surface_fence_reached(ID3D12Fence *fence,u64 value) {
    u64 completed=fence->lpVtbl->GetCompletedValue(fence);
    if(completed==UINT64_MAX) {
        HRESULT reason=s_d3d.device->lpVtbl->GetDeviceRemovedReason(s_d3d.device);
        cpu_surface_fail("GPU fence (device removed)",FAILED(reason)?reason:E_FAIL);
    }
    return completed>=value;
}
static void cpu_surface_fence_block(ID3D12Fence *fence,HANDLE event,u64 value,const char *operation) {
    if(cpu_surface_fence_reached(fence,value))return;
    CPU_SURFACE_HR(fence->lpVtbl->SetEventOnCompletion(fence,value,event));
    if(WaitForSingleObject(event,30000)!=WAIT_OBJECT_0 || !cpu_surface_fence_reached(fence,value))cpu_surface_fail(operation,E_FAIL);
}
/* Renderer thread only: one client-visible view for each slot of the surface
 * pool. CPU_SURFACE_CAPACITY is the slot count of the pool. */
static int cpu_surface_view_index(OffRT *r) {
    int index=(int)(r-s_d3d.off_rt);
    if(index<0 || index>=CPU_SURFACE_CAPACITY)cpu_surface_fail("surface view index",E_INVALIDARG);
    return index;
}
/* Renderer thread. The view of slot `index`, or NULL when the table has no
 * entry for it. */
static CpuSurfaceView *cpu_surface_view_entry(int index) {
    u64 views=cpu_surface_view_load(&s_cpu_surface_views);
    if((u64)index>=views>>CPU_SURFACE_VIEW_COUNT)return NULL;
    return (CpuSurfaceView *)(uintptr_t)(views&CPU_SURFACE_VIEW_ADDRESS)+index;
}
/* Renderer thread. The view of slot `index`. A slot beyond the table adds
 * entries: a table of twice the count (16 at first), then the old table
 * retires. Returns NULL when the table cannot grow. That slot then has no
 * view, and reads of it keep the queued path. */
static CpuSurfaceView *cpu_surface_view_slot(int index) {
    CpuSurfaceView *view=cpu_surface_view_entry(index);
    if(view)return view;
    u64 views=cpu_surface_view_load(&s_cpu_surface_views),count=views>>CPU_SURFACE_VIEW_COUNT;
    CpuSurfaceView *old=(CpuSurfaceView *)(uintptr_t)(views&CPU_SURFACE_VIEW_ADDRESS);
    u64 next=count?count*2:16;
    while(next<=(u64)index && next<=0x8000)next*=2;
    if(next>0xFFFF)return NULL;
    CpuSurfaceView *table=(CpuSurfaceView *)calloc((size_t)next,sizeof(CpuSurfaceView));
    if(!table)return NULL;
    if((uintptr_t)table&~CPU_SURFACE_VIEW_ADDRESS){free(table);return NULL;}
    for(u64 i=0;i<count;i++) {
        cpu_surface_view_store(&table[i].sequence,cpu_surface_view_load(&old[i].sequence));
        cpu_surface_view_store(&table[i].begin,cpu_surface_view_load(&old[i].begin));
        cpu_surface_view_store(&table[i].end,cpu_surface_view_load(&old[i].end));
        cpu_surface_view_store(&table[i].ready,cpu_surface_view_load(&old[i].ready));
        cpu_surface_view_store(&table[i].noted,cpu_surface_view_load(&old[i].noted));
    }
    cpu_surface_view_store(&s_cpu_surface_views,(uintptr_t)table|next<<CPU_SURFACE_VIEW_COUNT);
    for(u64 i=0;i<count;i++)cpu_surface_view_store(&old[i].sequence,cpu_surface_view_load(&old[i].sequence)+1);
    return table+index;
}
/* Renderer thread. Publishes r as cpu_surface_read and cpu_surface_copy see
 * it now: the guest bytes r owns and whether a copy of r would be a hit. The
 * new view is visible only after all four fields are stored. */
static void cpu_surface_view_update(OffRT *r) {
    CpuSurfaceView *v=cpu_surface_view_slot(cpu_surface_view_index(r));
    if(!v)return;
    int live=r->res && r->cpu_address && !(s_cpu_surface_deform.owned&&s_cpu_surface_deform.owned(r->off));
    u64 begin=live?(u64)r->cpu_address:0,end=live?begin+(u64)r->cpu_pitch*r->h:0;
    u64 ready=!r->cpu_pitch || !r->gpu_generation || r->cpu_generation==r->gpu_generation;
    u64 sequence=cpu_surface_view_load(&v->sequence);
    cpu_surface_view_store(&v->sequence,sequence+1);
    cpu_surface_view_store(&v->begin,begin);cpu_surface_view_store(&v->end,end);cpu_surface_view_store(&v->ready,ready);
    cpu_surface_view_store(&v->sequence,sequence+2);
}
/* Renderer thread. r left its slot. No client may serve the old range, and a
 * noted read must not reach the next surface of the slot. */
static void cpu_surface_view_retire(OffRT *r) {
    cpu_surface_view_update(r);
    CpuSurfaceView *v=cpu_surface_view_entry(cpu_surface_view_index(r));
    if(v)cpu_surface_view_exchange(&v->noted,0);
}
/* Prefetched copies of opted-in surfaces. RSX_CPU_SURFACE_PREFETCH lists up
 * to four RSX surface offsets (1-8 hex digits each, comma separated); none by
 * default. Reads stay coherent: a CPU read sees the surface as of every write
 * submitted before it is served (its current gpu_generation), exactly as with
 * an empty list. Only the source of those bytes changes. After each submitted
 * batch, a listed surface that the CPU has read since its last copy and that
 * has a newer generation gets a copy queued behind that batch. When a read
 * then needs exactly that generation, the renderer packs the prefetched copy,
 * waiting only for that copy's own fence (usually already passed) instead of
 * a fence behind all submitted work followed by a fresh copy. Anything else
 * (no copy, an older generation, a changed layout) takes the coherent copy.
 * In MotorStorm races the main thread reads the 2048x2048 deformation surface
 * (offset 01A80500) hundreds of times a second, probes a 1280x720 surface
 * (00A80500) and reads the 64x64 HDR exposure target (02A30480); the
 * launcher's --surface-prefetch 1 lists those three.
 * RSX_CPU_SURFACE_PREFETCH_LOG=1 prints used/waited/missed/copy counts. */
#define CPU_SURFACE_PREFETCH_MAX 4
typedef struct CpuSurfacePrefetch {
    u32 off;
    int slot;                     /* tracked OffRT index, -1 none */
    int wanted;                   /* read since the last copy was queued */
    u32 address, pitch, w, width, height, format;   /* layout of the queued copy */
    u64 generation;               /* gpu_generation the queued copy holds, 0 none */
    u64 fence;                    /* prefetch fence value that completes it */
    ID3D12Resource *readback;
    u64 readback_bytes;
    u32 readback_pitch;
    unsigned long long copies, used, waited, missed;
} CpuSurfacePrefetch;
static CpuSurfacePrefetch s_cpu_surface_prefetch[CPU_SURFACE_PREFETCH_MAX];
static unsigned s_cpu_surface_prefetch_count;
static int s_cpu_surface_prefetch_log;
static ID3D12CommandAllocator *s_cpu_surface_prefetch_alloc;
static ID3D12GraphicsCommandList *s_cpu_surface_prefetch_list;
static ID3D12Fence *s_cpu_surface_prefetch_fence;
static HANDLE s_cpu_surface_prefetch_event;
static u64 s_cpu_surface_prefetch_fence_value;   /* last value signalled */
static int cpu_surface_hex_digit(char c) {
    return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
}
static void cpu_surface_prefetch_configure(const char *list) {
    s_cpu_surface_prefetch_count=0;
    if(!list || !*list)return;
    for(;;) {
        u64 off=0;unsigned digits=0;int d;
        while((d=cpu_surface_hex_digit(*list))>=0){off=off*16u+(u64)d;list++;
            if(++digits>8)cpu_surface_fail("RSX_CPU_SURFACE_PREFETCH (offsets are 1-8 hex digits)",E_INVALIDARG);}
        if(!digits || (*list && *list!=',') || s_cpu_surface_prefetch_count==CPU_SURFACE_PREFETCH_MAX)
            cpu_surface_fail("RSX_CPU_SURFACE_PREFETCH (up to 4 comma-separated hex offsets)",E_INVALIDARG);
        for(unsigned i=0;i<s_cpu_surface_prefetch_count;i++)
            if(s_cpu_surface_prefetch[i].off==(u32)off)cpu_surface_fail("RSX_CPU_SURFACE_PREFETCH (duplicate offset)",E_INVALIDARG);
        CpuSurfacePrefetch *e=&s_cpu_surface_prefetch[s_cpu_surface_prefetch_count++];
        memset(e,0,sizeof *e);e->off=(u32)off;e->slot=-1;
        if(!*list)return;
        list++;
    }
}
static void cpu_surface_prefetch_untrack(CpuSurfacePrefetch *e) {
    e->slot=-1;e->wanted=0;e->generation=0;e->fence=0;
    e->address=e->pitch=e->w=e->width=e->height=e->format=0;
}
/* Packed guest width of r's copy; 0 when r cannot be prefetched. */
static u32 cpu_surface_prefetch_width(const OffRT *r) {
    unsigned bpp=rsx_surface_readback_bpp(r->cpu_format);
    if(!bpp || !r->cpu_address || !r->cpu_pitch || r->cpu_pitch%bpp || !r->w || !r->h)return 0;
    if((u64)r->cpu_address+(u64)r->cpu_pitch*r->h>0x100000000ull)return 0;
    u32 width=r->w;if(width>r->cpu_pitch/bpp)width=r->cpu_pitch/bpp;
    return width;
}
static CpuSurfacePrefetch *cpu_surface_prefetch_entry(const OffRT *r) {
    for(unsigned i=0;i<s_cpu_surface_prefetch_count;i++) {
        CpuSurfacePrefetch *e=&s_cpu_surface_prefetch[i];
        if(e->slot>=0 && &s_d3d.off_rt[e->slot]==r)return e;
    }
    return NULL;
}
/* A CPU read matched r: copy its next generation ahead. */
static void cpu_surface_prefetch_note(const OffRT *r) {
    CpuSurfacePrefetch *e=cpu_surface_prefetch_entry(r);
    if(e)e->wanted=1;
}
/* Serve a read of r from its prefetched copy if that copy holds r's current
 * generation in r's current layout. Returns 1 when vm_base now holds it. */
static int cpu_surface_prefetch_take(OffRT *r) {
    extern uint8_t *vm_base;
    CpuSurfacePrefetch *e=cpu_surface_prefetch_entry(r);
    if(!e)return 0;
    u32 width=cpu_surface_prefetch_width(r);
    if(!e->generation || e->generation!=r->gpu_generation || !width || width!=e->width || r->w!=e->w ||
       r->cpu_address!=e->address || r->cpu_pitch!=e->pitch || r->h!=e->height || r->cpu_format!=e->format){e->missed++;return 0;}
    if(!cpu_surface_fence_reached(s_cpu_surface_prefetch_fence,e->fence)) {
        e->waited++;cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,e->fence,"prefetch GPU fence wait");
    }
    D3D12_RANGE range={0,(SIZE_T)e->readback_bytes};void *mapped=NULL;
    CPU_SURFACE_HR(e->readback->lpVtbl->Map(e->readback,0,&range,&mapped));
    if(!mapped || rsx_surface_readback_rows(vm_base+r->cpu_address,(size_t)((u64)r->cpu_pitch*r->h),r->cpu_pitch,
        mapped,(size_t)e->readback_bytes,e->readback_pitch,width,r->h,r->cpu_format))cpu_surface_fail("prefetched surface packing",E_INVALIDARG);
    D3D12_RANGE empty={0,0};e->readback->lpVtbl->Unmap(e->readback,0,&empty);
    r->cpu_generation=r->gpu_generation;e->used++;cpu_surface_view_update(r);
    return 1;
}
/* Follow the surface at e->off. Returns it, or NULL when it is not live. */
static OffRT *cpu_surface_prefetch_track(CpuSurfacePrefetch *e) {
    for(int i=0;i<CPU_SURFACE_CAPACITY;i++) {
        OffRT *r=&s_d3d.off_rt[i];
        if(r->res && r->off==e->off && r->cpu_address && r->cpu_pitch) {
            if(e->slot!=i){cpu_surface_prefetch_untrack(e);e->slot=i;}
            return r;
        }
    }
    if(e->slot>=0)cpu_surface_prefetch_untrack(e);
    return NULL;
}
/* Queue copies of the given surfaces behind every batch submitted so far. */
static void cpu_surface_prefetch_issue(CpuSurfacePrefetch **entries,OffRT **surfaces,unsigned count) {
    CPU_SURFACE_HR(s_cpu_surface_prefetch_alloc->lpVtbl->Reset(s_cpu_surface_prefetch_alloc));
    CPU_SURFACE_HR(s_cpu_surface_prefetch_list->lpVtbl->Reset(s_cpu_surface_prefetch_list,s_cpu_surface_prefetch_alloc,NULL));
    for(unsigned i=0;i<count;i++) {
        CpuSurfacePrefetch *e=entries[i];OffRT *r=surfaces[i];
        u32 pitch=rsx_surface_readback_host_pitch(r->w,r->cpu_format,D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        u64 bytes=(u64)pitch*r->h;
        if(!pitch || bytes>256u*1024u*1024u)cpu_surface_fail("prefetch surface copy bounds",E_INVALIDARG);
        if(e->readback && e->readback_bytes!=bytes){e->readback->lpVtbl->Release(e->readback);e->readback=NULL;}
        if(!e->readback) {
            D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc={0};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
            desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&desc,
                D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&e->readback));
            e->readback_bytes=bytes;
        }
        e->readback_pitch=pitch;
        D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource=r->res;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore=r->st;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
        if(r->st!=D3D12_RESOURCE_STATE_COPY_SOURCE)s_cpu_surface_prefetch_list->lpVtbl->ResourceBarrier(s_cpu_surface_prefetch_list,1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=r->res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource=e->readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format=(DXGI_FORMAT)r->dxgi;
        dst.PlacedFootprint.Footprint.Width=r->w;dst.PlacedFootprint.Footprint.Height=r->h;
        dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=pitch;
        s_cpu_surface_prefetch_list->lpVtbl->CopyTextureRegion(s_cpu_surface_prefetch_list,&dst,0,0,0,&src,NULL);
        if(r->st!=D3D12_RESOURCE_STATE_COPY_SOURCE) {
            barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=r->st;
            s_cpu_surface_prefetch_list->lpVtbl->ResourceBarrier(s_cpu_surface_prefetch_list,1,&barrier);
        }
    }
    CPU_SURFACE_HR(s_cpu_surface_prefetch_list->lpVtbl->Close(s_cpu_surface_prefetch_list));
    ID3D12CommandList *lists[]={(ID3D12CommandList *)s_cpu_surface_prefetch_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue,1,lists);
    CPU_SURFACE_HR(s_d3d.cmd_queue->lpVtbl->Signal(s_d3d.cmd_queue,s_cpu_surface_prefetch_fence,++s_cpu_surface_prefetch_fence_value));
}
static void cpu_surface_prefetch_print(void) {
    for(unsigned i=0;i<s_cpu_surface_prefetch_count;i++) {
        CpuSurfacePrefetch *e=&s_cpu_surface_prefetch[i];
        fprintf(stderr,"[CPUSURFACE_PREFETCH] offset=%08X used=%llu waited=%llu missed=%llu copies=%llu\n",
            e->off,e->used,e->waited,e->missed,e->copies);
    }
}
/* Prefetch refresh, once per submitted batch. */
static void cpu_surface_prefetch_refresh(void) {
    if(!s_cpu_surface_prefetch_count)return;
    if(s_cpu_surface_recording)cpu_surface_fail("surface prefetch before command submission",E_FAIL);
    CpuSurfacePrefetch *entries[CPU_SURFACE_PREFETCH_MAX];OffRT *surfaces[CPU_SURFACE_PREFETCH_MAX];unsigned count=0;
    for(unsigned i=0;i<s_cpu_surface_prefetch_count;i++) {
        CpuSurfacePrefetch *e=&s_cpu_surface_prefetch[i];OffRT *r=cpu_surface_prefetch_track(e);
        /* A read a client thread served itself asks for the next copy as well. */
        CpuSurfaceView *view=r?cpu_surface_view_entry(cpu_surface_view_index(r)):NULL;
        if(view && cpu_surface_view_exchange(&view->noted,0))e->wanted=1;
        if(r && e->wanted && r->gpu_generation && r->gpu_generation!=e->generation && cpu_surface_prefetch_width(r)){entries[count]=e;surfaces[count++]=r;}
    }
    if(!count)return;
    /* The allocator still backs the last copies until their fence passes. */
    if(!cpu_surface_fence_reached(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_fence_value))return;
    cpu_surface_prefetch_issue(entries,surfaces,count);
    for(unsigned i=0;i<count;i++) {
        CpuSurfacePrefetch *e=entries[i];OffRT *r=surfaces[i];
        e->generation=r->gpu_generation;e->fence=s_cpu_surface_prefetch_fence_value;e->wanted=0;e->copies++;
        e->address=r->cpu_address;e->pitch=r->cpu_pitch;e->w=r->w;e->width=cpu_surface_prefetch_width(r);e->height=r->h;e->format=r->cpu_format;
    }
    static unsigned long long issues;
    if(s_cpu_surface_prefetch_log && !(++issues%1000))cpu_surface_prefetch_print();
}
/* r is being retired: its copy must not outlive the resource or be reused. */
static void cpu_surface_prefetch_retire(OffRT *r) {
    CpuSurfacePrefetch *e=cpu_surface_prefetch_entry(r);
    if(!e)return;
    if(e->generation)cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,e->fence,"prefetch GPU fence wait");
    cpu_surface_prefetch_untrack(e);
}
static void cpu_surface_prefetch_init(void) {
    cpu_surface_prefetch_configure(getenv("RSX_CPU_SURFACE_PREFETCH"));
    const char *log=getenv("RSX_CPU_SURFACE_PREFETCH_LOG");
    s_cpu_surface_prefetch_log=log&&log[0]=='1'&&!log[1];
    s_cpu_surface_prefetch_fence_value=0;
    if(!s_cpu_surface_prefetch_count)return;
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommandAllocator(s_d3d.device,D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator,(void **)&s_cpu_surface_prefetch_alloc));
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommandList(s_d3d.device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_cpu_surface_prefetch_alloc,NULL,&IID_ID3D12GraphicsCommandList,(void **)&s_cpu_surface_prefetch_list));
    CPU_SURFACE_HR(s_cpu_surface_prefetch_list->lpVtbl->Close(s_cpu_surface_prefetch_list));
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void **)&s_cpu_surface_prefetch_fence));
    s_cpu_surface_prefetch_event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!s_cpu_surface_prefetch_event)cpu_surface_fail("prefetch GPU event creation",E_FAIL);
    for(unsigned i=0;i<s_cpu_surface_prefetch_count;i++)
        fprintf(stderr,"[CPUSURFACE_PREFETCH] enabled offset=%08X\n",s_cpu_surface_prefetch[i].off);
}
static void cpu_surface_prefetch_shutdown(void) {
    if(!s_cpu_surface_prefetch_count)return;
    cpu_surface_fence_block(s_cpu_surface_prefetch_fence,s_cpu_surface_prefetch_event,s_cpu_surface_prefetch_fence_value,"prefetch GPU fence wait");
    if(s_cpu_surface_prefetch_log)cpu_surface_prefetch_print();
    for(unsigned i=0;i<s_cpu_surface_prefetch_count;i++) {
        CpuSurfacePrefetch *e=&s_cpu_surface_prefetch[i];
        cpu_surface_prefetch_untrack(e);
        if(e->readback){e->readback->lpVtbl->Release(e->readback);e->readback=NULL;}
    }
    s_cpu_surface_prefetch_count=0;
    if(s_cpu_surface_prefetch_list)s_cpu_surface_prefetch_list->lpVtbl->Release(s_cpu_surface_prefetch_list);
    if(s_cpu_surface_prefetch_alloc)s_cpu_surface_prefetch_alloc->lpVtbl->Release(s_cpu_surface_prefetch_alloc);
    if(s_cpu_surface_prefetch_fence)s_cpu_surface_prefetch_fence->lpVtbl->Release(s_cpu_surface_prefetch_fence);
    if(s_cpu_surface_prefetch_event)CloseHandle(s_cpu_surface_prefetch_event);
    s_cpu_surface_prefetch_list=NULL;s_cpu_surface_prefetch_alloc=NULL;s_cpu_surface_prefetch_fence=NULL;s_cpu_surface_prefetch_event=NULL;
}
static void cpu_surface_copy(OffRT *r) {
    extern uint8_t *vm_base;
    if(s_cpu_surface_deform.owned&&s_cpu_surface_deform.owned(r->off))return;
    if(!r->res || !r->cpu_address || !r->cpu_pitch || !r->gpu_generation || r->cpu_generation==r->gpu_generation){if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->hits++;return;}
    if(cpu_surface_prefetch_take(r))return;   /* the prefetched copy holds this generation */
    unsigned bpp=rsx_surface_readback_bpp(r->cpu_format);
    if(!bpp || r->cpu_pitch%bpp)cpu_surface_fail("unsupported guest surface layout",E_INVALIDARG);
    u32 width=r->w;
    if(width>r->cpu_pitch/bpp)width=r->cpu_pitch/bpp;
    u32 pitch=rsx_surface_readback_host_pitch(r->w,r->cpu_format,D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    u64 bytes=(u64)pitch*r->h;
    u64 guest_bytes=(u64)r->cpu_pitch*r->h;
    if(!width || !r->h || !pitch || bytes>256u*1024u*1024u || (u64)r->cpu_address+guest_bytes>0x100000000ull)
        cpu_surface_fail("surface copy bounds",E_INVALIDARG);
    u64 profile_begin=cpu_surface_tick();
    if(!r->cpu_readback) {
        D3D12_HEAP_PROPERTIES hp={0};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc={0};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;desc.MipLevels=1;desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommittedResource(s_d3d.device,&hp,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_COPY_DEST,NULL,&IID_ID3D12Resource,(void **)&r->cpu_readback));
    }
    if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->allocation+=cpu_surface_tick()-profile_begin;
    profile_begin=cpu_surface_tick();
    cpu_surface_fence_wait();
    if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->prior_fence+=cpu_surface_tick()-profile_begin;
    profile_begin=cpu_surface_tick();
    CPU_SURFACE_HR(s_cpu_surface_alloc->lpVtbl->Reset(s_cpu_surface_alloc));
    CPU_SURFACE_HR(s_cpu_surface_list->lpVtbl->Reset(s_cpu_surface_list,s_cpu_surface_alloc,NULL));
    D3D12_RESOURCE_BARRIER barrier={0};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=r->res;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=r->st;barrier.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    if(r->st!=D3D12_RESOURCE_STATE_COPY_SOURCE)s_cpu_surface_list->lpVtbl->ResourceBarrier(s_cpu_surface_list,1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION src={0},dst={0};src.pResource=r->res;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource=r->cpu_readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format=(DXGI_FORMAT)r->dxgi;
    dst.PlacedFootprint.Footprint.Width=r->w;dst.PlacedFootprint.Footprint.Height=r->h;
    dst.PlacedFootprint.Footprint.Depth=1;dst.PlacedFootprint.Footprint.RowPitch=pitch;
    s_cpu_surface_list->lpVtbl->CopyTextureRegion(s_cpu_surface_list,&dst,0,0,0,&src,NULL);
    if(r->st!=D3D12_RESOURCE_STATE_COPY_SOURCE) {
        barrier.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;barrier.Transition.StateAfter=r->st;
        s_cpu_surface_list->lpVtbl->ResourceBarrier(s_cpu_surface_list,1,&barrier);
    }
    CPU_SURFACE_HR(s_cpu_surface_list->lpVtbl->Close(s_cpu_surface_list));
    ID3D12CommandList *lists[]={(ID3D12CommandList *)s_cpu_surface_list};
    s_d3d.cmd_queue->lpVtbl->ExecuteCommandLists(s_d3d.cmd_queue,1,lists);
    if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->commands+=cpu_surface_tick()-profile_begin;
    profile_begin=cpu_surface_tick();
    cpu_surface_fence_wait();
    if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->copy_fence+=cpu_surface_tick()-profile_begin;
    profile_begin=cpu_surface_tick();
    D3D12_RANGE range={0,(SIZE_T)bytes};void *mapped=NULL;
    CPU_SURFACE_HR(r->cpu_readback->lpVtbl->Map(r->cpu_readback,0,&range,&mapped));
    if(!mapped || rsx_surface_readback_rows(vm_base+r->cpu_address,(size_t)guest_bytes,r->cpu_pitch,
        mapped,(size_t)bytes,pitch,width,r->h,r->cpu_format))cpu_surface_fail("guest surface packing",E_INVALIDARG);
    D3D12_RANGE empty={0,0};r->cpu_readback->lpVtbl->Unmap(r->cpu_readback,0,&empty);
    r->cpu_generation=r->gpu_generation;cpu_surface_view_update(r);
    if(s_cpu_surface_profile_active) {
        s_cpu_surface_profile_active->packing+=cpu_surface_tick()-profile_begin;
        s_cpu_surface_profile_active->copies++;
        s_cpu_surface_profile_active->host_bytes+=bytes;
        s_cpu_surface_profile_active->guest_bytes+=guest_bytes;
    }
    static unsigned logged;const char *log=getenv("RSX_CPU_SURFACE_LOG");
    if(log && logged++<(unsigned)strtoul(log,NULL,10))fprintf(stderr,"[CPUSURFACE] synchronized ea=%08X offset=%08X pitch=%u size=%ux%u format=%u generation=%llu\n",
        r->cpu_address,r->off,r->cpu_pitch,width,r->h,r->cpu_format&31u,(unsigned long long)r->gpu_generation);
}
static void cpu_surface_read(u32 address,u32 bytes) {
    if(s_cpu_surface_deform.serve&&s_cpu_surface_deform.serve(address,bytes))return;
    for(int i=0;i<CPU_SURFACE_CAPACITY;i++) {
        OffRT *r=&s_d3d.off_rt[i];
        if(!r->res || !r->cpu_address)continue;
        u64 end=(u64)r->cpu_address+(u64)r->cpu_pitch*r->h;
        if((u64)address<end && (u64)address+bytes>r->cpu_address){
            if(s_cpu_surface_recording && r->gpu_generation && r->cpu_generation!=r->gpu_generation)cpu_surface_fail("CPU load requires pending recorded surface writes",E_FAIL);
            if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->matched++;
            cpu_surface_prefetch_note(r);
            cpu_surface_copy(r);
        }
    }
}
/* Only the checked renderer owner calls this bounded, opt-in observation.
 * All extents and state belong to the admission boundary being observed. */
static void cpu_surface_snapshot_trace(CpuSurfaceRequest *request) {
    static unsigned sequence;
    if(!s_cpu_surface_profile || sequence>64)return;
    unsigned current=++sequence;
    if(current>64) {
        fputs("[CPUSNAPSHOT_CAP] limit=64\n",stderr);
        return;
    }
    request->snapshot_trace_sequence=current;
    fprintf(stderr,"[CPUSNAPSHOT_STATE] seq=%u client=%u renderer=%lu state=%u open=%u draws=%u recording=%u ranges=%u\n",
        current,request->client_thread,(unsigned long)GetCurrentThreadId(),
        s_d3d.current_rsx_state!=NULL,
        s_d3d.current_rsx_state?s_d3d.current_rsx_state->in_begin_end:0,
        s_d3d.draw_count,s_cpu_surface_recording,request->snapshot_count);
    for(u32 i=0;i<request->snapshot_count;i++) {
        const VmReadSnapshotRange *range=&request->snapshot_ranges[i];
        fprintf(stderr,"[CPUSNAPSHOT_RANGE] seq=%u index=%u address=%08X bytes=%u output=%u\n",
            current,i,range->address,range->bytes,range->output_offset);
    }
    for(int i=0;i<CPU_SURFACE_CAPACITY;i++) {
        const OffRT *target=&s_d3d.off_rt[i];
        if(!target->res || !target->cpu_address || !target->cpu_pitch || !target->h)continue;
        fprintf(stderr,"[CPUSNAPSHOT_VIEW] seq=%u slot=%d address=%08X pitch=%u height=%u format=%u gpu=%llu cpu=%llu\n",
            current,i,target->cpu_address,target->cpu_pitch,target->h,target->cpu_format,
            (unsigned long long)target->gpu_generation,(unsigned long long)target->cpu_generation);
    }
}
/* The renderer owner synchronizes unique live GPU views before staging the
 * complete batch. CPU ranges include memory flushed by normal target retirement.
 * The caller keeps source allocations alive throughout the request. */
static int cpu_surface_snapshot(const VmReadSnapshotRange *ranges,u32 count,
    void *staging,u32 output_bytes) {
    extern uint8_t *vm_base;
    if(!ranges || !staging || !count || count>VM_READ_SNAPSHOT_MAX_RANGES ||
        !output_bytes || output_bytes>VM_READ_SNAPSHOT_MAX_BYTES)
        return VM_READ_SNAPSHOT_INVALID;
    /* A consumed FIFO can still leave an open primitive or CPU draw records.
     * Let the caller retain its reference path until normal rendering retires
     * that work. Never stage a snapshot ahead of it. */
    if (!s_d3d.current_rsx_state ||
        s_d3d.current_rsx_state->in_begin_end ||
        s_d3d.draw_count)
        return VM_READ_SNAPSHOT_UNAVAILABLE;
    RsxCpuSurfaceSnapshotView views[CPU_SURFACE_CAPACITY];
    OffRT *targets[CPU_SURFACE_CAPACITY];
    unsigned view_count=0;
    for(int i=0;i<CPU_SURFACE_CAPACITY;i++) {
        OffRT *r=&s_d3d.off_rt[i];
        if(!r->res || !r->cpu_address || !r->cpu_pitch || !r->h)continue;
        if(s_cpu_surface_deform.owned&&s_cpu_surface_deform.owned(r->off))continue;
        views[view_count]=(RsxCpuSurfaceSnapshotView){r->cpu_address,r->cpu_pitch,r->h};
        targets[view_count++]=r;
    }
    RsxCpuSurfaceSnapshotBinding bindings[VM_READ_SNAPSHOT_MAX_RANGES];
    for(u32 i=0;i<count;i++) {
        const uint64_t begin=ranges[i].address;
        const uint64_t end=begin+ranges[i].bytes;
        if(!ranges[i].bytes || end>UINT64_C(0x100000000) ||
            (uint64_t)ranges[i].output_offset+ranges[i].bytes>output_bytes)
            return VM_READ_SNAPSHOT_INVALID;
        int intersects=0;
        for(unsigned j=0;j<view_count;j++) {
            const uint64_t view_end=(uint64_t)views[j].address+
                (uint64_t)views[j].pitch*views[j].height;
            if(begin<view_end && end>views[j].address)intersects=1;
        }
        /* A range the CPU buffer holds needs no view and no copy. */
        if(intersects&&s_cpu_surface_deform.serve&&s_cpu_surface_deform.serve(ranges[i].address,ranges[i].bytes))intersects=0;
        bindings[i].view_index=UINT32_MAX;
        if(intersects && !rsx_cpu_surface_snapshot_bind(&ranges[i],1,
            views,view_count,&bindings[i]))
            return VM_READ_SNAPSHOT_UNAVAILABLE;
    }
    for(u32 i=0;i<count;i++) {
        if(bindings[i].view_index==UINT32_MAX)continue;
        OffRT *r=targets[bindings[i].view_index];
        if(s_cpu_surface_recording && r->gpu_generation && r->cpu_generation!=r->gpu_generation)
            cpu_surface_fail("CPU snapshot requires pending recorded surface writes",E_FAIL);
    }
    for(u32 i=0;i<count;i++) {
        if(bindings[i].view_index==UINT32_MAX)continue;
        int seen=0;for(u32 j=0;j<i;j++)if(bindings[j].view_index==bindings[i].view_index){seen=1;break;}
        if(seen)continue;
        if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->matched++;
        cpu_surface_copy(targets[bindings[i].view_index]);
    }
    for(u32 i=0;i<count;i++) {
        SIZE_T copied=0;
        if(!ReadProcessMemory(GetCurrentProcess(),vm_base+ranges[i].address,
            (uint8_t *)staging+ranges[i].output_offset,ranges[i].bytes,&copied) ||
            copied!=ranges[i].bytes)
            return VM_READ_SNAPSHOT_UNAVAILABLE;
    }
    return VM_READ_SNAPSHOT_OK;
}
static void cpu_surface_service(int snapshot_boundary) {
    LONG thread=(LONG)GetCurrentThreadId();
    LONG owner=InterlockedCompareExchange(&s_cpu_surface_thread,thread,0);
    if(owner && owner!=thread)cpu_surface_fail("renderer service thread changed",E_FAIL);
    AcquireSRWLockExclusive(&s_cpu_surface_lock);
    CpuSurfaceRequest *request=s_cpu_surface_head;
    if(snapshot_boundary)s_cpu_surface_head=s_cpu_surface_tail=NULL;
    else {
        CpuSurfaceRequest *before=NULL,*cursor=request;
        while(cursor && !cursor->snapshot_ranges){before=cursor;cursor=cursor->next;}
        if(before){before->next=NULL;s_cpu_surface_head=cursor;if(!cursor)s_cpu_surface_tail=NULL;}
        else request=NULL;
    }
    ReleaseSRWLockExclusive(&s_cpu_surface_lock);
    while(request) {
        CpuSurfaceRequest *next=request->next;
        request->profile.started=cpu_surface_tick();
        s_cpu_surface_profile_active=s_cpu_surface_profile?&request->profile:NULL;
        if(request->snapshot_ranges) {
            cpu_surface_snapshot_trace(request);
            request->snapshot_result=cpu_surface_snapshot(request->snapshot_ranges,request->snapshot_count,
                request->snapshot_staging,request->snapshot_output_bytes);
        } else cpu_surface_read(request->address,request->bytes);
        request->status=0;
        s_cpu_surface_profile_active=NULL;
        request->profile.finished=cpu_surface_tick();
        u64 elapsed=GetTickCount64()-request->queued_ms;
        if(elapsed>=1000)fprintf(stderr,"[CPUSURFACE] complete address=%08X bytes=%u elapsed_ms=%llu renderer=%lu\n",
            request->address,request->bytes,(unsigned long long)elapsed,(unsigned long)GetCurrentThreadId());
        if(!SetEvent(request->done))cpu_surface_fail("CPU completion event",E_FAIL);
        request=next;
    }
}
/* Called only after a complete batch has submitted, retired frame-local
 * staging/capture work and advanced its allocator/fence lifetime. A FIFO
 * drain may render several batches before the outer message pump runs. */
static void cpu_surface_frame_complete(void) {
    if(s_cpu_surface_recording)cpu_surface_fail("CPU service before command submission",E_FAIL);
    cpu_surface_service(0);
}
static void submit_record_capacity(void* context, int present);
static int cpu_surface_snapshot_boundary(void *opaque) {
    (void)opaque;
    if(s_cpu_surface_recording)cpu_surface_fail("CPU snapshot before command submission",E_FAIL);
    /* Completed records use normal nonpresenting capacity submission after
     * primitive closure. */
    if(s_d3d.current_rsx_state &&
        !s_d3d.current_rsx_state->in_begin_end && s_d3d.draw_count)
        submit_record_capacity(NULL, 0);
    cpu_surface_service(1);return 1;
}
static int cpu_surface_snapshot_pending(void) {
    int pending=0;AcquireSRWLockShared(&s_cpu_surface_lock);
    for(CpuSurfaceRequest *r=s_cpu_surface_head;r;r=r->next)if(r->snapshot_ranges){pending=1;break;}
    ReleaseSRWLockShared(&s_cpu_surface_lock);return pending;
}
static void cpu_surface_request(u32 address,u32 bytes,void *opaque) {
    (void)opaque;
    u32 client_thread=(u32)GetCurrentThreadId();
    if((LONG)client_thread==InterlockedCompareExchange(&s_cpu_surface_thread,0,0)){
        CpuSurfaceRequest direct={0};direct.address=address;direct.bytes=bytes;direct.client_thread=client_thread;
        direct.profile.queued=direct.profile.started=cpu_surface_tick();
        RsxCpuSurfaceProfile *previous=s_cpu_surface_profile_active;
        s_cpu_surface_profile_active=s_cpu_surface_profile?&direct.profile:NULL;
        cpu_surface_read(address,bytes);s_cpu_surface_profile_active=previous;
        direct.profile.finished=direct.profile.returned=cpu_surface_tick();cpu_surface_profile_emit(&direct);return;
    }
    if(cpu_surface_fast_request(address,bytes,client_thread))return;
    CpuSurfaceRequest request={0};request.address=address;request.bytes=bytes;request.client_thread=client_thread;request.status=-1;request.queued_ms=GetTickCount64();request.profile.queued=cpu_surface_tick();
    request.done=CreateEventA(NULL,FALSE,FALSE,NULL);
    if(!request.done)cpu_surface_fail("CPU read event creation",E_FAIL);
    AcquireSRWLockExclusive(&s_cpu_surface_lock);
    if(!s_cpu_surface_live){ReleaseSRWLockExclusive(&s_cpu_surface_lock);CloseHandle(request.done);return;}
    if(s_cpu_surface_tail)s_cpu_surface_tail->next=&request;else s_cpu_surface_head=&request;
    s_cpu_surface_tail=&request;ReleaseSRWLockExclusive(&s_cpu_surface_lock);
    extern void *cellGcm_fifo_kick_event(void);
    HANDLE kick=(HANDLE)cellGcm_fifo_kick_event();if(kick&&!SetEvent(kick))cpu_surface_fail("renderer kick",E_FAIL);
    DWORD wait=WaitForSingleObject(request.done,30000);
    DWORD wait_error=wait==WAIT_FAILED?GetLastError():0;
    if(wait!=WAIT_OBJECT_0 || request.status) {
        fprintf(stderr,"[CPUSURFACE] request address=%08X bytes=%u wait=%08lX error=%lu status=%d elapsed_ms=%llu requester=%lu renderer=%ld\n",
            address,bytes,(unsigned long)wait,(unsigned long)wait_error,request.status,
            (unsigned long long)(GetTickCount64()-request.queued_ms),
            (unsigned long)GetCurrentThreadId(),s_cpu_surface_thread);
        cpu_surface_fail("CPU read synchronization",E_FAIL);
    }
    request.profile.returned=cpu_surface_tick();
    cpu_surface_profile_emit(&request);
    CloseHandle(request.done);
}
/* Snapshot fast path, client thread. RSX_CPU_SURFACE_SNAPSHOT_FAST=1 enables
 * it; by default every snapshot request takes the FIFO boundary path.
 * A request is served on the calling thread when the renderer holds nothing
 * for its bytes: no recorded draw targets a range, and every surface that
 * meets a range is current and holds the whole range. The guest bytes are
 * then the bytes a scalar read of the same addresses serves on its calling
 * thread (cpu_surface_fast_request). The check does not consume the FIFO: a
 * draw that the guest published and the renderer did not record yet is not
 * seen, as for a scalar read.
 * The client reads the bytes into its own buffer and then repeats the check.
 * The renderer writes the guest bytes of a surface only while that surface
 * is not current, and it changes a view or the recorded draws only between
 * two steps of a sequence number. An equal second check proves that no such
 * change fell between the first check and the end of the read, so the bytes
 * are one state of the memory. A declined request leaves the staging buffer
 * unchanged and takes the boundary path. */
/* Defined with the recorded draws of the prompt service. */
static int cpu_surface_snapshot_clear(const VmReadSnapshotRange *ranges,u32 count,u64 *seen);
static int cpu_surface_snapshot_recheck(const VmReadSnapshotRange *ranges,u32 count,const u64 *seen);
static int s_cpu_surface_snapshot_fast;   /* RSX_CPU_SURFACE_SNAPSHOT_FAST=1 */
static _Atomic u64 s_cpu_surface_snapshot_served, s_cpu_surface_snapshot_queued;
/* Returns 1 when it served the request and staging holds the bytes. Returns 0
 * when the request takes the boundary path. */
static int cpu_surface_snapshot_fast(const VmReadSnapshotRange *ranges,u32 count,void *staging,u32 output_bytes,u32 client_thread) {
    extern uint8_t *vm_base;
    if(!s_cpu_surface_snapshot_fast)return 0;
    if(!ranges || !staging || !count || count>VM_READ_SNAPSHOT_MAX_RANGES || !output_bytes || output_bytes>VM_READ_SNAPSHOT_MAX_BYTES)return 0;
    for(u32 i=0;i<count;i++)
        if(!ranges[i].bytes || (u64)ranges[i].address+ranges[i].bytes>0x100000000ull || (u64)ranges[i].output_offset+ranges[i].bytes>output_bytes)return 0;
    unsigned char bytes[VM_READ_SNAPSHOT_MAX_BYTES];
    u64 seen[CPU_SURFACE_SNAPSHOT_SEEN];
    if(!cpu_surface_snapshot_clear(ranges,count,seen))goto queued;
    memcpy(bytes,staging,output_bytes);
    for(u32 i=0;i<count;i++) {
        SIZE_T copied=0;
        if(!ReadProcessMemory(GetCurrentProcess(),vm_base+ranges[i].address,bytes+ranges[i].output_offset,ranges[i].bytes,&copied) || copied!=ranges[i].bytes)goto queued;
    }
    if(!cpu_surface_snapshot_recheck(ranges,count,seen))goto queued;
    memcpy(staging,bytes,output_bytes);
    CpuSurfaceRequest fast={0};fast.address=ranges[0].address;fast.bytes=output_bytes;fast.client_thread=client_thread;
    fast.profile.queued=fast.profile.started=fast.profile.finished=fast.profile.returned=cpu_surface_tick();
    fast.profile.matched=fast.profile.hits=seen[CPU_SURFACE_SNAPSHOT_MATCHED];cpu_surface_profile_emit(&fast);
    u64 served=cpu_surface_view_increase(&s_cpu_surface_snapshot_served);
    if(!(served&4095))fprintf(stderr,"[CPUSURFACE_SNAPSHOT_FAST] served=%llu queued=%llu\n",(unsigned long long)served,(unsigned long long)cpu_surface_view_load(&s_cpu_surface_snapshot_queued));
    return 1;
queued:
    cpu_surface_view_increase(&s_cpu_surface_snapshot_queued);
    return 0;
}
static int cpu_surface_snapshot_request(const VmReadSnapshotRange *ranges,u32 count,
    void *staging,u32 output_bytes,void *opaque) {
    (void)opaque;
    if((LONG)GetCurrentThreadId()==InterlockedCompareExchange(&s_cpu_surface_thread,0,0))
        return VM_READ_SNAPSHOT_UNAVAILABLE;
    if(cpu_surface_snapshot_fast(ranges,count,staging,output_bytes,(u32)GetCurrentThreadId()))return VM_READ_SNAPSHOT_OK;
    CpuSurfaceRequest request={0};request.address=ranges&&count?ranges[0].address:0;request.bytes=output_bytes;
    request.snapshot_ranges=ranges;request.snapshot_count=count;request.snapshot_staging=staging;
    request.snapshot_output_bytes=output_bytes;request.snapshot_result=VM_READ_SNAPSHOT_FAILED;
    request.client_thread=GetCurrentThreadId();
    request.status=-1;request.queued_ms=GetTickCount64();request.profile.queued=cpu_surface_tick();
    request.done=CreateEventA(NULL,FALSE,FALSE,NULL);if(!request.done)cpu_surface_fail("CPU snapshot event creation",E_FAIL);
    AcquireSRWLockExclusive(&s_cpu_surface_lock);
    if(!s_cpu_surface_live){ReleaseSRWLockExclusive(&s_cpu_surface_lock);CloseHandle(request.done);return VM_READ_SNAPSHOT_UNAVAILABLE;}
    if(s_cpu_surface_tail)s_cpu_surface_tail->next=&request;else s_cpu_surface_head=&request;
    s_cpu_surface_tail=&request;ReleaseSRWLockExclusive(&s_cpu_surface_lock);
    extern void *cellGcm_fifo_kick_event(void);HANDLE kick=(HANDLE)cellGcm_fifo_kick_event();
    if(kick&&!SetEvent(kick))cpu_surface_fail("renderer snapshot kick",E_FAIL);
    DWORD wait=WaitForSingleObject(request.done,30000);
    if(wait!=WAIT_OBJECT_0 || request.status)cpu_surface_fail("CPU snapshot synchronization",E_FAIL);
    request.profile.returned=cpu_surface_tick();cpu_surface_profile_emit(&request);CloseHandle(request.done);
    return request.snapshot_result;
}

/* Eager submission after writes to listed surfaces. RSX_CPU_SURFACE_EAGER
 * lists up to four RSX surface offsets (1-8 hex digits, comma separated);
 * none by default. When a recorded draw or clear writes a listed surface, the
 * renderer submits the batch at the first later draw boundary whose draw no
 * longer targets a listed surface, or at its idle pump, through the same
 * nonpresenting capacity path as the early submission for pending reads. The
 * GPU then starts on that write before the CPU asks for it, and the prefetch
 * refresh at that batch's end can copy the new generation. Reads stay
 * coherent: each still needs the surface's current generation; only the
 * submission point moves earlier. In MotorStorm races the main thread reads
 * the HDR exposure target (02A30480) 10-20 ms after the renderer records its
 * write; the launcher's --eager-surface-submit 1 lists it. */
#define CPU_SURFACE_EAGER_MAX 4
static u32 s_cpu_surface_eager[CPU_SURFACE_EAGER_MAX];
static unsigned s_cpu_surface_eager_count;
static int s_cpu_surface_eager_dirty;   /* renderer-owned: a listed surface has unsubmitted writes */
static unsigned long long s_cpu_surface_eager_submits[2];
static void cpu_surface_eager_configure(const char *list) {
    s_cpu_surface_eager_count=0;s_cpu_surface_eager_dirty=0;
    if(!list || !*list)return;
    for(;;) {
        u64 off=0;unsigned digits=0;int d;
        while((d=cpu_surface_hex_digit(*list))>=0){off=off*16u+(u64)d;list++;
            if(++digits>8)cpu_surface_fail("RSX_CPU_SURFACE_EAGER (offsets are 1-8 hex digits)",E_INVALIDARG);}
        if(!digits || (*list && *list!=',') || s_cpu_surface_eager_count==CPU_SURFACE_EAGER_MAX)
            cpu_surface_fail("RSX_CPU_SURFACE_EAGER (up to 4 comma-separated hex offsets)",E_INVALIDARG);
        for(unsigned i=0;i<s_cpu_surface_eager_count;i++)
            if(s_cpu_surface_eager[i]==(u32)off)cpu_surface_fail("RSX_CPU_SURFACE_EAGER (duplicate offset)",E_INVALIDARG);
        s_cpu_surface_eager[s_cpu_surface_eager_count++]=(u32)off;
        fprintf(stderr,"[CPUSURFACE_EAGER] enabled offset=%08X\n",(unsigned)off);
        if(!*list)return;
        list++;
    }
}
static int cpu_surface_eager_listed(u32 offset) {
    for(unsigned i=0;i<s_cpu_surface_eager_count;i++)if(s_cpu_surface_eager[i]==offset)return 1;
    return 0;
}
/* A recorded draw or clear wrote the colour target at RSX offset `offset`,
 * bound at guest address `address` (0 when unbound). */
static void cpu_surface_eager_note(u32 offset,u32 address) {
    if(address && address!=0xffffffffu && cpu_surface_eager_listed(offset))s_cpu_surface_eager_dirty=1;
}
/* Colour targets of the draw about to be recorded, chosen as
 * the backend chooses active targets: does it still write a listed surface? */
static int cpu_surface_eager_state_writes(const rsx_state *st) {
    if(!st)return 0;
    unsigned targets = st->color_target >= 0x1F ? 4 :
                       st->color_target >= 0x17 ? 3 :
                       st->color_target >= 0x13 ? 2 : 1;
    for(unsigned i=0;i<targets;i++) {
        unsigned source=(!i&&st->color_target==2)?1:i;
        u32 dma=st->surface_color_dma[source];
        if(dma!=CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER && dma!=CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER)continue;
        if(cpu_surface_eager_listed(st->surface_color_offset[source]))return 1;
    }
    return 0;
}
/* Renderer service thread only, outside render_frame; renderer state is read
 * only after both checks. At a draw boundary the submission waits while the
 * draw about to be recorded still writes a listed surface. */
static int cpu_surface_eager_wanted(int at_pump) {
    if(!s_cpu_surface_eager_count)return 0;
    if(InterlockedCompareExchange(&s_cpu_surface_thread,0,0)!=(LONG)GetCurrentThreadId())return 0;
    if(!s_cpu_surface_eager_dirty || s_cpu_surface_recording)return 0;
    if(!at_pump && cpu_surface_eager_state_writes(s_d3d.current_rsx_state))return 0;
    if(s_cpu_surface_profile && !(++s_cpu_surface_eager_submits[at_pump?1:0]&255))
        fprintf(stderr,"[CPUSURFACE_EAGER] draw=%llu pump=%llu\n",s_cpu_surface_eager_submits[0],s_cpu_surface_eager_submits[1]);
    return 1;
}
/* render_frame submitted every recorded draw. */
static void cpu_surface_eager_submitted(void) {
    s_cpu_surface_eager_dirty=0;
}
/* Prompt service between batches. A queued read is otherwise served at the
 * end of the renderer's next batch or by its idle pump, so it also waits out
 * whatever the renderer records and replays meanwhile (in MotorStorm races
 * ~37 ms per read of a surface whose last write was submitted long before).
 * While the renderer records, the GPU surfaces hold exactly the submitted
 * batches; the only pending changes are the draws recorded since. A read that
 * overlaps none of their colour targets sees the same bytes now as it would
 * at the batch end, so the renderer serves it at its next recorded draw.
 * Reads overlapping a pending write wait for the batch as before. Never runs
 * inside render_frame (states and generations move there).
 * RSX_CPU_SURFACE_PROMPT=0 keeps batch-end/idle service only. */
#define CPU_SURFACE_PENDING_MAX 64
typedef struct CpuSurfaceRange { u64 begin, end; } CpuSurfaceRange;
static CpuSurfaceRange s_cpu_surface_pending[CPU_SURFACE_PENDING_MAX];
static unsigned s_cpu_surface_pending_count;
static int s_cpu_surface_pending_overflow;
/* The recorded draws of the current batch, visible to client threads. The
 * renderer thread is the only writer. It changes the list only between two
 * steps of this sequence number, so a client that reads the same even number
 * before and after the list saw one state of the list. */
static _Atomic u64 s_cpu_surface_pending_sequence;
static int s_cpu_surface_prompt;
static void cpu_surface_pending_insert(u32 address,u32 pitch,u32 height) {
    if(!address || address==0xffffffffu || !pitch)return;
    if(!height){s_cpu_surface_pending_overflow=1;return;}   /* unknown extent: serve nothing early */
    u64 begin=address,end=begin+(u64)pitch*height;
    for(unsigned i=0;i<s_cpu_surface_pending_count;i++)
        if(s_cpu_surface_pending[i].begin<=begin && end<=s_cpu_surface_pending[i].end)return;
    if(s_cpu_surface_pending_count==CPU_SURFACE_PENDING_MAX){s_cpu_surface_pending_overflow=1;return;}
    s_cpu_surface_pending[s_cpu_surface_pending_count].begin=begin;
    s_cpu_surface_pending[s_cpu_surface_pending_count++].end=end;
}
/* Renderer thread. Each recorded draw or clear registers its targets here. */
static void cpu_surface_pending_add(u32 address,u32 pitch,u32 height) {
    if(s_cpu_surface_deform.serve&&s_cpu_surface_deform.serve(address,(u32)((u64)pitch*height)))return;
    cpu_surface_view_store(&s_cpu_surface_pending_sequence,cpu_surface_view_load(&s_cpu_surface_pending_sequence)+1);
    cpu_surface_pending_insert(address,pitch,height);
    cpu_surface_view_store(&s_cpu_surface_pending_sequence,cpu_surface_view_load(&s_cpu_surface_pending_sequence)+1);
}
static int cpu_surface_pending_overlaps(u32 address,u32 bytes) {
    if(s_cpu_surface_pending_overflow)return 1;
    for(unsigned i=0;i<s_cpu_surface_pending_count;i++)
        if((u64)address<s_cpu_surface_pending[i].end && (u64)address+bytes>s_cpu_surface_pending[i].begin)return 1;
    return 0;
}
/* Client thread. 1 when the renderer holds nothing for the bytes of the
 * ranges: no recorded draw targets a range, and the one surface that meets a
 * range is current and holds the whole range. seen gets the view table, the
 * sum of the sequence numbers of its entries, the sequence number of the
 * recorded draws, and the number of surfaces that hold a range. */
static int cpu_surface_snapshot_clear(const VmReadSnapshotRange *ranges,u32 count,u64 *seen) {
    u64 table=cpu_surface_view_load(&s_cpu_surface_views),sequences=0,matched=0;
    if(s_cpu_surface_deform.sequence)sequences+=s_cpu_surface_deform.sequence();
    CpuSurfaceView *v=(CpuSurfaceView *)(uintptr_t)(table&CPU_SURFACE_VIEW_ADDRESS);
    u32 owners[VM_READ_SNAPSHOT_MAX_RANGES]={0};
    for(u64 i=table>>CPU_SURFACE_VIEW_COUNT;i;i--,v++) {
        u64 sequence=cpu_surface_view_load(&v->sequence);
        sequences+=sequence;
        if(sequence&1)return 0;
        int holds=0;
        for(u32 j=0;j<count;j++) {
            u64 begin=cpu_surface_view_load(&v->begin),end=cpu_surface_view_load(&v->end);
            u64 first=ranges[j].address,last=first+ranges[j].bytes;
            if(first>=end || last<=begin)continue;
            /* The range meets this surface. The surface must be current and own every byte of the range. */
            if(!cpu_surface_view_load(&v->ready) || first<begin || last>end || owners[j]++)return 0;
            holds=1;
        }
        if(cpu_surface_view_load(&v->sequence)!=sequence)return 0;
        matched+=holds;
    }
    u64 pending=cpu_surface_view_load(&s_cpu_surface_pending_sequence);
    if(pending&1)return 0;
    for(u32 j=0;j<count;j++)if(cpu_surface_pending_overlaps(ranges[j].address,ranges[j].bytes))return 0;
    if(cpu_surface_view_load(&s_cpu_surface_pending_sequence)!=pending || cpu_surface_view_load(&s_cpu_surface_views)!=table)return 0;
    seen[CPU_SURFACE_SNAPSHOT_TABLE]=table;seen[CPU_SURFACE_SNAPSHOT_SEQUENCES]=sequences;seen[CPU_SURFACE_SNAPSHOT_PENDING]=pending;seen[CPU_SURFACE_SNAPSHOT_MATCHED]=matched;
    return 1;
}
/* Client thread. 1 when the renderer changed no view and no recorded draw
 * since cpu_surface_snapshot_clear wrote seen. */
static int cpu_surface_snapshot_recheck(const VmReadSnapshotRange *ranges,u32 count,const u64 *seen) {
    u64 now[CPU_SURFACE_SNAPSHOT_SEEN];
    return cpu_surface_snapshot_clear(ranges,count,now) && now[CPU_SURFACE_SNAPSHOT_TABLE]==seen[CPU_SURFACE_SNAPSHOT_TABLE] &&
        now[CPU_SURFACE_SNAPSHOT_SEQUENCES]==seen[CPU_SURFACE_SNAPSHOT_SEQUENCES] && now[CPU_SURFACE_SNAPSHOT_PENDING]==seen[CPU_SURFACE_SNAPSHOT_PENDING];
}
/* render_frame submitted every recorded draw. */
static void cpu_surface_batch_submitted(void) {
    cpu_surface_view_store(&s_cpu_surface_pending_sequence,cpu_surface_view_load(&s_cpu_surface_pending_sequence)+1);
    s_cpu_surface_pending_count=0;s_cpu_surface_pending_overflow=0;
    cpu_surface_view_store(&s_cpu_surface_pending_sequence,cpu_surface_view_load(&s_cpu_surface_pending_sequence)+1);
}
static void cpu_surface_service_ready(void) {
    if(!s_cpu_surface_prompt)return;
    if(!atomic_load_explicit(&s_cpu_surface_head,memory_order_acquire))return;
    if(InterlockedCompareExchange(&s_cpu_surface_thread,0,0)!=(LONG)GetCurrentThreadId())return;
    if(s_cpu_surface_recording)return;
    CpuSurfaceRequest *ready=NULL,**ready_tail=&ready,*kept=NULL;
    AcquireSRWLockExclusive(&s_cpu_surface_lock);
    CpuSurfaceRequest *pending=s_cpu_surface_head;
    for(CpuSurfaceRequest **link=&pending;*link;) {
        CpuSurfaceRequest *request=*link;
        /* Snapshot requests retain their owned FIFO-boundary barrier. */
        if(request->snapshot_ranges){kept=s_cpu_surface_tail;break;}
        if(cpu_surface_pending_overlaps(request->address,request->bytes)){kept=request;link=&request->next;continue;}
        *link=request->next;request->next=NULL;*ready_tail=request;ready_tail=&request->next;
    }
    atomic_store_explicit(&s_cpu_surface_head,pending,memory_order_release);
    s_cpu_surface_tail=kept;
    ReleaseSRWLockExclusive(&s_cpu_surface_lock);
    while(ready) {
        CpuSurfaceRequest *next=ready->next;
        ready->profile.started=cpu_surface_tick();
        s_cpu_surface_profile_active=s_cpu_surface_profile?&ready->profile:NULL;
        cpu_surface_read(ready->address,ready->bytes);ready->status=0;
        s_cpu_surface_profile_active=NULL;
        ready->profile.finished=cpu_surface_tick();
        if(!SetEvent(ready->done))cpu_surface_fail("CPU completion event",E_FAIL);
        ready=next;
    }
}
/* Early submission for reads that need a recorded write. A queued read that
 * overlaps a pending colour target would otherwise wait while the renderer
 * records the rest of the frame (~26 ms per MotorStorm deformation-depth
 * read). Instead the renderer ends the batch at its next draw boundary, or at
 * its idle pump, through the capacity-drain path; the read is then served at
 * that batch's completion, after the write, with the bytes a coherent read
 * gets anyway. RSX_CPU_SURFACE_FLUSH=0 keeps the frame-end wait. */
static int s_cpu_surface_flush;
static unsigned long long s_cpu_surface_flushes[2];
static int cpu_surface_flush_wanted(int at_pump) {
    if(!s_cpu_surface_flush)return 0;
    if(!atomic_load_explicit(&s_cpu_surface_head,memory_order_acquire))return 0;   /* unlocked hint, as in service_ready */
    if(InterlockedCompareExchange(&s_cpu_surface_thread,0,0)!=(LONG)GetCurrentThreadId())return 0;
    if(s_cpu_surface_recording)return 0;
    int wanted=0;
    AcquireSRWLockExclusive(&s_cpu_surface_lock);
    for(CpuSurfaceRequest *request=s_cpu_surface_head;request && !wanted;request=request->next) {
        /* The snapshot and its suffix wait for their owned FIFO boundary. */
        if(request->snapshot_ranges)break;
        wanted=cpu_surface_pending_overlaps(request->address,request->bytes);
    }
    ReleaseSRWLockExclusive(&s_cpu_surface_lock);
    if(wanted && s_cpu_surface_profile && !(++s_cpu_surface_flushes[at_pump?1:0]&63))
        fprintf(stderr,"[CPUSURFACE_FLUSH] draw=%llu pump=%llu\n",s_cpu_surface_flushes[0],s_cpu_surface_flushes[1]);
    return wanted;
}
static void cpu_surface_init(void) {
    s_cpu_surface_thread=0;
    const char *profile=getenv("RSX_CPU_SURFACE_PROFILE");
    s_cpu_surface_profile=profile&&profile[0]=='1'&&!profile[1];
    const char *prompt=getenv("RSX_CPU_SURFACE_PROMPT");
    s_cpu_surface_prompt=!(prompt&&prompt[0]=='0'&&!prompt[1]);
    const char *flush=getenv("RSX_CPU_SURFACE_FLUSH");
    s_cpu_surface_flush=!(flush&&flush[0]=='0'&&!flush[1]);
    const char *fast=getenv("RSX_CPU_SURFACE_FAST");s_cpu_surface_fast=!(fast&&fast[0]=='0'&&!fast[1]);
    const char *snapshot_fast=getenv("RSX_CPU_SURFACE_SNAPSHOT_FAST");s_cpu_surface_snapshot_fast=snapshot_fast&&snapshot_fast[0]=='1'&&!snapshot_fast[1];
    cpu_surface_batch_submitted();
    cpu_surface_eager_configure(getenv("RSX_CPU_SURFACE_EAGER"));
    if(s_cpu_surface_profile) {
        LARGE_INTEGER f;if(!QueryPerformanceFrequency(&f)||f.QuadPart<=0)cpu_surface_fail("profile frequency",E_FAIL);
        s_cpu_surface_profile_frequency=(u64)f.QuadPart;
    }
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommandAllocator(s_d3d.device,D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator,(void **)&s_cpu_surface_alloc));
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommandList(s_d3d.device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_cpu_surface_alloc,NULL,&IID_ID3D12GraphicsCommandList,(void **)&s_cpu_surface_list));
    CPU_SURFACE_HR(s_cpu_surface_list->lpVtbl->Close(s_cpu_surface_list));
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void **)&s_cpu_surface_fence));
    s_cpu_surface_event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!s_cpu_surface_event)cpu_surface_fail("GPU event creation",E_FAIL);
    cpu_surface_prefetch_init();
    s_cpu_surface_live=1;vm_read_snapshot_set_handler(cpu_surface_snapshot_request,NULL);vm_read_sync_set_handler(cpu_surface_request,NULL);
}
static void cpu_surface_retire(OffRT *r) {
    cpu_surface_prefetch_retire(r);
    cpu_surface_copy(r);
    if(r->cpu_readback){r->cpu_readback->lpVtbl->Release(r->cpu_readback);r->cpu_readback=NULL;}
    r->cpu_address=r->cpu_pitch=0;r->gpu_generation=r->cpu_generation=0;cpu_surface_view_retire(r);
}
static void cpu_surface_shutdown(void) {
    vm_read_sync_set_handler(NULL,NULL);
    vm_read_snapshot_set_handler(NULL,NULL);
    cpu_surface_prefetch_shutdown();
    AcquireSRWLockExclusive(&s_cpu_surface_lock);
    for(int i=0;i<CPU_SURFACE_CAPACITY;i++)cpu_surface_retire(&s_d3d.off_rt[i]);
    s_cpu_surface_live=0;
    CpuSurfaceRequest *request=s_cpu_surface_head;s_cpu_surface_head=s_cpu_surface_tail=NULL;
    ReleaseSRWLockExclusive(&s_cpu_surface_lock);
    while(request){CpuSurfaceRequest *next=request->next;request->status=0;request->snapshot_result=VM_READ_SNAPSHOT_UNAVAILABLE;request->profile.started=request->profile.finished=cpu_surface_tick();if(!SetEvent(request->done))cpu_surface_fail("shutdown CPU completion",E_FAIL);request=next;}
    if(s_cpu_surface_list)s_cpu_surface_list->lpVtbl->Release(s_cpu_surface_list);
    if(s_cpu_surface_alloc)s_cpu_surface_alloc->lpVtbl->Release(s_cpu_surface_alloc);
    if(s_cpu_surface_fence)s_cpu_surface_fence->lpVtbl->Release(s_cpu_surface_fence);
    if(s_cpu_surface_event)CloseHandle(s_cpu_surface_event);
}
static void cpu_surface_capture(D3D12DrawRecord *draw) {
    const rsx_state *st=s_d3d.current_rsx_state;
    for(unsigned i=0;i<4;i++) {
        draw->cpu_address[i]=draw->cpu_pitch[i]=0;
        if(!st)continue;
        unsigned source=(!i&&st->color_target==2)?1:i;
        u32 dma=st->surface_color_dma[source];
        if(dma!=CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER && dma!=CELL_GCM_CONTEXT_DMA_MEMORY_HOST_BUFFER)continue;
        extern u32 cellGcmResolveLocated(int local,u32 offset);
        draw->cpu_address[i]=cellGcmResolveLocated(dma==CELL_GCM_CONTEXT_DMA_MEMORY_FRAME_BUFFER,st->surface_color_offset[source]);
        draw->cpu_pitch[i]=st->surface_color_pitch[source];
        u32 recorded_offset = i ? draw->rt_mrt[i - 1] : draw->rt_off;
        if (recorded_offset && recorded_offset == st->surface_color_offset[source] && draw->cpu_pitch[i])
            cpu_surface_eager_note(recorded_offset,draw->cpu_address[i]);
    }
    /* Every recorded draw/clear passes here; its targets are pending until
     * the batch is submitted. Then serve the reads none of them touch. */
    for(unsigned i=0;i<4;i++)cpu_surface_pending_add(draw->cpu_address[i],draw->cpu_pitch[i],draw->rt_h);
    cpu_surface_service_ready();
}
static void cpu_surface_bind(int index,const D3D12DrawRecord *draw,unsigned target) {
    if(index<0)return;OffRT *r=&s_d3d.off_rt[index];
    u32 address=draw->cpu_address[target],pitch=draw->cpu_pitch[target];
    if(address==0xffffffffu || !pitch)return;
    if(r->cpu_address && address!=r->cpu_address)cpu_surface_fail("surface memory identity changed at one offset",E_INVALIDARG);
    if(r->cpu_pitch!=pitch)r->cpu_generation=0;
    r->cpu_address=address;r->cpu_pitch=pitch;r->cpu_format=draw->rt_fmt;
    if(address && vm_read_sync_mark_range(address,(u64)pitch*r->h))cpu_surface_fail("guest surface registration",E_INVALIDARG);
    cpu_surface_view_update(r);
}
static void cpu_surface_written(int primary,const int secondary[3]) {
    if(primary>=0){s_d3d.off_rt[primary].gpu_generation++;cpu_surface_view_update(&s_d3d.off_rt[primary]);}
    for(int i=0;i<3;i++)if(secondary[i]>=0){s_d3d.off_rt[secondary[i]].gpu_generation++;cpu_surface_view_update(&s_d3d.off_rt[secondary[i]]);}
}
#undef CPU_SURFACE_HR
