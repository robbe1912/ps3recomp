/* Default-off, one-event Windows PPU fault snapshots. No guest writes.
 * Context is materialized runtime memory, not a complete architectural trace.
 * Guest regions are sequential reads; concurrent writers can change them.
 */
#ifndef PS3RECOMP_PPU_FAULT_CAPTURE_H
#define PS3RECOMP_PPU_FAULT_CAPTURE_H
#include <stdint.h>
#ifdef _WIN32
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <atomic>
#include <sstream>
#include <string>

namespace ppu_fault_detail {
inline void fail(const char* operation) {
    fprintf(stderr,"[ppu-fault-capture] FATAL %s error=%lu\n",operation,GetLastError());
    fflush(stderr); abort();
}
inline void write(const std::string& path,const void* data,size_t size) {
    HANDLE f=CreateFileA(path.c_str(),GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if(f==INVALID_HANDLE_VALUE) fail("create output");
    DWORD done=0;BOOL ok=WriteFile(f,data,(DWORD)size,&done,NULL);
    DWORD error=GetLastError();BOOL closed=CloseHandle(f);
    if(!ok || done!=size || !closed){SetLastError(error);fail("write/close output");}
}
inline void region(std::ostringstream& meta,const std::string& dir,
                   const std::string& label,uintptr_t base,uint32_t ea) {
    unsigned char bytes[256]={0};SIZE_T got=0;DWORD error=0;
    bool in_range=(uint64_t)ea+sizeof bytes<=0x100000000ull &&
                   base<=UINTPTR_MAX-(uintptr_t)ea-sizeof bytes;
    BOOL ok=FALSE;
    if(in_range){ok=ReadProcessMemory(GetCurrentProcess(),(void*)(base+ea),bytes,sizeof bytes,&got);
        if(!ok) error=GetLastError();}
    else error=ERROR_ARITHMETIC_OVERFLOW;
    std::string file="fault-"+label+".bin";
    if(got) write(dir+"/"+file,bytes,got);
    meta<<"{\"label\":\""<<label<<"\",\"ea\":"<<ea<<",\"requested\":256,\"read\":"<<got
        <<",\"complete\":"<<(ok&&got==256?"true":"false")<<",\"error\":"<<error
        <<",\"file\":";
    if(got) meta<<"\""<<file<<"\"";else meta<<"null";
    meta<<"}";
}
}
/* normalize must be the runtime's pure EA normalizer, or NULL for flat memory.
 * Capture both requested and normalized addresses; never invoke VM accessors
 * with read-triggered MMIO/coherence or diagnostic side effects here. */
template<class Context>
inline void ppu_fault_capture(const Context* ctx,const unsigned char* vm,
                              uint32_t(*normalize)(uint32_t)=NULL) {
    static const char* dir=getenv("PPU_FAULT_CAPTURE_DIR");
    if(!dir || !*dir) return;
    static std::atomic_flag captured=ATOMIC_FLAG_INIT;
    if(captured.test_and_set(std::memory_order_relaxed)) return;
    if(!ctx || !vm){SetLastError(ERROR_INVALID_PARAMETER);ppu_fault_detail::fail("null input");}
    Context copy=*ctx;
    std::string output(dir);
    ppu_fault_detail::write(output+"/fault-context.bin",&copy,sizeof copy);
    LARGE_INTEGER ticks,hz;QueryPerformanceCounter(&ticks);QueryPerformanceFrequency(&hz);
    FILETIME created,exited,kernel,user,utc;GetSystemTimeAsFileTime(&utc);
    if(!GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user)) ppu_fault_detail::fail("owner time");
    auto ft=[](FILETIME f)->uint64_t{return ((uint64_t)f.dwHighDateTime<<32)|f.dwLowDateTime;};
    std::ostringstream meta;
    meta<<"{\"schema\":1,\"pid\":"<<GetCurrentProcessId()<<",\"host_tid\":"<<GetCurrentThreadId()
        <<",\"creation_filetime\":"<<ft(created)<<",\"utc_filetime\":"<<ft(utc)
        <<",\"qpc\":"<<ticks.QuadPart<<",\"clock_hz\":"<<hz.QuadPart
        <<",\"guest_tid\":"<<copy.thread_id<<",\"target\":"<<(uint32_t)copy.ctr
        <<",\"lr\":"<<(uint32_t)copy.lr<<",\"context_size\":"<<sizeof copy
        <<",\"vm_base\":"<<(uintptr_t)vm<<",\"regions\":[";
    for(unsigned i=0;i<33;i++) {
        uint32_t ea=(uint32_t)(i<32?copy.gpr[i]:copy.ctr);
        uint32_t translated=normalize?normalize(ea):ea;
        char label[24];snprintf(label,sizeof label,i<32?"r%02u-raw":"target-raw",i);
        if(i) meta<<",";
        ppu_fault_detail::region(meta,output,label,(uintptr_t)vm,ea);
        if(translated!=ea){meta<<",";snprintf(label,sizeof label,i<32?"r%02u-effective":"target-effective",i);
            ppu_fault_detail::region(meta,output,label,(uintptr_t)vm,translated);}
    }
    meta<<"],\"complete\":true,\"limits\":\"One error event; sequential memory, materialized context; not writer chronology.\"}\n";
    std::string text=meta.str();ppu_fault_detail::write(output+"/fault.json",text.data(),text.size());
    fprintf(stderr,"[ppu-fault-capture] one event saved; original error path continues\n");
}
#else
#include <stdio.h>
#include <stdlib.h>
template<class Context>
inline void ppu_fault_capture(const Context*,const unsigned char*,uint32_t(*)(uint32_t)=NULL) {
    const char* dir=getenv("PPU_FAULT_CAPTURE_DIR");
    if(dir && *dir){fprintf(stderr,"[ppu-fault-capture] FATAL snapshots require Windows\n");abort();}
}
#endif
#endif
