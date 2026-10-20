/* Optional, read-only pre-execution snapshots at configured SPU PCs.
 * Private binary rings contain target data; never publish their contents. */
#ifndef PS3_SPU_BOUNDARY_TRACE_H
#define PS3_SPU_BOUNDARY_TRACE_H
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef __cplusplus
#include <atomic>
static std::atomic<int> sb_ready(0);
#define SB_READY() sb_ready.load(std::memory_order_acquire)
#define SB_PUBLISH() sb_ready.store(1, std::memory_order_release)
#else
#include <stdatomic.h>
static atomic_int sb_ready;
#define SB_READY() atomic_load_explicit(&sb_ready,memory_order_acquire)
#define SB_PUBLISH() atomic_store_explicit(&sb_ready,1,memory_order_release)
#endif
#ifdef _WIN32
#include <windows.h>
#include <share.h>
typedef SRWLOCK sb_mutex;
#define SB_MUTEX_INIT SRWLOCK_INIT
static void sb_lock(sb_mutex* m){AcquireSRWLockExclusive(m);}
static void sb_unlock(sb_mutex* m){ReleaseSRWLockExclusive(m);}
static void sb_mutex_init(sb_mutex* m){InitializeSRWLock(m);}
static uint64_t sb_tick(void){LARGE_INTEGER t;QueryPerformanceCounter(&t);return (uint64_t)t.QuadPart;}
static uint64_t sb_frequency(void){LARGE_INTEGER t;QueryPerformanceFrequency(&t);return (uint64_t)t.QuadPart;}
static unsigned sb_tid(void){return GetCurrentThreadId();}
static unsigned sb_pid(void){return GetCurrentProcessId();}
static FILE* sb_open(const char* p){return _fsopen(p,"w+bx",_SH_DENYWR);}
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
typedef pthread_mutex_t sb_mutex;
#define SB_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static void sb_lock(sb_mutex* m){pthread_mutex_lock(m);}
static void sb_unlock(sb_mutex* m){pthread_mutex_unlock(m);}
static void sb_mutex_init(sb_mutex* m){pthread_mutex_init(m,NULL);}
static uint64_t sb_tick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000000000u+t.tv_nsec;}
static uint64_t sb_frequency(void){return 1000000000u;}
/* Thread identity is an opaque token, never a physical SPU number. */
static unsigned sb_tid(void){return (unsigned)(uintptr_t)pthread_self();}
static unsigned sb_pid(void){return (unsigned)getpid();}
static FILE* sb_open(const char* p){return fopen(p,"w+bx");}
#endif
#define SPU_BOUNDARY_CONTEXT_V2 1
#define SB_CONTEXTS 32
#define SB_HEADER_BYTES 4096
#define SB_LS_BYTES 4096
typedef struct sb_record {
    uint64_t sequence,tick,context;
    uint32_t thread,pc,instruction,policy;
    uint8_t gpr[2048],ls[SB_LS_BYTES];
    uint64_t committed_sequence;
} sb_record;
typedef struct sb_context {
    const void* owner;
    FILE* file;
    uint64_t sequence;
    sb_mutex lock;
    int failed;
    unsigned context_bytes,record_bytes,commit_offset;
} sb_context;
static struct {
    int enabled,policy_only,full_reported;
    unsigned capacity,ls_start,ls_length,match_start,match_value;
    int match_enabled,first_pc_enabled,bad_context_reported;
    unsigned context_limit,first_pc;
    char arm_file[2048];
    char directory[2048],pcs_text[1024];
    uint8_t selected[65536];
    sb_context contexts[SB_CONTEXTS];
} sb_state;
static sb_mutex sb_registry_lock=SB_MUTEX_INIT;
static void sb_error(const char* message){fprintf(stderr,"[DEBUG-spu-boundary] ERROR %s\n",message);fflush(stderr);}
static int sb_number(const char* text,unsigned* out){
    char* end;unsigned long n;if(!text||!*text)return 0;
    errno=0;n=strtoul(text,&end,0);
    if(errno||*end||n>0xfffffffful)return 0;*out=(unsigned)n;return 1;
}
static void sb_initialize(void){
    const char *dir,*pcs,*cap,*start,*length,*policy;unsigned count=0;
    const uint32_t endian=0x01020304;
    sb_lock(&sb_registry_lock);
    if(SB_READY()){sb_unlock(&sb_registry_lock);return;}
    dir=getenv("SPU_BOUNDARY_DIR");pcs=getenv("SPU_BOUNDARY_PCS");
    cap=getenv("SPU_BOUNDARY_CAPACITY");start=getenv("SPU_BOUNDARY_LS_START");
    length=getenv("SPU_BOUNDARY_LS_LENGTH");policy=getenv("SPU_BOUNDARY_POLICY_ONLY");
    if(!dir||!*dir)goto done;
    if(*(const uint8_t*)&endian!=4){sb_error("unsupported host byte order");goto done;}
    if(strlen(dir)>=sizeof(sb_state.directory)||!pcs||strlen(pcs)>=sizeof(sb_state.pcs_text)
       ||!sb_number(cap,&sb_state.capacity)||sb_state.capacity<16||sb_state.capacity>1024
       ||!sb_number(start,&sb_state.ls_start)||!sb_number(length,&sb_state.ls_length)
       ||!sb_state.ls_length||sb_state.ls_length>SB_LS_BYTES
       ||sb_state.ls_start>0x40000u-sb_state.ls_length
       ||(policy&&strcmp(policy,"0")&&strcmp(policy,"1"))){sb_error("invalid configuration; capture unavailable");goto done;}
    {
        const char* where=getenv("SPU_BOUNDARY_MATCH_LS");
        const char* value=getenv("SPU_BOUNDARY_MATCH_BE32");
        if(where||value){
            if(!sb_number(where,&sb_state.match_start)||sb_state.match_start>0x3fffcu
               ||!sb_number(value,&sb_state.match_value)){sb_error("invalid LS word filter");goto done;}
            sb_state.match_enabled=1;
        }
    }
    {
        const char* p=pcs;
        while(*p){char* end;unsigned long pc;errno=0;pc=strtoul(p,&end,0);
            if(end==p||errno||pc>=0x40000u||(pc&3)||(*end&&*end!=',')){
                sb_error("invalid PC filter; capture unavailable");goto done;}
            sb_state.selected[pc>>2]=1;++count;
            if(!*end)break;p=end+1;if(!*p){sb_error("empty PC filter item");goto done;}
        }
    }
    if(!count){sb_error("empty PC filter");goto done;}
    {
        const char* limit=getenv("SPU_BOUNDARY_CONTEXT_LIMIT");
        const char* first=getenv("SPU_BOUNDARY_FIRST_PC");
        const char* arm=getenv("SPU_BOUNDARY_ARM_FILE");
        if(limit&&(!sb_number(limit,&sb_state.context_limit)||!sb_state.context_limit||sb_state.context_limit>16)){
            sb_error("invalid full-context limit (1..16)");goto done;}
        if(first){
            if(!sb_number(first,&sb_state.first_pc)||sb_state.first_pc>=0x40000u||
               (sb_state.first_pc&3)||!sb_state.selected[sb_state.first_pc>>2]){
                sb_error("first PC must be selected");goto done;}
            sb_state.first_pc_enabled=1;
        }
        if(arm){if(!*arm||strlen(arm)>=sizeof(sb_state.arm_file)){
            sb_error("invalid arm-file path");goto done;}strcpy(sb_state.arm_file,arm);}
    }
    strcpy(sb_state.directory,dir);strcpy(sb_state.pcs_text,pcs);
    sb_state.policy_only=policy&&!strcmp(policy,"1");sb_state.enabled=1;
done:
    SB_PUBLISH();sb_unlock(&sb_registry_lock);
}
static sb_context* sb_find_context(const void* owner,size_t context_bytes){
    sb_context* found=NULL;sb_lock(&sb_registry_lock);
    if(sb_state.context_limit&&(!context_bytes||context_bytes>524288u)){
        if(!sb_state.bad_context_reported){sb_state.bad_context_reported=1;sb_error("full-context hook size unavailable or too large");}
        sb_unlock(&sb_registry_lock);return NULL;
    }
    for(unsigned i=0;i<SB_CONTEXTS;i++){
        sb_context* c=&sb_state.contexts[i];
        if(c->owner==owner){
            found=c;
            if(c->context_bytes!=(sb_state.context_limit?(unsigned)context_bytes:0)&&!c->failed){
                c->failed=1;sb_error("context size changed; capture unavailable");}
            break;
        }
        if(!c->owner){
            char path[2304],header[SB_HEADER_BYTES]={0};int n;
            c->owner=owner;sb_mutex_init(&c->lock);found=c;
            c->context_bytes=sb_state.context_limit?(unsigned)context_bytes:0;
            c->commit_offset=(unsigned)offsetof(sb_record,committed_sequence)+
                c->context_bytes+(c->context_bytes?0x40000u:0);
            c->record_bytes=c->commit_offset+8;
            n=snprintf(path,sizeof(path),"%s/boundary-%u-%u.bin",sb_state.directory,sb_pid(),i);
            if(n<0||n>=(int)sizeof(path)||!(c->file=sb_open(path))){c->failed=1;sb_error("exclusive trace file creation failed");break;}
            n=snprintf(header,sizeof(header),"{\"format\":%u,\"endian\":16909060,\"phase\":\"fetched-before-execution\",\"context\":%llu,\"capacity\":%u,\"policy_only\":%d,\"match_enabled\":%d,\"match_start\":%u,\"match_value\":%u,\"record_size\":%u,\"gpr_offset\":%u,\"ls_offset\":%u,\"commit_offset\":%u,\"ls_start\":%u,\"ls_length\":%u,\"frequency\":%llu,\"pcs\":\"%s\",\"context_offset\":%u,\"context_bytes\":%u,\"full_ls_offset\":%u,\"full_ls_bytes\":%u,\"context_limit\":%u,\"first_pc_enabled\":%d,\"first_pc\":%u}",
                c->context_bytes?2u:1u,(unsigned long long)(uintptr_t)owner,sb_state.capacity,sb_state.policy_only,sb_state.match_enabled,sb_state.match_start,sb_state.match_value,c->record_bytes,
                (unsigned)offsetof(sb_record,gpr),(unsigned)offsetof(sb_record,ls),c->commit_offset,
                sb_state.ls_start,sb_state.ls_length,(unsigned long long)sb_frequency(),sb_state.pcs_text,
                (unsigned)offsetof(sb_record,committed_sequence),c->context_bytes,
                (unsigned)offsetof(sb_record,committed_sequence)+c->context_bytes,c->context_bytes?0x40000u:0,
                sb_state.context_limit,sb_state.first_pc_enabled,sb_state.first_pc);
            if(n<0||n>=(int)sizeof(header)||fwrite(header,1,sizeof(header),c->file)!=sizeof(header)||fflush(c->file)){
                c->failed=1;sb_error("trace header write failed");}
            break;
        }
    }
    if(!found&&!sb_state.full_reported){sb_state.full_reported=1;sb_error("context capacity reached; coverage incomplete");}
    sb_unlock(&sb_registry_lock);return found;
}
/* Kept out of the fast filter: a stack record and file I/O are only needed
 * for selected boundaries. Snapshots do not establish instruction retirement. */
#ifdef _WIN32
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void sb_capture(const void* owner,size_t context_bytes,uint32_t pc,uint32_t instruction,
                       const void* gpr,const uint8_t* ls,int policy){
    sb_context* c=sb_find_context(owner,context_bytes);sb_record r;
    uint8_t* bytes=(uint8_t*)&r;uint64_t zero=0;long offset;
    if(!c)return;sb_lock(&c->lock);
    if(c->failed||(sb_state.context_limit&&c->sequence>=sb_state.context_limit)||
       (!c->sequence&&sb_state.first_pc_enabled&&pc!=sb_state.first_pc)){
        sb_unlock(&c->lock);return;
    }
    memset(&r,0,sizeof(r));r.sequence=++c->sequence;r.tick=sb_tick();r.context=(uint64_t)(uintptr_t)owner;
    r.thread=sb_tid();r.pc=pc;r.instruction=instruction;r.policy=(uint32_t)policy;
    memcpy(r.gpr,gpr,sizeof(r.gpr));memcpy(r.ls,ls+sb_state.ls_start,sb_state.ls_length);
    if(c->context_bytes){
        bytes=(uint8_t*)calloc(1,c->record_bytes);
        if(!bytes){c->failed=1;sb_error("full-context allocation failed");sb_unlock(&c->lock);return;}
        memcpy(bytes,&r,offsetof(sb_record,committed_sequence));
        memcpy(bytes+offsetof(sb_record,committed_sequence),owner,c->context_bytes);
        memcpy(bytes+offsetof(sb_record,committed_sequence)+c->context_bytes,ls,0x40000u);
    }
    offset=(long)(SB_HEADER_BYTES+((r.sequence-1)%sb_state.capacity)*c->record_bytes);
    if(fseek(c->file,offset+(long)c->commit_offset,SEEK_SET)
       ||fwrite(&zero,sizeof(zero),1,c->file)!=1||fflush(c->file)
       ||fseek(c->file,offset,SEEK_SET)||fwrite(bytes,1,c->record_bytes,c->file)!=c->record_bytes||fflush(c->file)
       ||fseek(c->file,offset+(long)c->commit_offset,SEEK_SET)
       ||fwrite(&r.sequence,sizeof(r.sequence),1,c->file)!=1||fflush(c->file)){
        c->failed=1;sb_error("trace record write failed; coverage incomplete");}
    if(c->context_bytes)free(bytes);
    sb_unlock(&c->lock);
}
/* Arm-file polling is limited to selected boundaries and at most 10 Hz per
 * host thread. It gates observation only; it never changes guest execution. */
static int sb_arm_ready(void){
#ifdef _WIN32
    static __declspec(thread) int armed,failed;
    static __declspec(thread) uint64_t next_check;
#else
    static __thread int armed,failed;
    static __thread uint64_t next_check;
#endif
    uint64_t now;FILE* f;
    if(!sb_state.arm_file[0]||armed)return 1;
    if(failed)return 0;
    now=sb_tick();if(now<next_check)return 0;next_check=now+sb_frequency()/10;
    f=fopen(sb_state.arm_file,"rb");
    if(f){fclose(f);armed=1;return 1;}
    if(errno!=ENOENT){failed=1;sb_error("arm-file read failed; capture unavailable");}
    return 0;
}
static inline void spu_boundary_observe_context(const void* owner,size_t context_bytes,uint32_t pc,uint32_t instruction,
                                        const void* gpr,const uint8_t* ls,int policy){
    if(!SB_READY())sb_initialize();
    if(!sb_state.enabled||(sb_state.policy_only&&!policy)||pc>=0x40000u||(pc&3)||!sb_state.selected[pc>>2])return;
    if(sb_state.match_enabled){
        const uint8_t* p=ls+sb_state.match_start;
        uint32_t word=((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
        if(word!=sb_state.match_value)return;
    }
    if(!sb_arm_ready())return;
    sb_capture(owner,context_bytes,pc,instruction,gpr,ls,policy);
}
/* Legacy callers retain version-1 rings unless full-context capture was
 * requested. A missing size then fails explicitly, never yielding zero state. */
static inline void spu_boundary_observe(const void* owner,uint32_t pc,uint32_t instruction,
                                        const void* gpr,const uint8_t* ls,int policy){
    spu_boundary_observe_context(owner,0,pc,instruction,gpr,ls,policy);
}

#endif
