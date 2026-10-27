// Emoji-aware rich text: Noto Color Emoji (CBDT bitmap strike) rasterized
// straight through FreeType and mixed with Plutonium's TTF output for the
// plain-text runs. When the romfs font is missing every entry point degrades
// to the plain-text path (no emoji rendering, no crash, no TODOs).
#include "NxMatrix/Ui/Elements/RichText.hpp"

#include "NxMatrix/Ui/Elements/TextWrap.hpp"

#include <pu/Plutonium>

#include <freetype/freetype.h>
#include <ft2build.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace NxMatrix::Ui {

namespace {

constexpr const char *EmojiFontPath = "romfs:/NotoColorEmoji.ttf";

struct EmojiState {
    FT_Library Library = nullptr;
    FT_Face Face = nullptr;
};

EmojiState g_Emoji;
std::once_flag g_EmojiOnce;

// Lazy, run-once font load (thread safe via call_once): FreeType library plus
// the bundled CBDT face - bitmap fonts expose fixed-size strikes only, so the
// single available strike is selected right here, once.
void EnsureEmojiFont() {
    std::call_once(g_EmojiOnce, [] {
        if(FT_Init_FreeType(&g_Emoji.Library) != 0) {
            g_Emoji.Library = nullptr;
            return;
        }
        if(FT_New_Face(g_Emoji.Library, EmojiFontPath, 0, &g_Emoji.Face) != 0) {
            g_Emoji.Face = nullptr;
            return;
        }
        if(g_Emoji.Face->num_fixed_sizes < 1 || FT_Select_Size(g_Emoji.Face, 0) != 0) {
            FT_Done_Face(g_Emoji.Face);
            g_Emoji.Face = nullptr;
        }
    });
}

struct CodePoint {
    std::uint32_t Cp = 0;
    std::size_t Size = 0;
};

// UTF-8 decode; malformed bytes pass through as single-byte code points so
// the piece byte slices always round-trip the original string exactly.
std::vector<CodePoint> DecodeUtf8(const std::string &Text) {
    std::vector<CodePoint> Out;
    const std::size_t N = Text.size();
    std::size_t I = 0;
    while(I < N) {
        const auto Lead = static_cast<unsigned char>(Text[I]);
        std::size_t Len = 1;
        std::uint32_t Cp = Lead;
        if((Lead & 0xE0) == 0xC0) {
            Len = 2;
            Cp = Lead & 0x1FU;
        } else if((Lead & 0xF0) == 0xE0) {
            Len = 3;
            Cp = Lead & 0x0FU;
        } else if((Lead & 0xF8) == 0xF0) {
            Len = 4;
            Cp = Lead & 0x07U;
        }
        if(Len > 1) {
            bool Valid = I + Len <= N;
            std::uint32_t Decoded = Cp;
            for(std::size_t K = 1; Valid && K < Len; K++) {
                const auto Cont = static_cast<unsigned char>(Text[I + K]);
                if((Cont & 0xC0) != 0x80) {
                    Valid = false;
                    break;
                }
                Decoded = (Decoded << 6) | (Cont & 0x3FU);
            }
            if(Valid) {
                Out.push_back({Decoded, Len});
                I += Len;
                continue;
            }
            // Malformed sequence: emit the lead byte as-is and resync.
            Cp = Lead;
            Len = 1;
        }
        Out.push_back({Cp, Len});
        I += Len;
    }
    return Out;
}

// Broad emoji ranges; a code point only becomes an emoji piece when the
// bundled font actually has a glyph for it, so stray symbols from these
// ranges fall back to the plain text run.
bool IsEmojiBase(std::uint32_t Cp) {
    return (Cp >= 0x1F000U && Cp <= 0x1FAFFU)  // emoticons, symbols, transport, supplemental
           || (Cp >= 0x2600U && Cp <= 0x27BFU) // misc symbols, dingbats
           || (Cp >= 0x2300U && Cp <= 0x23FFU) // clocks, media controls
           || (Cp >= 0x2B00U && Cp <= 0x2BFFU) // stars, arrows-ish symbols
           || Cp == 0x00A9U || Cp == 0x00AEU   // (c) (r)
           || Cp == 0x2122U || Cp == 0x2139U;  // tm, info
}

// Zero-width characters riding inside an emoji sequence (ZWJ, variation
// selector, flag tag characters). They add no glyph cell of their own.
bool IsEmojiJoiner(std::uint32_t Cp) {
    return Cp == 0x200DU || Cp == 0xFE0FU || (Cp >= 0xE0020U && Cp <= 0xE007FU);
}

bool HasEmojiGlyph(std::uint32_t Cp) {
    return g_Emoji.Face != nullptr && FT_Get_Char_Index(g_Emoji.Face, Cp) != 0;
}

enum class PieceKind {
    Text,
    Space,
    Emoji,
    Break,
};

struct Piece {
    PieceKind Kind = PieceKind::Text;
    std::string Bytes;                 // exact source slice (round-trips)
    std::vector<std::uint32_t> Glyphs; // emoji code points owning a glyph
    std::int32_t Width = 0;
    std::int32_t Height = 0;
};

// The emoji cell is one text line tall (and, by approximation, one cell wide
// per code point) - the same value WrapRichText and RenderRichText measure
// with, so wrapping and rendering always agree.
std::int32_t FontLineHeight(const std::string &FontName) {
    std::int32_t W = 0;
    std::int32_t H = 0;
    pu::ui::render::GetTextDimensions(FontName, "Mg", W, H);
    return H > 0 ? H : 22;
}

// Splits text into measurement/render units: plain text runs, single spaces,
// atomic emoji sequences (base glyph + joiners + further bases) and \n.
std::vector<Piece> BuildPieces(const std::string &FontName, const std::string &Text) {
    std::vector<Piece> Out;
    if(Text.empty() || g_Emoji.Face == nullptr) {
        return Out;
    }
    const auto Cps = DecodeUtf8(Text);
    const std::int32_t Cell = FontLineHeight(FontName);
    const std::int32_t SpaceW = pu::ui::render::GetTextWidth(FontName, " ");

    Piece TextRun;
    auto FlushText = [&]() {
        if(!TextRun.Bytes.empty()) {
            TextRun.Width = pu::ui::render::GetTextWidth(FontName, TextRun.Bytes);
            std::int32_t W = 0;
            pu::ui::render::GetTextDimensions(FontName, TextRun.Bytes, W, TextRun.Height);
            if(TextRun.Height <= 0) {
                TextRun.Height = Cell;
            }
            Out.push_back(std::move(TextRun));
            TextRun = Piece();
        }
    };

    std::size_t Off = 0;
    for(std::size_t I = 0; I < Cps.size(); I++) {
        const std::string Slice = Text.substr(Off, Cps[I].Size);
        Off += Cps[I].Size;
        const std::uint32_t Cp = Cps[I].Cp;

        if(Cp == '\n') {
            FlushText();
            Piece P;
            P.Kind = PieceKind::Break;
            P.Bytes = Slice;
            P.Height = Cell;
            Out.push_back(std::move(P));
            continue;
        }
        if(Cp == ' ') {
            FlushText();
            Piece P;
            P.Kind = PieceKind::Space;
            P.Bytes = Slice;
            P.Width = SpaceW;
            P.Height = Cell;
            Out.push_back(std::move(P));
            continue;
        }
        if(IsEmojiBase(Cp) && HasEmojiGlyph(Cp)) {
            FlushText();
            Piece P;
            P.Kind = PieceKind::Emoji;
            P.Bytes = Slice;
            P.Glyphs.push_back(Cp);
            // Consume the rest of the sequence: joiners (ZWJ, variation
            // selector, flag tags) plus further base glyphs. Multi-code point
            // sequences render as adjacent cells, never composed.
            while(I + 1 < Cps.size()) {
                const std::uint32_t Next = Cps[I + 1].Cp;
                const bool Joiner = IsEmojiJoiner(Next);
                const bool Base = IsEmojiBase(Next) && HasEmojiGlyph(Next);
                if(!Joiner && !Base) {
                    break;
                }
                I++;
                P.Bytes += Text.substr(Off, Cps[I].Size);
                Off += Cps[I].Size;
                if(Base) {
                    P.Glyphs.push_back(Next);
                }
            }
            P.Width = static_cast<std::int32_t>(P.Glyphs.size()) * Cell;
            P.Height = Cell;
            Out.push_back(std::move(P));
            continue;
        }
        TextRun.Bytes += Slice;
    }
    FlushText();
    return Out;
}

// Greedy word wrap over pieces. Emoji pieces are atomic (never split, never
// separated from their measurement), spaces are held back until the next word
// commits so they never pad a line end. MaxWidth <= 0 keeps everything on one
// line per \n.
std::vector<std::vector<Piece>> WrapPieces(const std::string &FontName, const std::vector<Piece> &Pieces, std::int32_t MaxWidth) {
    std::vector<std::vector<Piece>> Lines;
    std::vector<Piece> Cur;
    std::vector<Piece> PendingSpaces;
    std::int32_t CurW = 0;
    std::int32_t PendingW = 0;

    auto CloseLine = [&]() {
        Lines.push_back(std::move(Cur));
        Cur.clear();
        PendingSpaces.clear();
        PendingW = 0;
        CurW = 0;
    };

    // An overlong plain word is cut at UTF-8 code point boundaries; a single
    // code point wider than the whole line is taken as-is so the loop always
    // makes progress (same contract as WrapText).
    auto SplitWord = [&](const Piece &Word) {
        std::vector<Piece> Chunks;
        const auto Cps = DecodeUtf8(Word.Bytes);
        std::size_t Off = 0;
        std::string CurChunk;
        for(const auto &C : Cps) {
            const std::string Slice = Word.Bytes.substr(Off, C.Size);
            Off += C.Size;
            const std::string Try = CurChunk + Slice;
            if(!CurChunk.empty() && pu::ui::render::GetTextWidth(FontName, Try) > MaxWidth) {
                Piece Chunk;
                Chunk.Kind = PieceKind::Text;
                Chunk.Bytes = CurChunk;
                Chunk.Width = pu::ui::render::GetTextWidth(FontName, Chunk.Bytes);
                Chunk.Height = Word.Height;
                Chunks.push_back(std::move(Chunk));
                CurChunk = Slice;
            } else {
                CurChunk = Try;
            }
        }
        if(!CurChunk.empty()) {
            Piece Chunk;
            Chunk.Kind = PieceKind::Text;
            Chunk.Bytes = CurChunk;
            Chunk.Width = pu::ui::render::GetTextWidth(FontName, Chunk.Bytes);
            Chunk.Height = Word.Height;
            Chunks.push_back(std::move(Chunk));
        }
        return Chunks;
    };

    for(const auto &P : Pieces) {
        if(P.Kind == PieceKind::Break) {
            CloseLine();
            continue;
        }
        if(P.Kind == PieceKind::Space) {
            PendingSpaces.push_back(P);
            PendingW += P.Width;
            continue;
        }
        // Word piece (plain text run or atomic emoji run).
        if(MaxWidth > 0 && !Cur.empty() && CurW + PendingW + P.Width > MaxWidth) {
            CloseLine(); // drops the held-back spaces
        }
        if(!Cur.empty()) {
            for(const auto &S : PendingSpaces) {
                Cur.push_back(S);
            }
            CurW += PendingW;
        }
        PendingSpaces.clear();
        PendingW = 0;

        Piece Word = P;
        if(MaxWidth > 0 && Cur.empty() && Word.Kind == PieceKind::Text && Word.Width > MaxWidth) {
            auto Chunks = SplitWord(Word);
            for(std::size_t K = 0; K + 1 < Chunks.size(); K++) {
                Cur.push_back(Chunks[K]);
                CloseLine();
            }
            Word = Chunks.back();
        }
        CurW += Word.Width;
        Cur.push_back(std::move(Word));
    }
    Lines.push_back(std::move(Cur));
    return Lines;
}

// Rasterizes one emoji code point from the CBDT strike into a Cell x Cell
// straight-alpha texture (bilinear downscale, centered, aspect preserved).
pu::sdl2::Texture MakeEmojiCell(std::uint32_t Cp, std::int32_t Cell) {
    if(g_Emoji.Face == nullptr || Cell <= 0) {
        return nullptr;
    }
    const FT_UInt GlyphIndex = FT_Get_Char_Index(g_Emoji.Face, Cp);
    if(GlyphIndex == 0) {
        return nullptr;
    }
    if(FT_Load_Glyph(g_Emoji.Face, GlyphIndex, FT_LOAD_COLOR) != 0) {
        return nullptr;
    }
    const FT_Bitmap &Bm = g_Emoji.Face->glyph->bitmap;
    if(Bm.pixel_mode != FT_PIXEL_MODE_BGRA || Bm.buffer == nullptr || Bm.pitch <= 0 || Bm.width == 0 || Bm.rows == 0) {
        return nullptr;
    }
    // FreeType hands CBDT bitmaps over as BGRA bytes, which is exactly SDL's
    // ARGB8888 byte order on little-endian targets (Switch and host alike).
    SDL_Surface *Source = SDL_CreateRGBSurfaceWithFormatFrom(Bm.buffer, static_cast<int>(Bm.width), static_cast<int>(Bm.rows), 32, Bm.pitch,
                                                             SDL_PIXELFORMAT_ARGB8888);
    if(Source == nullptr) {
        return nullptr;
    }
    SDL_Surface *CellSurface = SDL_CreateRGBSurfaceWithFormat(0, Cell, Cell, 32, SDL_PIXELFORMAT_ARGB8888);
    if(CellSurface == nullptr) {
        SDL_FreeSurface(Source);
        return nullptr;
    }
    SDL_FillRect(CellSurface, nullptr, SDL_MapRGBA(CellSurface->format, 0, 0, 0, 0));
    const std::int32_t MaxSide = std::max(static_cast<std::int32_t>(Bm.width), static_cast<std::int32_t>(Bm.rows));
    std::int32_t DstW = (static_cast<std::int32_t>(Bm.width) * Cell + MaxSide / 2) / MaxSide;
    std::int32_t DstH = (static_cast<std::int32_t>(Bm.rows) * Cell + MaxSide / 2) / MaxSide;
    DstW = std::max(1, DstW);
    DstH = std::max(1, DstH);
    SDL_Rect Dst{static_cast<int>((Cell - DstW) / 2), static_cast<int>((Cell - DstH) / 2), static_cast<int>(DstW), static_cast<int>(DstH)};
    const SDL_Rect FullSrc{0, 0, static_cast<int>(Bm.width), static_cast<int>(Bm.rows)};
    // Bilinear where available (the strike is far larger than a text line),
    // nearest-neighbor blit as a fallback.
    if(SDL_SoftStretchLinear(Source, &FullSrc, CellSurface, &Dst) != 0) {
        SDL_BlitScaled(Source, nullptr, CellSurface, &Dst);
    }
    SDL_FreeSurface(Source);
    return pu::ui::render::ConvertToTexture(CellSurface); // frees CellSurface
}

} // namespace

bool HasEmojiFont() {
    EnsureEmojiFont();
    return g_Emoji.Face != nullptr;
}

pu::sdl2::Texture RenderRichText(const std::string &FontName, const std::string &Text, const pu::ui::Color &Color, std::int32_t MaxWidth) {
    if(Text.empty()) {
        return nullptr;
    }
    EnsureEmojiFont();
    if(g_Emoji.Face == nullptr) {
        // Font missing: plain Plutonium rendering of the whole string (in
        // that mode WrapRichText handed us already-wrapped lines).
        return pu::ui::render::RenderText(FontName, Text, Color);
    }
    const auto Lines = WrapPieces(FontName, BuildPieces(FontName, Text), MaxWidth);
    SDL_Renderer *Renderer = pu::ui::render::GetMainRenderer();
    if(Renderer == nullptr) {
        return nullptr;
    }

    const std::int32_t FallbackH = FontLineHeight(FontName);
    std::int32_t TotalW = 0;
    std::int32_t TotalH = 0;
    for(const auto &Line : Lines) {
        std::int32_t W = 0;
        std::int32_t H = FallbackH;
        for(const auto &P : Line) {
            W += P.Width;
            H = std::max(H, P.Height);
        }
        TotalW = std::max(TotalW, W);
        TotalH += H;
    }
    if(TotalW <= 0 || TotalH <= 0) {
        return nullptr;
    }

    SDL_Texture *Target = SDL_CreateTexture(Renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, TotalW, TotalH);
    if(Target == nullptr) {
        return nullptr;
    }
    SDL_Texture *Previous = SDL_GetRenderTarget(Renderer);
    if(SDL_SetRenderTarget(Renderer, Target) != 0) {
        SDL_DestroyTexture(Target);
        return nullptr;
    }
    SDL_SetRenderDrawColor(Renderer, 0, 0, 0, 0);
    SDL_RenderClear(Renderer);

    std::int32_t LineY = 0;
    for(const auto &Line : Lines) {
        std::int32_t LineH = FallbackH;
        for(const auto &P : Line) {
            LineH = std::max(LineH, P.Height);
        }
        std::int32_t X = 0;
        for(const auto &P : Line) {
            if(P.Kind == PieceKind::Break) {
                continue;
            }
            if(P.Kind == PieceKind::Space) {
                X += P.Width;
                continue;
            }
            if(P.Kind == PieceKind::Emoji) {
                const std::int32_t Cell = P.Height;
                for(std::size_t K = 0; K < P.Glyphs.size(); K++) {
                    pu::sdl2::Texture CellTex = MakeEmojiCell(P.Glyphs[K], Cell);
                    if(CellTex != nullptr) {
                        const SDL_Rect Dst{static_cast<int>(X + static_cast<std::int32_t>(K) * Cell),
                                           static_cast<int>(LineY + (LineH - Cell) / 2), static_cast<int>(Cell), static_cast<int>(Cell)};
                        SDL_RenderCopy(Renderer, CellTex, nullptr, &Dst);
                        SDL_DestroyTexture(CellTex);
                    }
                }
                X += P.Width;
                continue;
            }
            pu::sdl2::Texture Tex = pu::ui::render::RenderText(FontName, P.Bytes, Color);
            if(Tex != nullptr) {
                const SDL_Rect Dst{static_cast<int>(X), static_cast<int>(LineY + (LineH - pu::ui::render::GetTextureHeight(Tex)) / 2),
                                   static_cast<int>(pu::ui::render::GetTextureWidth(Tex)),
                                   static_cast<int>(pu::ui::render::GetTextureHeight(Tex))};
                SDL_RenderCopy(Renderer, Tex, nullptr, &Dst);
                pu::ui::render::DeleteTexture(Tex);
            }
            X += P.Width;
        }
        LineY += LineH;
    }

    SDL_SetRenderTarget(Renderer, Previous);
    SDL_SetTextureBlendMode(Target, SDL_BLENDMODE_BLEND);
    return Target;
}

std::vector<std::string> WrapRichText(const std::string &FontName, const std::string &Text, std::int32_t MaxWidth) {
    std::vector<std::string> Out;
    if(Text.empty()) {
        return Out;
    }
    EnsureEmojiFont();
    if(g_Emoji.Face == nullptr) {
        return WrapText(FontName, Text, MaxWidth);
    }
    const auto Lines = WrapPieces(FontName, BuildPieces(FontName, Text), MaxWidth);
    Out.reserve(Lines.size());
    for(const auto &Line : Lines) {
        std::string Joined;
        for(const auto &P : Line) {
            Joined += P.Bytes;
        }
        Out.push_back(std::move(Joined));
    }
    return Out;
}

} // namespace NxMatrix::Ui