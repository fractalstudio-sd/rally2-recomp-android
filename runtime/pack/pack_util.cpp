#include "pack_util.h"
#include <cstdlib>
#include <cstdio>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "Pkgs/stb_image.h"
#include "Pkgs/stb_image_write.h"

namespace Pack {

Config& Get()
{
	static Config c = [] {
		Config k;
		if (const char* d = getenv("M3_DUMP")) k.dumpDir = d;
		if (const char* p = getenv("M3_PACK")) k.packDir = p;
		return k;
	}();
	return c;
}

uint64_t Hash64(const void* data, size_t bytes, uint64_t h)
{
	const uint8_t* p = (const uint8_t*)data;
	for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
	return h;
}

std::string HashName(uint64_t h)
{
	char b[24]; snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
	return b;
}

bool FileExists(const std::string& path)
{
	struct stat st; return stat(path.c_str(), &st) == 0;
}

void MakeDirs(const std::string& path)
{
	std::string cur;
	for (size_t i = 0; i <= path.size(); i++) {
		if (i == path.size() || path[i] == '/') {
			if (!cur.empty()) {
#if defined(_WIN32)
				_mkdir(cur.c_str());
#else
				mkdir(cur.c_str(), 0755);
#endif
			}
		}
		if (i < path.size()) cur += path[i];
	}
}

bool SavePNG(const std::string& path, int w, int h, const uint8_t* rgba)
{
	return stbi_write_png(path.c_str(), w, h, 4, rgba, w * 4) != 0;
}

bool LoadPNG(const std::string& path, int& w, int& h, std::vector<uint8_t>& rgba)
{
	int n = 0;
	unsigned char* d = stbi_load(path.c_str(), &w, &h, &n, 4);
	if (!d) return false;
	rgba.assign(d, d + (size_t)w * h * 4);
	stbi_image_free(d);
	return true;
}

}
