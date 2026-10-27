#pragma once
#include "NxMatrix/Types.hpp"

#include <pu/Plutonium>

namespace NxMatrix::Ui {

// Decodes encoded image bytes (PNG/JPEG/WebP/GIF, whatever SDL_image
// supports) into a texture. Returns nullptr on failure; runs on the UI thread
// (the media callback already is marshalled there).
pu::sdl2::TextureHandle::Ref DecodeImageTexture(const ByteArray &Data);

} // namespace NxMatrix::Ui