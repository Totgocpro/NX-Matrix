// Shared decoder for media bytes (avatars, message previews).
#include "NxMatrix/Ui/Elements/ImageTexture.hpp"

namespace NxMatrix::Ui {

pu::sdl2::TextureHandle::Ref DecodeImageTexture(const ByteArray &Data) {
    if(Data.empty()) {
        return nullptr;
    }
    auto Rw = SDL_RWFromMem(const_cast<ByteArray::value_type *>(Data.data()), static_cast<int>(Data.size()));
    if(Rw == nullptr) {
        return nullptr;
    }
    // The 1 frees the RWops when SDL_image is done with it.
    auto Surface = IMG_Load_RW(Rw, 1);
    if(Surface == nullptr) {
        return nullptr;
    }
    const auto Texture = pu::ui::render::ConvertToTexture(Surface);
    if(Texture == nullptr) {
        return nullptr;
    }
    return pu::sdl2::TextureHandle::New(Texture);
}

} // namespace NxMatrix::Ui