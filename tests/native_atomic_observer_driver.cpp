#include "native_atomic_observer_support.h"

alignas(128) static uint8_t arena[16384];
uint8_t* vm_base=arena;
int fixture_lock_held,fixture_lock_opens,fixture_notifications,fixture_rewrite_on_unlock;
unsigned g_spu_putllc_count,g_spu_putllc_sync_hit;
uint32_t g_barrier_sync_watch;
void (*g_spu_line_commit_hook)(uint32_t);
void (*g_spu_host_line_commit_hook)(uint32_t);
static unsigned host_notifications, line_notifications;
static unsigned taskset_notifications,event_count;
static uint64_t ticks=100;
static int clock_mode;
static int accept_enabled=1;
static ps3_atomic_observation last;

void spu_lockline_lock(void){assert(!fixture_lock_held);fixture_lock_held=1;}
void spu_lockline_unlock(void){
    assert(fixture_lock_held);fixture_lock_held=0;fixture_lock_opens++;
    if(fixture_rewrite_on_unlock)memset(arena+256,0xCC,128);
}
void spu_coh_notify_write(uint32_t){assert(fixture_lock_held);fixture_notifications++;}
void spu_taskset_line_committed(uint32_t){assert(!fixture_lock_held);taskset_notifications++;}
void spu_lockguard_check(spu_context*,uint32_t,const uint8_t*,uint32_t,const char*){
    assert(fixture_lock_held);
}
/* Reserve more address space than the authored committed range, like the VM. */
int mfc_ea_range_committed(uint32_t ea,uint32_t n){return ea<=4096&&n<=4096-ea;}
void yz_lockstep_tick(spu_context*){}
void spu_coh_reserve(spu_context*,uint32_t){assert(fixture_lock_held);}
static uint64_t now(void*){
    assert(!fixture_lock_held); /* Clock and delivery must both be outside locks. */
    assert(!issue219_ppu_lock_held());
    if(clock_mode==1)return 0;
    if(clock_mode==2)return ticks--;
    return ++ticks;
}
static void observe(void*,const ps3_atomic_observation* e){
    assert(!fixture_lock_held);assert(!issue219_ppu_lock_held());last=*e;event_count++;
}
static int accept(void*,uint32_t,uintptr_t,uint64_t,uint32_t){
    assert(!fixture_lock_held);assert(!issue219_ppu_lock_held());return accept_enabled;
}
static void line_hook(uint32_t line){assert(!fixture_lock_held && line==256);line_notifications++;}
static void host_hook(uint32_t line){assert(!fixture_lock_held && line==256);host_notifications++;}
static void be32(uint8_t*p,uint32_t x){for(int i=3;i>=0;i--){p[i]=(uint8_t)x;x>>=8;}}
static void be64(uint8_t*p,uint64_t x){for(int i=7;i>=0;i--){p[i]=(uint8_t)x;x>>=8;}}
static int all(const uint8_t*p,unsigned n,uint8_t value){for(unsigned i=0;i<n;i++)if(p[i]!=value)return 0;return 1;}
static void check_event(unsigned old_count,int enabled,int committed,int attempted){
    assert(event_count==old_count+(unsigned)enabled);
    if(!enabled)return;
    assert(!!(last.flags&PS3_ATOMIC_COMMITTED)==committed);
    assert(!!(last.flags&PS3_ATOMIC_ATTEMPTED)==attempted);
    if(!clock_mode)assert(last.begin_tick&&last.end_tick>=last.begin_tick);
}
int main(void){
    ps3_atomic_observer observer={NULL,now,observe,accept},bad={NULL,NULL,observe,NULL};
    assert(!ps3_atomic_observer_install_ppu(&bad)&&!ps3_atomic_observer_install_spu(&bad));
    const int compiled=PS3_ATOMIC_OBSERVATION;
    for(int enabled=0;enabled<=compiled;enabled++){
        assert(ps3_atomic_observer_install_ppu(enabled?&observer:NULL));
        assert(ps3_atomic_observer_install_spu(enabled?&observer:NULL));
        for(int wide=0;wide<2;wide++)for(int off=0;off<2;off++)for(int coherent=0;coherent<2;coherent++){
            for(int outcome=0;outcome<3;outcome++){
                memset(arena,0,sizeof(arena));fixture_notifications=0;
                uint64_t old=wide?UINT64_C(0x1020304050607080):0x10203040;
                uint64_t value=wide?UINT64_C(0x9080706050403020):0x90807060;
                if(wide)be64(arena+256,old);else be32(arena+256,(uint32_t)old);
                uint8_t before[128],expected[128];memcpy(before,arena+256,128);memcpy(expected,before,128);
                const int lost=outcome==2&&!off,success=outcome==0||(outcome==2&&off);
                issue219_ppu_setup(lost?512:256,off,coherent);
                unsigned n=event_count;
                int ok=wide?ppu_stdcx64(256,outcome==1?old+1:old,value):
                    ppu_stwcx32(256,(uint32_t)(outcome==1?old+1:old),(uint32_t)value);
                assert(ok==success);
                if(success){if(wide)be64(expected,value);else be32(expected,(uint32_t)value);}
                assert(!memcmp(arena+256,expected,128));
                assert(fixture_notifications==(success&&coherent&&!off));
                assert(issue219_ppu_breaks()==(unsigned)(success&&!off?(wide?2:1):0));
                if(success&&!off)assert(issue219_ppu_reservation_lost());
                check_event(n,enabled,success,!lost);
                if(enabled){
                    assert(last.source==(unsigned)(wide?PS3_ATOMIC_PPU64:PS3_ATOMIC_PPU32));
                    assert(last.address==256&&last.size==(unsigned)(wide?8:4));
                    assert(last.context&&last.return_address==0x1234);
                    if(lost)assert((last.flags&(PS3_ATOMIC_RESERVATION_LOST|PS3_ATOMIC_DATA_UNAVAILABLE))==
                        (PS3_ATOMIC_RESERVATION_LOST|PS3_ATOMIC_DATA_UNAVAILABLE));
                    else{assert(!memcmp(last.before,before,last.size));assert(!memcmp(last.after,expected,last.size));}
                    assert(all(last.before+last.size,128-last.size,0));
                }
            }
        }
        spu_context*c=(spu_context*)calloc(1,sizeof(*c));assert(c);c->ls=c->ls_store;
        g_spu_line_commit_hook=line_hook;
        g_spu_host_line_commit_hook=host_hook;
        for(unsigned mode=0;mode<9;mode++){
            memset(arena,0x11,sizeof(arena));memset(c->ls,0x22,SPU_LS_SIZE);
            c->mfc_eal=256;c->mfc_lsa=128;c->pc=0x3240;c->gpr[0]._u32[0]=0x100;
            c->resv_valid=1;c->resv_ea=256;memcpy(c->resv_line,arena+256,128);
            if(mode==1)c->resv_valid=0;
            if(mode==2)c->resv_ea=512;
            if(mode==3)c->resv_line[7]^=1;
            if(mode>=6)c->mfc_eal=8192;
            uint32_t command=mode==4||mode==7?MFC_PUTLLUC_CMD:
                mode==5||mode==8?MFC_PUTQLLUC_CMD:MFC_PUTLLC_CMD;
            fixture_notifications=0;taskset_notifications=0;unsigned n=event_count;
            host_notifications=0;line_notifications=0;
            const int success=mode==0||mode==4||mode==5;
            fixture_rewrite_on_unlock=success;
            assert(issue219_atomic(c,command)==1);fixture_rewrite_on_unlock=0;
            assert(c->atomic_stat==(unsigned)(mode==4||mode==5?2:success?0:1));
            assert(fixture_notifications==success&&taskset_notifications==(unsigned)success);
            assert(host_notifications==(unsigned)success && line_notifications==(unsigned)success);
            check_event(n,enabled,success,mode<6);
            if(enabled){
                assert(last.source==PS3_ATOMIC_SPU_STORE&&last.size==128&&last.command==command);
                assert(last.context==(uintptr_t)c&&last.saved_pc==0x3240&&last.return_address==0x100);
                if(mode>=6)assert((last.flags&(PS3_ATOMIC_INVALID_RANGE|PS3_ATOMIC_DATA_UNAVAILABLE))==
                    (PS3_ATOMIC_INVALID_RANGE|PS3_ATOMIC_DATA_UNAVAILABLE));
                else{assert(all(last.before,128,0x11));assert(all(last.after,128,success?0x22:0x11));}
            }
            assert(all(arena+256,128,success?0xCC:0x11));
        }
        unsigned n=event_count;c->mfc_eal=256;c->mfc_lsa=128;
        assert(issue219_atomic(c,MFC_GETLLAR_CMD)==1);assert(event_count==n);
        assert(issue219_atomic(c,0x20)==0);assert(event_count==n);
        uint64_t prior_ticks=ticks;accept_enabled=0;
        memset(c->ls+128,0x33,128);
        assert(issue219_atomic(c,MFC_PUTLLUC_CMD)==1);
        assert(event_count==n&&ticks==prior_ticks&&all(arena+256,128,0x33));
        accept_enabled=1;
        free(c);
    }
    if(compiled){
        assert(ps3_atomic_observer_install_ppu(&observer));
        unsigned n=event_count;uint64_t previous_ticks=ticks;accept_enabled=0;
        issue219_ppu_setup(256,1,0);be32(arena+256,10);
        assert(ppu_stwcx32(256,10,11));
        assert(event_count==n&&ticks==previous_ticks);accept_enabled=1;
        issue219_ppu_setup(256,1,0);issue219_ppu_no_context();be32(arena+256,10);
        assert(ppu_stwcx32(256,10,11));assert(last.flags&PS3_ATOMIC_CONTEXT_UNKNOWN);
        for(clock_mode=1;clock_mode<=2;clock_mode++){
            issue219_ppu_setup(256,1,0);be32(arena+256,10);
            assert(ppu_stwcx32(256,10,11));assert(last.flags&PS3_ATOMIC_CLOCK_INVALID);
        }
    }else assert(!ps3_atomic_observer_install_ppu(&observer)&&!ps3_atomic_observer_install_spu(&observer));
    assert(ps3_atomic_observer_install_ppu(NULL)&&ps3_atomic_observer_install_spu(NULL));
    puts("PASS actual PPU/SPU atomic observations: outcomes, bytes, ownership, locks, clock, compiled-out mode");
}
