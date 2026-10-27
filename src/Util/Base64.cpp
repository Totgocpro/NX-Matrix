#include "NxMatrix/Util/Base64.hpp"

#include <array>

namespace NxMatrix::Util {

namespace {

constexpr char kStandardAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char kUrlSafeAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

// Shared decode table: both alphabets map to the same 6-bit values ('-'/'+' and
// '_'/'/' differ only in two positions), everything else -1 (invalid).
const std::array<std::int8_t, 256> &DecodeTable() {
    static const std::array<std::int8_t, 256> Table = [] {
        std::array<std::int8_t, 256> T{};
        T.fill(-1);
        for(std::size_t I = 0; I < 64; ++I) {
            T[static_cast<unsigned char>(kStandardAlphabet[I])] = static_cast<std::int8_t>(I);
            T[static_cast<unsigned char>(kUrlSafeAlphabet[I])] = static_cast<std::int8_t>(I);
        }
        return T;
    }();
    return Table;
}

std::string Base64EncodeWith(const ByteArray &Data, const char *Alphabet, bool Pad) {
    std::string Out;
    Out.reserve(((Data.size() + 2) / 3) * 4);

    std::size_t I = 0;
    for(; I + 3 <= Data.size(); I += 3) {
        const std::uint32_t Group =
            (static_cast<std::uint32_t>(Data[I]) << 16) | (static_cast<std::uint32_t>(Data[I + 1]) << 8) | Data[I + 2];
        Out.push_back(Alphabet[(Group >> 18) & 0x3F]);
        Out.push_back(Alphabet[(Group >> 12) & 0x3F]);
        Out.push_back(Alphabet[(Group >> 6) & 0x3F]);
        Out.push_back(Alphabet[Group & 0x3F]);
    }

    const std::size_t Left = Data.size() - I;
    if(Left == 1) {
        const std::uint32_t Group = static_cast<std::uint32_t>(Data[I]) << 16;
        Out.push_back(Alphabet[(Group >> 18) & 0x3F]);
        Out.push_back(Alphabet[(Group >> 12) & 0x3F]);
        if(Pad) {
            Out.push_back('=');
            Out.push_back('=');
        }
    } else if(Left == 2) {
        const std::uint32_t Group = (static_cast<std::uint32_t>(Data[I]) << 16) | (static_cast<std::uint32_t>(Data[I + 1]) << 8);
        Out.push_back(Alphabet[(Group >> 18) & 0x3F]);
        Out.push_back(Alphabet[(Group >> 12) & 0x3F]);
        Out.push_back(Alphabet[(Group >> 6) & 0x3F]);
        if(Pad) {
            Out.push_back('=');
        }
    }

    return Out;
}

} // namespace

std::string Base64Encode(const ByteArray &Data) {
    return Base64EncodeWith(Data, kStandardAlphabet, true);
}

std::string Base64EncodeUrlSafe(const ByteArray &Data) {
    return Base64EncodeWith(Data, kUrlSafeAlphabet, false);
}

ByteArray Base64Decode(const std::string &Text) {
    ByteArray Out;
    Out.reserve(Text.size() / 4 * 3);

    const std::array<std::int8_t, 256> &Table = DecodeTable();
    std::uint32_t Accum = 0;
    std::size_t Bits = 0;
    bool SeenPad = false;

    for(const unsigned char C : Text) {
        if(C == '=') {
            SeenPad = true;
            continue;
        }
        if(SeenPad) {
            return {}; // data after padding
        }
        const std::int8_t Value = Table[C];
        if(Value < 0) {
            return {}; // character from neither alphabet
        }
        Accum = (Accum << 6) | static_cast<std::uint32_t>(Value);
        Bits += 6;
        if(Bits >= 8) {
            Bits -= 8;
            Out.push_back(static_cast<std::uint8_t>((Accum >> Bits) & 0xFF));
        }
    }

    if(Bits == 6) {
        return {}; // one leftover sextet: impossible group length
    }
    return Out;
}

} // namespace NxMatrix::Util