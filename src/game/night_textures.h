#pragma once
// Restores the game's night city textures.
//
// The data ships a night twin for every city atlas (atlasN.tga -> nightN.tga, alpha_atlasN.tga ->
// alpha_nightN.tga, X.tga -> X_night.tga; DXT DDS files despite the extension): dark, blue-tinted,
// windows and street lights lit. The game's code that swapped them in at night (CStreamTexture,
// CStreamTextureManager::Update on DayTime state 3) is dead in the shipped build: CStreamTextures
// are only created by CCustomResFactory, which only the offline buildBatchMesh tool installs. So
// the city stayed in daylight all night (also on phones).
//
// This module recognises the day textures when the game uploads them (content fingerprint of
// the compressed data) and re-uploads the night twin into the same GL texture while DayTime is in
// its night state, and the day image again afterwards; a few textures per frame, no hitch.
#include <filesystem>

namespace nighttex {

// Pairs the files in the data folder and starts watching texture uploads (GL context current).
bool init(const std::filesystem::path& data_dir);
// Once per frame, before the game draws: DayTime state (3 = night) and minute of the game day.
// Swaps night images in and out and sets the dawn/dusk tint; enabled=false restores the original.
void update(bool enabled, int state, int minute);

}  // namespace nighttex
