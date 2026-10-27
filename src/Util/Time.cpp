#include "NxMatrix/Util/Time.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>

namespace NxMatrix::Util {

namespace {

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's
// algorithm): keeps parsing/formatting independent of locale and timegm.
std::int64_t DaysFromCivil(int Year, unsigned Month, unsigned Day) {
    Year -= (Month <= 2) ? 1 : 0;
    const std::int64_t Era = (Year >= 0 ? Year : Year - 399) / 400;
    const unsigned YearOfEra = static_cast<unsigned>(Year - Era * 400);
    const unsigned DayOfYear = (153 * (Month + (Month > 2 ? -3 : 9)) + 2) / 5 + Day - 1;
    const unsigned DayOfEra = YearOfEra * 365 + YearOfEra / 4 - YearOfEra / 100 + DayOfYear;
    return Era * 146097 + static_cast<std::int64_t>(DayOfEra) - 719468;
}

bool IsLeapYear(int Year) {
    return (Year % 4 == 0 && Year % 100 != 0) || Year % 400 == 0;
}

unsigned DaysInMonth(int Year, unsigned Month) {
    static const unsigned Days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(Month == 2 && IsLeapYear(Year)) {
        return 29;
    }
    return Days[Month - 1];
}

bool ParseDigits(const std::string &Text, std::size_t Pos, std::size_t Count, int &Out) {
    if(Pos + Count > Text.size()) {
        return false;
    }
    int Value = 0;
    for(std::size_t I = 0; I < Count; ++I) {
        const char C = Text[Pos + I];
        if(C < '0' || C > '9') {
            return false;
        }
        Value = Value * 10 + (C - '0');
    }
    Out = Value;
    return true;
}

void LocalTimeOf(std::time_t EpochSec, std::tm &Out) {
    localtime_r(&EpochSec, &Out);
}

std::int64_t CivilDayNumber(const std::tm &Tm) {
    return DaysFromCivil(Tm.tm_year + 1900, static_cast<unsigned>(Tm.tm_mon + 1), static_cast<unsigned>(Tm.tm_mday));
}

} // namespace

std::int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t EpochMsFromIso8601(const std::string &Iso8601) {
    // "YYYY-MM-DDTHH:MM[:SS[.fff]][Z]" — seconds, fraction and Z optional.
    int Year = 0;
    int Month = 0;
    int Day = 0;
    if(!ParseDigits(Iso8601, 0, 4, Year) || Iso8601.size() < 10 || Iso8601[4] != '-' || !ParseDigits(Iso8601, 5, 2, Month) ||
       Iso8601[7] != '-' || !ParseDigits(Iso8601, 8, 2, Day)) {
        return 0;
    }
    if(Iso8601.size() < 16) {
        return 0;
    }
    const char Sep = Iso8601[10];
    if(Sep != 'T' && Sep != 't' && Sep != ' ') {
        return 0;
    }
    int Hour = 0;
    int Minute = 0;
    if(Iso8601[13] != ':' || !ParseDigits(Iso8601, 11, 2, Hour) || !ParseDigits(Iso8601, 14, 2, Minute)) {
        return 0;
    }

    std::size_t Pos = 16;
    int Second = 0;
    if(Pos < Iso8601.size() && Iso8601[Pos] == ':') {
        if(!ParseDigits(Iso8601, Pos + 1, 2, Second)) {
            return 0;
        }
        Pos += 3;
    }

    int Millis = 0;
    if(Pos < Iso8601.size() && Iso8601[Pos] == '.') {
        ++Pos;
        const std::size_t FracBegin = Pos;
        int Digits = 0;
        int Fraction = 0;
        while(Pos < Iso8601.size() && Iso8601[Pos] >= '0' && Iso8601[Pos] <= '9') {
            if(Digits < 3) {
                Fraction = Fraction * 10 + (Iso8601[Pos] - '0');
                ++Digits;
            }
            ++Pos;
        }
        if(Pos == FracBegin) {
            return 0; // '.' not followed by digits
        }
        while(Digits < 3) {
            Fraction *= 10;
            ++Digits;
        }
        Millis = Fraction;
    }

    if(Pos < Iso8601.size()) {
        if(Iso8601[Pos] != 'Z' && Iso8601[Pos] != 'z') {
            return 0; // offsets/stray text not accepted
        }
        ++Pos;
        if(Pos != Iso8601.size()) {
            return 0;
        }
    }

    if(Month < 1 || Month > 12 || Day < 1 || static_cast<unsigned>(Day) > DaysInMonth(Year, static_cast<unsigned>(Month))) {
        return 0;
    }
    if(Hour > 23 || Minute > 59 || Second > 60) {
        return 0;
    }

    const std::int64_t Days = DaysFromCivil(Year, static_cast<unsigned>(Month), static_cast<unsigned>(Day));
    return ((Days * 86400 + Hour * 3600 + Minute * 60 + Second) * 1000) + Millis;
}

std::string FormatClock(std::int64_t EpochMs) {
    std::tm Tm{};
    LocalTimeOf(static_cast<std::time_t>(EpochMs / 1000), Tm);
    char Buffer[8];
    std::snprintf(Buffer, sizeof(Buffer), "%02d:%02d", Tm.tm_hour, Tm.tm_min);
    return Buffer;
}

std::string FormatDayLabel(std::int64_t EpochMs) {
    std::tm Then{};
    LocalTimeOf(static_cast<std::time_t>(EpochMs / 1000), Then);
    std::tm Today{};
    LocalTimeOf(static_cast<std::time_t>(NowMs() / 1000), Today);

    if(Then.tm_year == Today.tm_year && Then.tm_mon == Today.tm_mon && Then.tm_mday == Today.tm_mday) {
        return "Today";
    }
    if(CivilDayNumber(Then) == CivilDayNumber(Today) - 1) {
        return "Yesterday";
    }
    char Buffer[16];
    std::snprintf(Buffer, sizeof(Buffer), "%02d/%02d/%04d", Then.tm_mday, Then.tm_mon + 1, Then.tm_year + 1900);
    return Buffer;
}

std::string FormatDateTime(std::int64_t EpochMs) {
    std::tm Tm{};
    LocalTimeOf(static_cast<std::time_t>(EpochMs / 1000), Tm);
    char Buffer[32];
    std::snprintf(Buffer, sizeof(Buffer), "%02d/%02d/%04d %02d:%02d", Tm.tm_mday, Tm.tm_mon + 1, Tm.tm_year + 1900, Tm.tm_hour, Tm.tm_min);
    return Buffer;
}

std::string FormatAgo(std::int64_t DeltaMs) {
    char Buffer[32];
    if(DeltaMs < 1000) {
        return "just now";
    }
    if(DeltaMs < 60000) {
        std::snprintf(Buffer, sizeof(Buffer), "%llds ago", static_cast<long long>(DeltaMs / 1000));
        return Buffer;
    }
    if(DeltaMs < 3600000) {
        std::snprintf(Buffer, sizeof(Buffer), "%lldm ago", static_cast<long long>(DeltaMs / 60000));
        return Buffer;
    }
    if(DeltaMs < 86400000) {
        std::snprintf(Buffer, sizeof(Buffer), "%lldh ago", static_cast<long long>(DeltaMs / 3600000));
        return Buffer;
    }
    std::snprintf(Buffer, sizeof(Buffer), "%lldd ago", static_cast<long long>(DeltaMs / 86400000));
    return Buffer;
}

} // namespace NxMatrix::Util