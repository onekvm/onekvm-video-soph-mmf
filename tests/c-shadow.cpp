#include "../src/lt6911_c_shadow.hpp"
#include <array>
#include <cassert>
#include <cstdio>
using namespace onekvm::video_backend;
struct Fake {
    std::array<uint8_t,256> ram{};
    uint8_t gate=0, hpd=0x28, control=2, id0=0x16;
    unsigned index=0;
    int calls=0, fail=-1, fail_forever=-1;
    bool journaled=false, reject_journal=false, fault_after_write=false;
    bool fault() { return calls++ == fail || (fail_forever>=0 && calls>fail_forever); }
    static int read(void *p,uint8_t bank,uint8_t reg,uint8_t *v) {
        auto &s=*static_cast<Fake*>(p); if(s.fault())return -1;
        if(bank==0xa0 && reg<2)*v=reg?5:s.id0;
        else if(bank==0xc0 && reg==6)*v=s.hpd;
        else if(bank==0xc0 && reg==0x8e)*v=s.control;
        else assert(false); // No C090 RAM reads, 80EE reads or UXC watchdog.
        return 0;
    }
    static int write(void *p,uint8_t bank,uint8_t reg,uint8_t v) {
        auto &s=*static_cast<Fake*>(p); const bool fault=s.fault();
        if(fault && !s.fault_after_write)return -1;
        if(bank==0x80 && reg==0xee)s.gate=v;
        else if(bank==0xc0 && reg==6)s.hpd=v;
        else if(bank==0xc0 && reg==0x8e) {s.control=v;if(v==3)s.index=0;}
        else if(bank==0xc0 && reg==0x90) {
            assert(!(s.hpd&8) && s.gate==1 && s.control==3 && s.index<256);
            s.ram[s.index++]=v;
        } else assert(false); // Any Flash-controller access is a failure.
        return fault?-1:0;
    }
    static int prepare(void *p,const CShadowControl &control) {
        auto &s=*static_cast<Fake*>(p);
        assert(s.hpd==control.hpd && s.control==control.control && (s.hpd&8));
        s.journaled=true;return s.reject_journal?-1:0;
    }
    ShadowIO io(){return {this,read,write,[](unsigned){}};}
    void restored(){assert(gate==0 && hpd==0x28 && control==2);}
};
int main(){
    Fake initial;
    const uint8_t header[]={0,255,255,255,255,255,255,0};
    std::memcpy(initial.ram.data(),header,8);initial.ram[126]=1;
    uint8_t sum=0;for(unsigned i=0;i<127;++i)sum+=initial.ram[i];initial.ram[127]=-sum;
    auto desired=initial.ram;desired[80]=42;desired[127]-=42;
    Fake good=initial;
    assert(!c_shadow_apply(good.io(),initial.ram.data(),desired.data(),Fake::prepare));
    assert(good.ram==desired && good.journaled);good.restored();
    int calls=good.calls;
    for(int fail=0;fail<calls;++fail)for(bool applied:{false,true}){
        Fake test=initial;test.fail=fail;test.fault_after_write=applied;
        assert(c_shadow_apply(test.io(),initial.ram.data(),desired.data(),Fake::prepare)!=0);
        test.restored();assert(test.ram==initial.ram);
    }
    Fake interrupted=initial;interrupted.ram=desired;interrupted.control=3;interrupted.hpd=0x20;interrupted.index=57;
    CShadowControl saved{0x28,2};
    assert(!c_shadow_apply(interrupted.io(),initial.ram.data(),initial.ram.data(),nullptr,&saved));
    interrupted.restored();assert(interrupted.ram==initial.ram);
    Fake bad=initial;bad.id0=0x17;
    assert(c_shadow_apply(bad.io(),initial.ram.data(),desired.data())==-ENODEV);
    bad.restored();assert(bad.ram==initial.ram);
    Fake busy=initial;busy.control=3;
    assert(c_shadow_apply(busy.io(),initial.ram.data(),desired.data())==-EOPNOTSUPP);
    assert(busy.ram==initial.ram && busy.hpd==0x28 && busy.gate==0 && busy.control==3);
    Fake journal=initial;journal.reject_journal=true;
    assert(c_shadow_apply(journal.io(),initial.ram.data(),desired.data(),Fake::prepare)==-EIO);
    journal.restored();assert(journal.ram==initial.ram);
    Fake broken=initial;broken.fail_forever=30;
    assert(c_shadow_apply(broken.io(),initial.ram.data(),desired.data())==-EUCLEAN);
    assert(!(broken.hpd&8));
    auto invalid=desired;invalid[80]++;
    Fake rejected=initial;
    assert(c_shadow_apply(rejected.io(),initial.ram.data(),invalid.data())==-EINVAL && rejected.calls==0);
    std::printf("C shadow: success, interrupted transaction, chip/idle/journal guards, persistent fault and %d transfer fault cases passed\n",calls*2);
}
