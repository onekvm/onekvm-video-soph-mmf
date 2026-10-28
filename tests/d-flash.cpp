#include "../src/lt6911_d_flash.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>

using namespace onekvm::video_backend;
namespace {
std::array<uint8_t, 0x10000> flash;
uint8_t registers[256][256], fifo[32];
unsigned fifo_size, calls, fail_at, fail_at2, erases, programs, reads, corrupt_read;
bool bad_program;
int fault() {
    ++calls;
    return calls == fail_at ? -EIO : calls == fail_at2 ? -EREMOTEIO : 0;
}
unsigned address() {
    return (unsigned(registers[0xe0][0x5b]) << 16) |
        (unsigned(registers[0xe0][0x5c]) << 8) | registers[0xe0][0x5d];
}
int wr(uint8_t bank, uint8_t reg, uint8_t value) {
    assert(bank == 0xe0 || bank == 0xe1);
    if (int rc=fault()) return rc;
    registers[bank][reg]=value;
    if (bank == 0xe1 && reg == 3 && value == 0xbf) fifo_size=0;
    if (bank == 0xe0 && reg == 0x5a && value == 1) {
        assert(address() == 0x8000);
        std::fill(flash.begin()+0x8000,flash.begin()+0x9000,0xff);
        ++erases;
    }
    if (bank == 0xe0 && reg == 0x5a && value == 0x10 && fifo_size) {
        unsigned pos=address(); assert(pos >= 0x8000 && pos+fifo_size <= 0x9000);
        std::copy(fifo,fifo+fifo_size,flash.begin()+pos);
        if (bad_program) flash[pos] ^= 1;
        fifo_size=0; ++programs;
    }
    return 0;
}
int rd(uint8_t bank, uint8_t reg, uint8_t *data, size_t len) {
    assert(bank == 0xe0 && reg == 0x5f && len == 32);
    if (int rc=fault()) return rc;
    assert(address() >= 0x8000 && address()+len <= 0x9000);
    std::copy(flash.begin()+address(),flash.begin()+address()+len,data);
    if (++reads == corrupt_read) data[0] ^= 1;
    return 0;
}
int block(uint8_t bank, uint8_t reg, const uint8_t *data, size_t len) {
    assert(bank == 0xe0 && reg == 0x59 && len == 32);
    if (int rc=fault()) return rc;
    std::copy(data,data+len,fifo); fifo_size=len;
    return 0;
}
void wait(unsigned) {}
DFlashIO io{wr,rd,block,wait};
void reset() {
    flash.fill(0xff);
    std::fill(flash.begin(),flash.begin()+0x8000,0x53); // MCU program sentinel
    flash[0x8100]=0x91; flash[0x8fff]=0xa2;
    std::memset(registers,0,sizeof(registers));
    fifo_size=calls=fail_at=fail_at2=erases=programs=reads=corrupt_read=0;
    bad_program=false;
}
}
int main() {
    std::array<uint8_t,256> edid{}, actual{};
    for(unsigned n=0;n<edid.size();++n) edid[n]=n ^ 0x5a;
    reset();
    assert(!d_flash_write_edid(io,edid.data(),edid.size()));
    assert(erases==1 && programs==10 && registers[0xe0][0xee]==0);
    assert(flash[0x8100]==0x91 && flash[0x8fff]==0xa2);
    assert(!std::memcmp(flash.data()+0x8000,edid.data(),edid.size()));
    assert(!d_flash_write_edid(io,edid.data(),edid.size()) && erases==1);
    calls=0; assert(!d_flash_read(io,0x8000,actual.data(),actual.size()) && actual==edid);
    unsigned read_calls=calls;
    for(unsigned n=1;n<=read_calls;++n) {
        reset(); fail_at=n;
        assert(d_flash_read(io,0x8000,actual.data(),actual.size())==-EIO);
        assert(registers[0xe0][0xee]==(n==read_calls) && !erases && !programs);
    }
    reset(); assert(!d_flash_write_edid(io,edid.data(),edid.size()));
    unsigned write_calls=calls;
    for(unsigned n=1;n<=write_calls;++n) {
        reset(); fail_at=n;
        assert(d_flash_write_edid(io,edid.data(),edid.size())==-EIO);
        assert(std::all_of(flash.begin(),flash.begin()+0x8000,[](uint8_t b){return b==0x53;}));
    }
    for(unsigned n=1;n<=4;++n) {
        reset(); registers[0xe0][0xee]=1; fail_at=n;
        assert(d_flash_end(io,0)==-EIO && calls==4);
        assert(registers[0xe0][0xee]==(n==4));
    }
    reset(); fail_at=1; fail_at2=2;
    assert(d_flash_read(io,0x8000,actual.data(),actual.size())==-EIO && !registers[0xe0][0xee]);
    reset(); corrupt_read=129;
    assert(d_flash_write_edid(io,edid.data(),edid.size())==-EIO && !erases);
    reset(); bad_program=true;
    assert(d_flash_write_edid(io,edid.data(),edid.size())==-EIO);
    reset();
    assert(d_flash_read(io,0,actual.data(),256)==-EINVAL && !calls);
    assert(d_flash_write_edid(io,edid.data(),128)==-EINVAL && !calls);
    std::printf("D Flash: %u read / %u write fault points; metadata preserved, MCU program untouched, cleanup and verification passed\n",read_calls,write_calls);
}
