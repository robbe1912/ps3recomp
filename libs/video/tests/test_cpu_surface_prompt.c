/* Authored renderer/queue model; the production prompt-service block is injected. */
#include <stdint.h>
#include <stdatomic.h>
#include "ps3emu/guest_read_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_cpu_surface_profile.h"
typedef uint32_t u32;typedef uint64_t u64;typedef long LONG;typedef unsigned long DWORD;
typedef int SRWLOCK;typedef struct Event {int signaled;} *HANDLE;
typedef struct LARGE_INTEGER {int64_t QuadPart;} LARGE_INTEGER;
#define SRWLOCK_INIT 0
#define FALSE 0
#define E_FAIL (-1)
static LONG current_thread=2;static int s_cpu_surface_recording;static unsigned locks_held,lock_calls,reads;
static u32 served[16];static unsigned served_count;
static LONG GetCurrentThreadId(void){return current_thread;}
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG expected){LONG old=*p;if(old==expected)*p=value;return old;}
static void AcquireSRWLockExclusive(SRWLOCK *p){(void)p;locks_held++;lock_calls++;}
static void ReleaseSRWLockExclusive(SRWLOCK *p){(void)p;locks_held--;}
static int QueryPerformanceCounter(LARGE_INTEGER *p){static int64_t t;p->QuadPart=++t;return 1;}
static int SetEvent(HANDLE h){h->signaled=1;return 1;}
static void cpu_surface_fail(const char *operation,int result){fprintf(stderr,"FAIL: %s %d\n",operation,result);exit(2);}
/* PRODUCTION_DECLARATIONS */
_Static_assert(_Generic(&s_cpu_surface_head, _Atomic(CpuSurfaceRequest*)*:1, default:0), "Publish queue head with C11 atomics");
/* PRODUCTION_HELPERS */
static void cpu_surface_read(u32 address,u32 bytes){
    (void)bytes;if(locks_held)cpu_surface_fail("surface copy under the request lock",0);
    if(s_cpu_surface_recording)cpu_surface_fail("surface copy during replay",0);
    reads++;if(served_count<16)served[served_count++]=address;
}
/* Instrument renderer-owned reads without changing their value. */
static int recording_for_owner(void) {
    if (GetCurrentThreadId() != InterlockedCompareExchange(&s_cpu_surface_thread, 0, 0))
        cpu_surface_fail("renderer state read by another thread", 0);
    return s_cpu_surface_recording;
}
#define s_cpu_surface_recording recording_for_owner()
/* PRODUCTION_PROMPT */
#undef s_cpu_surface_recording
static struct {
    int initialized;
    u32 draw_count;
} s_d3d = {1, 1};
static unsigned forced_submits;
static int initialized_for_owner(void) {
    if (GetCurrentThreadId() != InterlockedCompareExchange(&s_cpu_surface_thread, 0, 0))
        cpu_surface_fail("GPU state read by another thread", 0);
    return s_d3d.initialized;
}
static void submit_record_capacity(void* context, int present) {
    if (context || present || locks_held || s_cpu_surface_recording)
        cpu_surface_fail("invalid early submission", 0);
    ++forced_submits;
}
/* PRODUCTION_FLUSH */

#define REQUIRE(x,message) do{if(!(x)){fprintf(stderr,"FAIL: %s\n",message);exit(1);}}while(0)
static struct Event events[8];static CpuSurfaceRequest requests[8];
static void queue(unsigned i,u32 address,u32 bytes){
    CpuSurfaceRequest *r=&requests[i];memset(r,0,sizeof *r);memset(&events[i],0,sizeof events[i]);
    r->address=address;r->bytes=bytes;r->status=-1;r->done=&events[i];
    if(s_cpu_surface_tail)s_cpu_surface_tail->next=r;else s_cpu_surface_head=r;
    s_cpu_surface_tail=r;
}
static unsigned queued(void){unsigned n=0;for(CpuSurfaceRequest *r=s_cpu_surface_head;r;r=r->next)n++;return n;}
static void reset(void){
    s_cpu_surface_head=s_cpu_surface_tail=NULL;served_count=reads=0;s_cpu_surface_recording=0;
    current_thread=2;s_cpu_surface_thread=2;s_cpu_surface_prompt=1;s_cpu_surface_flush=1;cpu_surface_batch_submitted();
}
int main(void){
    reset();
    queue(0,0x20000,2);queue(1,0x21000,2);queue(2,0x22000,2);
    static VmReadSnapshotRange snapshot_range;
    requests[1].snapshot_ranges=&snapshot_range;
    cpu_surface_pending_add(0x22000,0x100,4);
    REQUIRE(!cpu_surface_flush_wanted(0),"snapshot barrier retains flush decisions for its queued suffix");
    cpu_surface_service_ready();
    REQUIRE(reads==1 && s_cpu_surface_head==&requests[1] && s_cpu_surface_tail==&requests[2] && !events[1].signaled && !events[2].signaled,"snapshot barrier retains queued suffix");

    /* 1. Only the read overlapping a recorded draw's target stays queued, in order. */
    reset();
    cpu_surface_pending_add(0x10000,0x100,4);              /* pending write [0x10000,0x10400) */
    queue(0,0x10200,2);queue(1,0x20000,2);queue(2,0x103FF,1);queue(3,0x0FFFE,3);queue(6,0x0FFFC,4);queue(4,0x10400,4);
    cpu_surface_service_ready();
    REQUIRE(served_count==3 && served[0]==0x20000 && served[1]==0x0FFFC && served[2]==0x10400,"non-overlapping (incl. adjacent) reads served at the next recorded draw, in order");
    REQUIRE(events[1].signaled && events[6].signaled && events[4].signaled && !events[0].signaled && !events[2].signaled && !events[3].signaled,"exactly the served waiters released");
    REQUIRE(requests[1].status==0 && requests[4].status==0 && requests[0].status==-1,"served status published");
    REQUIRE(queued()==3 && s_cpu_surface_head==&requests[0] && requests[0].next==&requests[2] && requests[2].next==&requests[3] && s_cpu_surface_tail==&requests[3],"overlapping reads keep their order and the tail");
    queue(5,0x30000,4);REQUIRE(requests[3].next==&requests[5],"queue append after a partial extraction");
    cpu_surface_service_ready();
    REQUIRE(served_count==4 && served[3]==0x30000 && queued()==3 && s_cpu_surface_tail==&requests[3],"tail restored after the last request leaves");
    /* 2. After the batch is submitted nothing is pending. */
    cpu_surface_batch_submitted();cpu_surface_service_ready();
    REQUIRE(queued()==0 && !s_cpu_surface_tail && served_count==7,"submitted batch releases every read");
    /* 3. Guards: replay, other threads, switch off, overflow, unknown extent, empty queue. */
    reset();queue(0,0x20000,2);s_cpu_surface_recording=1;cpu_surface_service_ready();
    REQUIRE(!reads && queued()==1,"no prompt service while render_frame replays");
    s_cpu_surface_recording=0;current_thread=7;cpu_surface_service_ready();
    REQUIRE(!reads && queued()==1,"only the renderer service thread serves");
    current_thread=2;s_cpu_surface_prompt=0;cpu_surface_service_ready();
    REQUIRE(!reads && queued()==1,"RSX_CPU_SURFACE_PROMPT=0 keeps batch-end service");
    s_cpu_surface_prompt=1;
    for(u32 i=0;i<CPU_SURFACE_PENDING_MAX;i++)cpu_surface_pending_add(0x40000000u+i*0x1000u,0x100,4);
    cpu_surface_service_ready();REQUIRE(reads==1,"full but exact pending set still serves disjoint reads");
    reset();queue(0,0x20000,2);
    for(u32 i=0;i<=CPU_SURFACE_PENDING_MAX;i++)cpu_surface_pending_add(0x40000000u+i*0x1000u,0x100,4);
    cpu_surface_service_ready();REQUIRE(!reads && queued()==1,"overflowed pending set serves nothing early");
    reset();queue(0,0x20000,2);cpu_surface_pending_add(0x50000,0x100,0);cpu_surface_service_ready();
    REQUIRE(!reads && queued()==1,"a target of unknown height serves nothing early");
    reset();cpu_surface_pending_add(0,0x100,4);cpu_surface_pending_add(0xffffffffu,0x100,4);cpu_surface_pending_add(0x60000,0,4);
    queue(0,0x60000,2);cpu_surface_service_ready();REQUIRE(reads==1,"unbound targets are not pending writes");
    reset();cpu_surface_pending_add(0x70000,0x100,8);cpu_surface_pending_add(0x70100,0x100,2);
    REQUIRE(s_cpu_surface_pending_count==1,"contained ranges are not duplicated");
    cpu_surface_pending_add(0x70000,0x100,9);REQUIRE(s_cpu_surface_pending_count==2,"a larger range is recorded");
    reset();lock_calls=0;cpu_surface_service_ready();REQUIRE(!reads && !lock_calls,"empty queue takes no lock");
    /* 4. Early submission: wanted exactly when a queued read needs a pending write. */
    reset();lock_calls=0;cpu_surface_pending_add(0x10000,0x100,4);
    REQUIRE(!cpu_surface_flush_wanted(0) && !lock_calls,"empty queue: no early submission, no lock");
    queue(0,0x20000,2);queue(1,0x0FFFE,2);
    REQUIRE(!cpu_surface_flush_wanted(0),"reads clear of (and adjacent to) pending writes need no submission");
    queue(2,0x103FE,2);
    REQUIRE(cpu_surface_flush_wanted(0) && cpu_surface_flush_wanted(1),"a read behind others that needs a pending write asks for submission");
    REQUIRE(!reads && queued()==3 && s_cpu_surface_tail==&requests[2] && !locks_held,"asking serves and dequeues nothing");
    cpu_surface_flush_recorded(0);
    REQUIRE(forced_submits==1 && !reads && queued()==3,"overlapping read submits at a draw boundary without serving under the queue lock");
    s_d3d.draw_count=0;
    cpu_surface_flush_recorded(1);
    REQUIRE(forced_submits==1,"empty recorded batch stays empty");
    s_d3d.draw_count=1;
    s_cpu_surface_recording=1;REQUIRE(!cpu_surface_flush_wanted(0),"never while render_frame replays");
    s_cpu_surface_recording=0;current_thread=7;REQUIRE(!cpu_surface_flush_wanted(1),"only the renderer service thread submits");
    cpu_surface_flush_recorded(1);
    REQUIRE(forced_submits==1,"foreign thread never submits or reads GPU state");
    current_thread=2;s_cpu_surface_flush=0;REQUIRE(!cpu_surface_flush_wanted(0),"RSX_CPU_SURFACE_FLUSH=0 keeps the frame-end wait");
    s_cpu_surface_flush=1;cpu_surface_batch_submitted();REQUIRE(!cpu_surface_flush_wanted(0),"nothing pending after submission");
    for(u32 i=0;i<=CPU_SURFACE_PENDING_MAX;i++)cpu_surface_pending_add(0x40000000u+i*0x1000u,0x100,4);
    REQUIRE(cpu_surface_flush_wanted(0),"overflowed pending set submits");
    puts("PASS: prompt CPU surface service: disjoint reads early, overlapping reads queued in order, submit reset, guards");
    return 0;
}
