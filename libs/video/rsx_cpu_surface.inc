/* Render-thread service for CPU loads from host-rendered guest surfaces. */
#include "ps3emu/guest_read_sync.h"
#include "rsx_surface_readback.h"
#include "rsx_cpu_surface_profile.h"
#include "rsx_cpu_surface_snapshot.h"
#include "rsx_fifo_boundary.h"
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
static CpuSurfaceRequest *s_cpu_surface_head, *s_cpu_surface_tail;
static volatile LONG s_cpu_surface_thread;
static ID3D12CommandAllocator *s_cpu_surface_alloc;
static ID3D12GraphicsCommandList *s_cpu_surface_list;
static ID3D12Fence *s_cpu_surface_fence;
static HANDLE s_cpu_surface_event;
static u64 s_cpu_surface_fence_value;
static int s_cpu_surface_live;
static int s_cpu_surface_recording;
static void cpu_surface_fail(const char *operation, HRESULT result) {
    fprintf(stderr,"[CPUSURFACE] %s failed: 0x%08lX\n",operation,(unsigned long)result);rsx_gpu_fault_report(s_d3d.device);abort();
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

static void cpu_surface_copy(OffRT *r) {
    extern uint8_t *vm_base;
    if(!r->res || !r->cpu_address || !r->cpu_pitch || !r->gpu_generation || r->cpu_generation==r->gpu_generation){if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->hits++;return;}
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
    r->cpu_generation=r->gpu_generation;
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
    for(int i=0;i<CPU_SURFACE_CAPACITY;i++) {
        OffRT *r=&s_d3d.off_rt[i];
        if(!r->res || !r->cpu_address)continue;
        u64 end=(u64)r->cpu_address+(u64)r->cpu_pitch*r->h;
        if((u64)address<end && (u64)address+bytes>r->cpu_address){
            if(s_cpu_surface_recording && r->gpu_generation && r->cpu_generation!=r->gpu_generation)cpu_surface_fail("CPU load requires pending recorded surface writes",E_FAIL);
            if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->matched++;
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
/* This runs only on the renderer owner.  A view is admitted only when every
 * requested byte is uniquely owned by a live pitch-backed allocation.  The
 * matching CPU copies establish texture visibility before staging from VM; no
 * unsynchronized VM load is used as a substitute for a missing surface. */
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
        views[view_count]=(RsxCpuSurfaceSnapshotView){r->cpu_address,r->cpu_pitch,r->h};
        targets[view_count++]=r;
    }
    RsxCpuSurfaceSnapshotBinding bindings[VM_READ_SNAPSHOT_MAX_RANGES];
    if(!rsx_cpu_surface_snapshot_bind(ranges,count,views,view_count,bindings))
        return VM_READ_SNAPSHOT_UNAVAILABLE;
    for(u32 i=0;i<count;i++) {
        OffRT *r=targets[bindings[i].view_index];
        if(s_cpu_surface_recording && r->gpu_generation && r->cpu_generation!=r->gpu_generation)
            cpu_surface_fail("CPU snapshot requires pending recorded surface writes",E_FAIL);
    }
    for(u32 i=0;i<count;i++) {
        int seen=0;for(u32 j=0;j<i;j++)if(bindings[j].view_index==bindings[i].view_index){seen=1;break;}
        if(seen)continue;
        if(s_cpu_surface_profile_active)s_cpu_surface_profile_active->matched++;
        cpu_surface_copy(targets[bindings[i].view_index]);
    }
    for(u32 i=0;i<count;i++)
        memcpy((uint8_t *)staging+ranges[i].output_offset,vm_base+ranges[i].address,ranges[i].bytes);
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
static int cpu_surface_snapshot_request(const VmReadSnapshotRange *ranges,u32 count,
    void *staging,u32 output_bytes,void *opaque) {
    (void)opaque;
    if((LONG)GetCurrentThreadId()==InterlockedCompareExchange(&s_cpu_surface_thread,0,0))
        return VM_READ_SNAPSHOT_UNAVAILABLE;
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
static void cpu_surface_init(void) {
    s_cpu_surface_thread=0;
    const char *profile=getenv("RSX_CPU_SURFACE_PROFILE");
    s_cpu_surface_profile=profile&&profile[0]=='1'&&!profile[1];
    if(s_cpu_surface_profile) {
        LARGE_INTEGER f;if(!QueryPerformanceFrequency(&f)||f.QuadPart<=0)cpu_surface_fail("profile frequency",E_FAIL);
        s_cpu_surface_profile_frequency=(u64)f.QuadPart;
    }
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommandAllocator(s_d3d.device,D3D12_COMMAND_LIST_TYPE_DIRECT,
        &IID_ID3D12CommandAllocator,(void **)&s_cpu_surface_alloc));
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateCommandList(s_d3d.device,0,D3D12_COMMAND_LIST_TYPE_DIRECT,
        s_cpu_surface_alloc,NULL,&IID_ID3D12GraphicsCommandList,(void **)&s_cpu_surface_list));
    rsx_gpu_fault_name((ID3D12Object*)s_cpu_surface_list,L"RSX CPU surface copy");
    CPU_SURFACE_HR(s_cpu_surface_list->lpVtbl->Close(s_cpu_surface_list));
    CPU_SURFACE_HR(s_d3d.device->lpVtbl->CreateFence(s_d3d.device,0,D3D12_FENCE_FLAG_NONE,&IID_ID3D12Fence,(void **)&s_cpu_surface_fence));
    s_cpu_surface_event=CreateEventA(NULL,FALSE,FALSE,NULL);if(!s_cpu_surface_event)cpu_surface_fail("GPU event creation",E_FAIL);
    s_cpu_surface_live=1;vm_read_snapshot_set_handler(cpu_surface_snapshot_request,NULL);vm_read_sync_set_handler(cpu_surface_request,NULL);
}
static void cpu_surface_retire(OffRT *r) {
    cpu_surface_copy(r);
    if(r->cpu_readback){r->cpu_readback->lpVtbl->Release(r->cpu_readback);r->cpu_readback=NULL;}
    r->cpu_address=r->cpu_pitch=0;r->gpu_generation=r->cpu_generation=0;
}
static void cpu_surface_shutdown(void) {
    vm_read_sync_set_handler(NULL,NULL);
    vm_read_snapshot_set_handler(NULL,NULL);
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
    }
}
static void cpu_surface_bind(int index,const D3D12DrawRecord *draw,unsigned target) {
    if(index<0)return;OffRT *r=&s_d3d.off_rt[index];
    u32 address=draw->cpu_address[target],pitch=draw->cpu_pitch[target];
    if(address==0xffffffffu || !pitch)return;
    if(r->cpu_address && address!=r->cpu_address)cpu_surface_fail("surface memory identity changed at one offset",E_INVALIDARG);
    if(r->cpu_pitch!=pitch)r->cpu_generation=0;
    r->cpu_address=address;r->cpu_pitch=pitch;r->cpu_format=draw->rt_fmt;
    if(address && vm_read_sync_mark_range(address,(u64)pitch*r->h))cpu_surface_fail("guest surface registration",E_INVALIDARG);
}
static void cpu_surface_written(int primary,const int secondary[3]) {
    if(primary>=0)s_d3d.off_rt[primary].gpu_generation++;
    for(int i=0;i<3;i++)if(secondary[i]>=0)s_d3d.off_rt[secondary[i]].gpu_generation++;
}
#undef CPU_SURFACE_HR
