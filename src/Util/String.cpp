#include "NxMatrix/Util/String.hpp"

#include <cctype>
#include <cstdio>

namespace NxMatrix::Util {

std::string ToLower(const std::string &Text) {
    std::string Out = Text;
    for(char &C : Out) {
        C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
    }
    return Out;
}

std::string Trim(const std::string &Text) {
    std::size_t Begin = 0;
    std::size_t End = Text.size();
    while(Begin < End && std::isspace(static_cast<unsigned char>(Text[Begin])) != 0) {
        ++Begin;
    }
    while(End > Begin && std::isspace(static_cast<unsigned char>(Text[End - 1])) != 0) {
        --End;
    }
    return Text.substr(Begin, End - Begin);
}

bool StartsWith(const std::string &Text, const std::string &Prefix) {
    return Text.size() >= Prefix.size() && Text.compare(0, Prefix.size(), Prefix) == 0;
}

bool EndsWith(const std::string &Text, const std::string &Suffix) {
    return Text.size() >= Suffix.size() && Text.compare(Text.size() - Suffix.size(), Suffix.size(), Suffix) == 0;
}

std::vector<std::string> Split(const std::string &Text, char Separator) {
    std::vector<std::string> Parts;
    std::size_t Start = 0;
    while(true) {
        const std::size_t Pos = Text.find(Separator, Start);
        if(Pos == std::string::npos) {
            Parts.push_back(Text.substr(Start));
            break;
        }
        Parts.push_back(Text.substr(Start, Pos - Start));
        Start = Pos + 1;
    }
    return Parts;
}

std::string Join(const std::vector<std::string> &Parts, const std::string &Separator) {
    std::string Out;
    std::size_t Total = 0;
    for(const std::string &Part : Parts) {
        Total += Part.size();
    }
    Total += Separator.size() * (Parts.empty() ? 0 : Parts.size() - 1);
    Out.reserve(Total);
    for(std::size_t I = 0; I < Parts.size(); ++I) {
        if(I != 0) {
            Out += Separator;
        }
        Out += Parts[I];
    }
    return Out;
}

std::string ReplaceAll(std::string Text, const std::string &From, const std::string &To) {
    if(From.empty()) {
        return Text;
    }
    std::size_t Pos = 0;
    while((Pos = Text.find(From, Pos)) != std::string::npos) {
        Text.replace(Pos, From.size(), To);
        Pos += To.size();
    }
    return Text;
}

std::string UrlEncode(const std::string &Text) {
    static const char Hex[] = "0123456789ABCDEF";
    std::string Out;
    Out.reserve(Text.size());
    for(const unsigned char C : Text) {
        const bool Unreserved =
            (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-' || C == '.' || C == '_' || C == '~';
        if(Unreserved) {
            Out.push_back(static_cast<char>(C));
        } else {
            Out.push_back('%');
            Out.push_back(Hex[C >> 4]);
            Out.push_back(Hex[C & 0x0F]);
        }
    }
    return Out;
}

std::string TruncateWithEllipsis(const std::string &Text, std::size_t MaxChars) {
    // Walk codepoints: a lead byte carries its sequence length, so the cut
    // position always lands on a codepoint boundary.
    std::size_t Pos = 0;
    std::size_t Count = 0;
    while(Pos < Text.size() && Count < MaxChars) {
        const unsigned char Lead = static_cast<unsigned char>(Text[Pos]);
        std::size_t Len = 1;
        if((Lead & 0x80) != 0) {
            if((Lead & 0xE0) == 0xC0) {
                Len = 2;
            } else if((Lead & 0xF0) == 0xE0) {
                Len = 3;
            } else if((Lead & 0xF8) == 0xF0) {
                Len = 4;
            }
            if(Pos + Len > Text.size()) {
                Len = 1; // truncated tail bytes: count individually
            }
        }
        Pos += Len;
        ++Count;
    }

    if(Pos >= Text.size()) {
        return Text; // fits within MaxChars
    }
    return Text.substr(0, Pos) + "...";
}

} // namespace NxMatrix::Util