#include "rsx_vertex_fetch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#define CHECK(x,m) do { if (!(x)) { fprintf(stderr,"FAIL %s\n",m); exit(2); } } while (0)
u8* vm_base;
u64 test_fetch_calls;
static unsigned resolves;
static u64 resolve_trace;
static u8 memory[0x80000];
static rsx_state state;
static volatile u64 sink;
u32 cellGcmResolveOffset(u32 off) { (void)off; abort(); }
u32 cellGcmResolveLocated(int location,u32 off) {
#ifdef VERTEX_VERIFY_COUNTS
    resolves++; resolve_trace=resolve_trace*0x100000001b3ULL ^ ((u64)(unsigned)location<<32) ^ off;
#endif
    return (location ? 0x40000u : 0x10000u) +
        ((((off >> 12) ^ (off >> 16)) & 15u) << 12) + (off & 4095u);
}
static void setup(unsigned active,unsigned type) {
    memset(&state,0,sizeof state); vm_base=memory;
    for(unsigned i=0;i<sizeof memory;i++) memory[i]=(u8)(i*31u+(i>>8)*17u);
    for(unsigned i=0;i<16;i++) {
        for(unsigned k=0;k<4;k++) { u32 v=(i%4==0?0x7fc00000u:i%4==1?0x80000000u:0x3f000000u)+i*0x100u+k;
            memcpy(&state.vertex_data4f[i][k],&v,4); }
        rsx_vertex_attrib* a=&state.vertex_attribs[i];
        a->offset=(i&1u?0x80000000u:0u)+i*37u+4090u;
        a->type=type; a->size=(i%5u); a->stride=24u;
        a->frequency=i%4u; a->enabled=i<active;
    }
    state.frequency_divider_op=0x5555u;
}
static void original(u32 vi,rsx_vertex_value out[16]) {
    for(int i=0;i<16;i++)rsx_fetch_attrib(&state,i,vi,out[i].v);
}
static void parity(rsx_vertex_fetch_batch* b,u32 vi,const char* why) {
    rsx_vertex_value a[16],c[16]; unsigned ra,rc;u64 ta,tc;
    resolves=0;resolve_trace=0; original(vi,a); ra=resolves;ta=resolve_trace;
    resolves=0;resolve_trace=0; test_fetch_calls=0; rsx_fetch_vertex_prepared(b,vi,c);rc=resolves;tc=resolve_trace;
    CHECK(memcmp(a,c,sizeof a)==0,why); CHECK(ra==rc && ta==tc,"per-element address resolution changed");
#ifdef VERTEX_VERIFY_COUNTS
    /* Prepared draws decode types 1..7 inline and call the scalar fetch only
     * for other types; dense draws (>8 arrays) stay fully scalar. */
    u64 scalar=0;
    if(b->count>8)scalar=16;
    else if(vm_base)for(u32 i=0;i<b->count;i++){u32 t=state.vertex_attribs[b->active[i]].type;scalar+=!(t>=1&&t<=7);}
    CHECK(test_fetch_calls==scalar,"constant-only slots fetched individually");
#endif
}
static double now(void) {
#ifdef _WIN32
    LARGE_INTEGER a,b;CHECK(QueryPerformanceCounter(&a)&&QueryPerformanceFrequency(&b),"QPC");return(double)a.QuadPart/b.QuadPart;
#else
    struct timespec t;CHECK(clock_gettime(CLOCK_MONOTONIC,&t)==0,"clock");return t.tv_sec+t.tv_nsec*1e-9;
#endif
}
static double cpu_now(void) {
#ifdef _WIN32
    FILETIME c,e,k,u;CHECK(GetProcessTimes(GetCurrentProcess(),&c,&e,&k,&u),"CPU clock");ULARGE_INTEGER a,b;a.LowPart=k.dwLowDateTime;a.HighPart=k.dwHighDateTime;b.LowPart=u.dwLowDateTime;b.HighPart=u.dwHighDateTime;return(double)(a.QuadPart+b.QuadPart)*1e-7;
#else
    struct timespec t;CHECK(clock_gettime(CLOCK_PROCESS_CPUTIME_ID,&t)==0,"CPU clock");return t.tv_sec+t.tv_nsec*1e-9;
#endif
}
static u64 checksum(const rsx_vertex_value v[16],u32 vi) {
    u32 bits;memcpy(&bits,&v[(vi>>2)&15].v[vi&3],4);return bits^vi;
}
#ifdef _MSC_VER
#define NOINLINE __declspec(noinline)
#else
#define NOINLINE __attribute__((noinline))
#endif
static NOINLINE u64 old_draw(unsigned n) {
    rsx_vertex_value out[16];u64 sum=0;for(u32 i=0;i<n;i++){original(i,out);sum+=checksum(out,i);}return sum;
}
static NOINLINE u64 new_draw(unsigned n) {
    rsx_vertex_fetch_batch batch;rsx_vertex_fetch_prepare(&batch,&state);
    if(batch.count>8)return old_draw(n);
    rsx_vertex_value out[16];u64 sum=0;for(u32 i=0;i<n;i++){rsx_fetch_vertex_prepared(&batch,i,out);sum+=checksum(out,i);}return sum;
}
int main(int argc,char**argv) {
    if(argc==5) {
        unsigned active=(unsigned)strtoul(argv[2],NULL,10),n=(unsigned)strtoul(argv[3],NULL,10),draws=(unsigned)strtoul(argv[4],NULL,10);
        CHECK(active<=16 && n && n<=4096 && draws,"benchmark args");setup(active,2);
        u64 expected=old_draw(n);CHECK(new_draw(n)==expected,"benchmark full draw checksum");
        CHECK(!strcmp(argv[1],"original")||!strcmp(argv[1],"candidate"),"benchmark side");
        u64(*volatile fn)(unsigned)=!strcmp(argv[1],"original")?old_draw:new_draw;
        for(unsigned i=0;i<100;i++)sink+=fn(n);sink=0;
        double cpu=cpu_now(),start=now();for(unsigned i=0;i<draws;i++)sink+=fn(n);double elapsed=now()-start;cpu=cpu_now()-cpu;
        CHECK(sink==expected*draws,"timed real work checksum");
        printf("{\"side\":\"%s\",\"active\":%u,\"vertices\":%u,\"draws\":%u,\"checksum\":%llu,\"seconds\":%.9f,\"cpu_seconds\":%.9f}\n",argv[1],active,n,draws,(unsigned long long)sink,elapsed,cpu);return 0;
    }
    rsx_vertex_fetch_batch b;setup(4,7);rsx_vertex_fetch_prepare(&b,&state);
    parity(&b,0,"initial defaults and arrays");
    memory[cellGcmResolveLocated(1,state.vertex_attribs[0].offset)]^=1;
    parity(&b,0,"live vertex edits lost");
    const u32 indices[]={0,1,17,257,0xfffffffeu,0xffffffffu};
    unsigned cases=0;
    const u32 types[] = {0,1,2,3,4,5,6,7,8,0x101,0x102,0x103,0x104,0x105,0x106,0x107,0xffffff04u};
    for(unsigned ti=0;ti<sizeof types/sizeof types[0];ti++)for(unsigned active=0;active<=16;active++) {
        const u32 type = types[ti];
        setup(active,type);
        for(unsigned i=0;i<16;i++)if(i%7==0)state.vertex_attribs[i].stride=0;
        rsx_vertex_fetch_prepare(&b,&state);
        for(unsigned j=0;j<6;j++){parity(&b,indices[j],"exact scalar format/default/index parity");cases++;}
        vm_base=NULL;parity(&b,17,"missing VM constant parity");vm_base=memory;
        state.vertex_data4f[15][2]=-0.0f;state.vertex_attribs[15].enabled=1;
        rsx_vertex_fetch_prepare(&b,&state);parity(&b,3,"next draw refreshed declarations/constants");
    }
    printf("PASS actual prepared/scalar vertex fetch: %u cases; live bytes/mapping/frequencies/defaults\n",cases);return 0;
}
