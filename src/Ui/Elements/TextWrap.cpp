// Greedy word wrap on Plutonium font metrics: \n forces a break, words that
// cannot fit alone are split per UTF-8 code point.
#include "NxMatrix/Ui/Elements/TextWrap.hpp"

#include <pu/Plutonium>

namespace NxMatrix::Ui {

namespace {

std::int32_t TextWidth(const std::string &FontName, const std::string &Text) {
    return pu::ui::render::GetTextWidth(FontName, Text);
}

// Wraps one paragraph (no \n inside). Multiple spaces collapse to one.
std::vector<std::string> WrapParagraph(const std::string &FontName, const std::string &Paragraph, std::int32_t MaxWidth) {
    std::vector<std::string> Lines;
    std::string Line;
    std::string Word;

    auto Fits = [&](const std::string &Candidate) { return TextWidth(FontName, Candidate) <= MaxWidth; };

    auto FlushWord = [&]() {
        while(!Word.empty()) {
            if(Line.empty()) {
                if(Fits(Word)) {
                    Line = Word;
                    Word.clear();
                    return;
                }
                // Overlong word on an empty line: take the longest prefix of
                // whole code points that fits and emit it as its own line.
                std::string Head;
                std::size_t Taken = 0;
                while(Taken < Word.size()) {
                    std::size_t Next = Taken + 1;
                    while(Next < Word.size() && (static_cast<unsigned char>(Word[Next]) & 0xC0) == 0x80) {
                        Next++;
                    }
                    const auto Candidate = Head + Word.substr(Taken, Next - Taken);
                    if(!Fits(Candidate)) {
                        break;
                    }
                    Head = Candidate;
                    Taken = Next;
                }
                if(Taken == 0) {
                    // Even one code point is too wide; take it anyway so the
                    // loop always makes progress.
                    std::size_t Next = 1;
                    while(Next < Word.size() && (static_cast<unsigned char>(Word[Next]) & 0xC0) == 0x80) {
                        Next++;
                    }
                    Taken = Next;
                    Head = Word.substr(0, Taken);
                }
                Lines.push_back(Head);
                Word.erase(0, Taken);
                continue;
            }
            if(Fits(Line + " " + Word)) {
                Line += " " + Word;
                Word.clear();
                return;
            }
            Lines.push_back(Line);
            Line.clear();
        }
    };

    for(const auto Ch : Paragraph) {
        if(Ch == ' ') {
            FlushWord();
        } else {
            Word += Ch;
        }
    }
    FlushWord();
    Lines.push_back(Line);
    return Lines;
}

} // namespace

std::vector<std::string> WrapText(const std::string &FontName, const std::string &Text, std::int32_t MaxWidth) {
    std::vector<std::string> Lines;
    if(Text.empty()) {
        return Lines;
    }
    if(MaxWidth <= 0) {
        Lines.push_back(Text);
        return Lines;
    }

    // \n breaks are honored before the greedy pass over each paragraph.
    std::string Paragraph;
    auto FlushParagraph = [&]() {
        if(Paragraph.empty()) {
            Lines.push_back("");
        } else {
            const auto Wrapped = WrapParagraph(FontName, Paragraph, MaxWidth);
            Lines.insert(Lines.end(), Wrapped.begin(), Wrapped.end());
        }
        Paragraph.clear();
    };
    for(const auto Ch : Text) {
        if(Ch == '\n') {
            FlushParagraph();
        } else {
            Paragraph += Ch;
        }
    }
    FlushParagraph();
    return Lines;
}

std::int32_t MeasureWrappedHeight(const std::string &FontName, const std::string &Text, std::int32_t MaxWidth) {
    std::int32_t Height = 0;
    for(const auto &Line : WrapText(FontName, Text, MaxWidth)) {
        std::int32_t W = 0;
        std::int32_t H = 0;
        pu::ui::render::GetTextDimensions(FontName, Line, W, H);
        Height += H;
    }
    return Height;
}

} // namespace NxMatrix::Ui