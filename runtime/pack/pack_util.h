// Asset dump / replacement pack helpers (PNG I/O, hashing, config). Optional feature: everything is off unless a
// dump directory (M3_DUMP) or a pack directory (M3_PACK) is configured.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Pack {

struct Config {
	std::string dumpDir;       // M3_DUMP : write original assets here (3d/, 2d_tiles/, 2d_layers/)
	std::string packDir;       // M3_PACK : read replacement assets from here (same sub-folders)
	bool dumping() const { return !dumpDir.empty(); }
	bool packing() const { return !packDir.empty(); }
};
Config& Get();                 // reads the environment on first use

uint64_t Hash64(const void* data, size_t bytes, uint64_t seed = 0xcbf29ce484222325ull);   // FNV-1a
std::string HashName(uint64_t h);                                                         // 16 hex chars

bool SavePNG(const std::string& path, int w, int h, const uint8_t* rgba);
bool LoadPNG(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba);       // always RGBA8
bool FileExists(const std::string& path);
void MakeDirs(const std::string& path);

}
