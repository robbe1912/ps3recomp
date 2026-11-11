/* Optional read-only texture snapshot. Diagnostic bytes stay in owned output.
 * Snapshot/hash work and synchronized I/O perturb timing; not a perf profile. */
#ifndef RSX_TEXTURE_CAPTURE_H
#define RSX_TEXTURE_CAPTURE_H
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct tex_capture_key {
    uint32_t bound,source,w,h,fmt,mips,face,bytes,pitch;
    uint64_t fingerprint;
};
static SRWLOCK tex_capture_lock=SRWLOCK_INIT;
static struct tex_capture_key tex_capture_seen[32];
static unsigned tex_capture_count,tex_capture_attempts;
static uint64_t tex_capture_total;
static int tex_capture_full;

static int tex_capture_write(const char* path,const void* data,DWORD size)
{
    HANDLE f=CreateFileA(path,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) { fprintf(stderr,"[TEX_RAW] create failed error=%lu path=%s\n",GetLastError(),path);return 0; }
    DWORD wrote=0;int ok=WriteFile(f,data,size,&wrote,NULL) && wrote==size;
    DWORD err=GetLastError();int closed=CloseHandle(f);if(!closed)err=GetLastError();
    if(!ok || !closed) { fprintf(stderr,"[TEX_RAW] write/close failed error=%lu bytes=%lu/%lu path=%s\n",err,wrote,size,path);return 0; }
    return 1;
}
static int tex_capture_equal(const struct tex_capture_key* a,const struct tex_capture_key* b)
{
    return a->bound==b->bound && a->source==b->source && a->w==b->w && a->h==b->h &&
        a->fmt==b->fmt && a->mips==b->mips && a->face==b->face && a->bytes==b->bytes &&
        a->pitch==b->pitch && a->fingerprint==b->fingerprint;
}
static void rsx_capture_texture(const uint8_t* base,uint32_t bound,uint32_t source,
    uint32_t w,uint32_t h,uint32_t fmt,uint32_t mips,uint32_t face,uint32_t bytes,uint32_t pitch)
{
    const char* enable=getenv("TEX_RAWDBG");
    if(!enable || !*enable || !strcmp(enable,"0"))return;
    const char* dir=getenv("TEX_RAWDBG_DIR");
    AcquireSRWLockExclusive(&tex_capture_lock);
    if(tex_capture_attempts>=32 || tex_capture_total>=128ull*1024*1024) {
        if(!tex_capture_full) { tex_capture_full=1;fprintf(stderr,"[TEX_RAW] capture budget reached; later uploads unobserved\n"); }
        goto done;
    }
    if(!dir || !*dir || GetFileAttributesA(dir)==INVALID_FILE_ATTRIBUTES || !(GetFileAttributesA(dir)&FILE_ATTRIBUTE_DIRECTORY)) {
        tex_capture_attempts++;fprintf(stderr,"[TEX_RAW] existing TEX_RAWDBG_DIR is required\n");goto done;
    }
    if(!base || !bytes || bytes>16u*1024*1024 || (uint64_t)source+bytes>0x100000000ull || tex_capture_total+bytes>128ull*1024*1024) {
        tex_capture_attempts++;fprintf(stderr,"[TEX_RAW] invalid/over-budget span source=%08X bytes=%u\n",source,bytes);goto done;
    }
    uint8_t* copy=(uint8_t*)malloc(bytes);SIZE_T got=0;
    uintptr_t addr=(uintptr_t)base+source;
    if(!copy || addr<(uintptr_t)base || !ReadProcessMemory(GetCurrentProcess(),(const void*)addr,copy,bytes,&got) || got!=bytes) {
        tex_capture_attempts++;fprintf(stderr,"[TEX_RAW] snapshot failed source=%08X bytes=%u\n",source,bytes);free(copy);goto done;
    }
    uint64_t hash=14695981039346656037ull;
    for(uint32_t i=0;i<bytes;i++)hash=(hash^copy[i])*1099511628211ull;
    struct tex_capture_key key={bound,source,w,h,fmt,mips,face,bytes,pitch,hash};
    for(unsigned i=0;i<tex_capture_count;i++)if(tex_capture_equal(&key,&tex_capture_seen[i])) { free(copy);goto done; }
    unsigned sequence=tex_capture_attempts++;
    char raw[1024],meta[1024],record[768];
    int n=snprintf(raw,sizeof raw,"%s/raw_%lu_%u_%08X.bin",dir,GetCurrentProcessId(),sequence,source);
    int m=snprintf(meta,sizeof meta,"%s/raw_%lu_%u_%08X.json",dir,GetCurrentProcessId(),sequence,source);
    if(n<0 || n>=(int)sizeof raw || m<0 || m>=(int)sizeof meta) {
        fprintf(stderr,"[TEX_RAW] output path too long\n");free(copy);goto done;
    }
    LARGE_INTEGER qpc;if(!QueryPerformanceCounter(&qpc)) { fprintf(stderr,"[TEX_RAW] QPC failed\n");free(copy);goto done; }
    /* Write the actual event timestamp using the same complete metadata shape. */
    n=snprintf(record,sizeof record,"{\"qpc\":%llu,\"host_tid\":%lu,\"bound\":%u,\"source\":%u,\"width\":%u,\"height\":%u,\"format\":%u,\"mips\":%u,\"face\":%u,\"pitch\":%u,\"bytes\":%u,\"fingerprint_fnv1a64\":\"%016llX\",\"coverage\":\"upload-cache-miss-only\"}\n",(unsigned long long)qpc.QuadPart,GetCurrentThreadId(),bound,source,w,h,fmt,mips,face,pitch,bytes,(unsigned long long)hash);
    int ok=tex_capture_write(raw,copy,bytes);
    tex_capture_total+=bytes;
    if(ok && n>0 && n<(int)sizeof record && tex_capture_write(meta,record,(DWORD)n)) {
        tex_capture_seen[tex_capture_count++]=key;
        fprintf(stderr,"[TEX_RAW] captured source=%08X bytes=%u sequence=%u\n",source,bytes,sequence);
    }
    free(copy);
done:
    ReleaseSRWLockExclusive(&tex_capture_lock);
}
#endif
