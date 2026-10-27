#include "NxMatrix/Crypto/Random.hpp"

#include <switch.h>

#include <mutex>

namespace NxMatrix::Crypto {

namespace {

std::once_flag g_CsrngOnce;

void EnsureCsrng() {
    // FillRandom has no failure channel for callers, so a transient service
    // error must not leave us without entropy: retry until csrng is up.
    while(R_FAILED(csrngInitialize())) {
        svcSleepThread(100000000LL); // 100 ms
    }
}

} // namespace

void FillRandom(std::uint8_t *Out, std::size_t Size) {
    if(Out == nullptr || Size == 0) {
        return;
    }
    std::call_once(g_CsrngOnce, EnsureCsrng);
    while(R_FAILED(csrngGetRandomBytes(Out, Size))) {
        svcSleepThread(100000000LL);
    }
}

ByteArray RandomBytes(std::size_t Size) {
    ByteArray Bytes(Size);
    if(Size != 0) {
        FillRandom(Bytes.data(), Size);
    }
    return Bytes;
}

} // namespace NxMatrix::Crypto