/* Authored queue/clock model; production request/service functions are injected. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rsx_cpu_surface_profile.h"
typedef uint32_t u32;typedef uint64_t u64;typedef long LONG;typedef unsigned long DWORD;
typedef int SRWLOCK;typedef struct Event {int signaled;} *HANDLE;
typedef struct LARGE_INTEGER {int64_t QuadPart;} LARGE_INTEGER;
#define SRWLOCK_INIT 0
#define WAIT_OBJECT_0 0
#define WAIT_FAILED 0xffffffffu
#define FALSE 0
#define E_FAIL (-1)
static u64 clock_value=100;static unsigned clock_calls,read_calls;static LONG current_thread=1;
static int s_cpu_surface_recording;static int clock_ok=1;static u32 gpu_pixel,cpu_pixel;static unsigned submitted_batches;static struct Event kick_event;
static LONG GetCurrentThreadId(void){return current_thread;}
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG expected){LONG old=*p;if(old==expected)*p=value;return old;}
static void AcquireSRWLockExclusive(SRWLOCK *p){(void)p;}
static void ReleaseSRWLockExclusive(SRWLOCK *p){(void)p;}
static int QueryPerformanceCounter(LARGE_INTEGER *p){clock_calls++;p->QuadPart=(int64_t)clock_value++;return clock_ok;}
static u64 GetTickCount64(void){return clock_value/1000;}
static DWORD GetLastError(void){return 55;}
static HANDLE CreateEventA(void *a,int b,int c,const char *d){(void)a;(void)b;(void)c;(void)d;clock_value+=2;return calloc(1,sizeof(struct Event));}
static int SetEvent(HANDLE h){if(h==&kick_event)clock_value+=3;else h->signaled=1;return 1;}
static void CloseHandle(HANDLE h){free(h);}
static void *cellGcm_fifo_kick_event(void){return &kick_event;}
static void cpu_surface_service(void);
static void cpu_surface_frame_complete(void);
static DWORD WaitForSingleObject(HANDLE h,DWORD deadline){
    if(deadline!=30000)abort();
    clock_value+=40;current_thread=2;
    /* Authored GPU submission and retirement; production completion must
     * copy this published pixel and release the waiter without a pump. */
    gpu_pixel=0xBEEF;s_cpu_surface_recording=0;submitted_batches++;
    cpu_surface_frame_complete();
    if(!h->signaled){fprintf(stderr,"FAIL: queued request not released at completed batch\n");exit(2);}
    current_thread=1;clock_value+=5;
    return h->signaled?WAIT_OBJECT_0:WAIT_FAILED;
}
static void cpu_surface_fail(const char *operation,int result){fprintf(stderr,"FAIL: %s %d\n",operation,result);exit(2);}
/* PRODUCTION_DECLARATIONS */
static void cpu_surface_snapshot_trace(CpuSurfaceRequest *request){(void)request;cpu_surface_fail("unexpected snapshot trace in scalar fixture",E_FAIL);}
/* PRODUCTION_HELPERS */
static void cpu_surface_read(u32 address,u32 bytes){
    if(address!=0x20120 || bytes!=2)abort();
    if(!submitted_batches || s_cpu_surface_recording)abort();
    cpu_pixel=gpu_pixel;read_calls++;
    if(s_cpu_surface_profile_active){
        RsxCpuSurfaceProfile *p=s_cpu_surface_profile_active;
        p->allocation=2;p->prior_fence=3;p->commands=5;p->copy_fence=7;p->packing=11;
        p->matched=2;p->hits=1;p->copies=1;p->host_bytes=256;p->guest_bytes=8;
    }
    clock_value+=31;
}
/* PRODUCTION_SERVICE */
/* PRODUCTION_COMPLETION */
/* PRODUCTION_REQUEST */
#define REQUIRE(x,message) do{if(!(x)){fprintf(stderr,"FAIL: %s\n",message);return 1;}}while(0)
int main(int argc,char **argv){
    RsxCpuSurfaceProfile p={0};p.queued=10;p.started=20;p.finished=60;p.returned=70;
    p.allocation=2;p.prior_fence=3;p.commands=5;p.copy_fence=7;p.packing=11;p.matched=2;p.hits=1;p.copies=1;
    REQUIRE(rsx_cpu_surface_profile_valid(&p),"disjoint stages admitted");
    p.packing=100;REQUIRE(!rsx_cpu_surface_profile_valid(&p),"overlapping/over-accounted stages rejected");
    p.packing=11;p.started=9;REQUIRE(!rsx_cpu_surface_profile_valid(&p),"nonmonotonic queue rejected");
    p.started=20;p.copies=3;REQUIRE(!rsx_cpu_surface_profile_valid(&p),"invalid copy count rejected");
    s_cpu_surface_live=1;
    if(argc==2 && !strcmp(argv[1],"unsubmitted")){s_cpu_surface_recording=1;cpu_surface_frame_complete();return 1;}
    if(argc==2 && !strcmp(argv[1],"clock-failure")){s_cpu_surface_profile=1;clock_ok=0;cpu_surface_request(0x20120,2,NULL);return 1;}
    cpu_surface_request(0x20120,2,NULL);
    REQUIRE(cpu_pixel==0xBEEF && submitted_batches==1,"submitted pixel visible at completion");
    REQUIRE(read_calls==1 && clock_calls==0,"default-off has no profiling clocks and still services read");
    s_cpu_surface_profile=1;s_cpu_surface_profile_frequency=1000;
    cpu_surface_request(0x20120,2,NULL);
    REQUIRE(read_calls==2 && clock_calls==4,"queued request completes with exact timing boundaries");
    current_thread=2;cpu_surface_request(0x20120,2,NULL);
    REQUIRE(read_calls==3 && clock_calls==6,"direct renderer path completes and restores active record");
    REQUIRE(!s_cpu_surface_profile_active && !s_cpu_surface_head && !s_cpu_surface_tail,"request ownership cleared");
    puts("PASS: actual CPU surface request/service, default-off clocks, direct path and accounting");return 0;
}
