// Fills a ROMSet from the flat region images produced by tools/build_images.py. GPLv3.
#include <cstdio>
#include <cstring>
#include <string>
#include "ROMSet.h"
static bool load_region(ROMSet &rs, const std::string &dir, const char *name, bool required)
{
	std::string p = dir + "/" + name + ".bin";
	FILE *f = fopen(p.c_str(), "rb");
	if (!f) { if (required) fprintf(stderr, "missing %s\n", p.c_str()); return !required; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	ROM r; r.size = (size_t)n;
	r.data = std::shared_ptr<uint8_t>(new uint8_t[n], std::default_delete<uint8_t[]>());
	if (fread(r.data.get(), 1, n, f) != (size_t)n) { fclose(f); return false; }
	fclose(f);
	rs.rom_by_region[name] = r;
	return true;
}
bool LoadRomImages(ROMSet &rs, const std::string &dir)
{
	static const char *req[] = {"crom", "banked_crom", "vrom", "sound_program", "sound_samples", "mpeg_program", "mpeg_music"};
	for (auto n : req) if (!load_region(rs, dir, n, true)) return false;
	load_region(rs, dir, "driveboard_program", false);
	return true;
}
