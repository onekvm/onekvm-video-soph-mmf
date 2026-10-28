#include "../src/lt6911_uxc_shadow.hpp"
#include <array>
#include <cassert>
#include <cstdio>
using namespace onekvm::video_backend;
struct Fake {
    std::array<uint8_t, 256> ram{};
    uint8_t gate=0, watchdog=5, index=19, control=0x14, hpd=0x8d;
    int calls=0, fail=-1;
    bool fault() { return calls++ == fail; }
    static int read(void *p, uint8_t bank, uint8_t reg, uint8_t *v) {
        auto &s=*static_cast<Fake*>(p); if (s.fault()) return -1;
        if (bank==0x80 && reg==0xee) *v=s.gate;
        else if (bank==0x80 && reg==0x10) *v=s.watchdog;
        else if (bank==0x81 && reg<3) *v=std::array<uint8_t,3>{0x17,4,0x83}[reg];
        else if (bank==0xb0 && reg==0x0a) *v=s.index;
        else if (bank==0xb0 && reg==0x0b) *v=s.control;
        else if (bank==0xb0 && reg==0x00) *v=s.hpd;
        else if (bank==0xb0 && reg==0xb0) { assert(!(s.hpd&1)); *v=s.ram[s.index]; }
        else assert(false);
        return 0;
    }
    static int write(void *p, uint8_t bank, uint8_t reg, uint8_t v) {
        auto &s=*static_cast<Fake*>(p); if (s.fault()) return -1;
        // Any flash-controller access is a test failure.
        if (bank==0x80 && reg==0xee) s.gate=v;
        else if (bank==0x80 && reg==0x10) s.watchdog=v;
        else if (bank==0xb0 && reg==0x0a) s.index=v;
        else if (bank==0xb0 && reg==0x0b) s.control=v;
        else if (bank==0xb0 && reg==0x00) s.hpd=v;
        else if (bank==0xb0 && reg==0xb0) { assert(!(s.hpd&1) && !(s.control&4)); s.ram[s.index]=v; }
        else assert(false);
        return 0;
    }
    UxcShadowIO io() { return {this,read,write,[](unsigned){}}; }
    void restored() { assert(gate==0 && watchdog==5 && index==19 && control==0x14 && hpd==0x8d); }
};
int main() {
    Fake initial;
    const uint8_t header[]={0,255,255,255,255,255,255,0};
    std::memcpy(initial.ram.data(),header,8); initial.ram[126]=1;
    for(unsigned block=0;block<2;++block) {
        uint8_t sum=0; for(unsigned j=0;j<127;++j) sum+=initial.ram[block*128+j];
        initial.ram[block*128+127]=-sum;
    }
    auto desired=initial.ram; desired[80]=42; desired[127]-=42;
    uint8_t actual[256]{};
    Fake good=initial;
    assert(!uxc_shadow_apply(good.io(),desired.data(),actual));
    assert(good.ram==desired && !std::memcmp(actual,desired.data(),256)); good.restored();
    const int calls=good.calls;
    for(int fail=0;fail<calls;++fail) {
        Fake test=initial; test.fail=fail;
        assert(uxc_shadow_apply(test.io(),desired.data(),actual)!=0);
        test.restored(); assert(test.ram==initial.ram);
    }
    Fake read=initial; assert(!uxc_shadow_apply(read.io(),nullptr,actual));
    read.restored(); assert(read.ram==initial.ram);
    auto invalid=desired; invalid[80]++;
    Fake rejected=initial; assert(uxc_shadow_apply(rejected.io(),invalid.data(),actual)==-EINVAL);
    assert(rejected.calls==0);
    std::printf("shadow transaction: success, read, invalid input and %d I2C failure points passed\n",calls);
}
