#include "tex3d.h"
#include "pack_util.h"
#include <vector>
#include <algorithm>
#include <cstdio>

namespace Pack {

bool Tex3D::Enabled() const { return Get().dumping() || Get().packing(); }

void Tex3D::AttachMemory(const uint16_t* ram) { m_ram = ram; }

// mirrors ExtractColour() in the fragment shader
static void Decode(int type, uint32_t v, uint8_t* o)
{
	float r = 0, g = 0, b = 0, a = 0;
	auto n4 = [](uint32_t x) { return float(x & 0xF) / 15.0f; };
	switch (type) {
	case 0: r = ((v >> 10) & 0x1F) / 31.0f; g = ((v >> 5) & 0x1F) / 31.0f; b = (v & 0x1F) / 31.0f; a = 1.0f - ((v >> 15) & 1); break;
	case 1: r = g = b = n4(v); a = n4(v >> 4); break;
	case 2: a = n4(v); r = g = b = n4(v >> 4); break;
	case 3: r = g = b = n4(v >> 8); a = n4(v >> 12); break;
	case 4: a = n4(v >> 8); r = g = b = n4(v >> 12); break;
	case 5: r = g = b = float(v & 0xFF) / 255.0f; a = (r == 1.0f) ? 0.0f : 1.0f; break;
	case 6: r = g = b = float((v >> 8) & 0xFF) / 255.0f; a = (r == 1.0f) ? 0.0f : 1.0f; break;
	case 7: r = n4(v >> 12); g = n4(v >> 8); b = n4(v >> 4); a = n4(v); break;
	case 8: r = g = b = n4(v); a = (r == 1.0f) ? 0.0f : 1.0f; break;
	case 9: r = g = b = n4(v >> 4); a = (r == 1.0f) ? 0.0f : 1.0f; break;
	case 10: r = g = b = n4(v >> 8); a = (r == 1.0f) ? 0.0f : 1.0f; break;
	case 11: r = g = b = n4(v >> 12); a = (r == 1.0f) ? 0.0f : 1.0f; break;
	}
	o[0] = (uint8_t)(r * 255.0f + 0.5f); o[1] = (uint8_t)(g * 255.0f + 0.5f);
	o[2] = (uint8_t)(b * 255.0f + 0.5f); o[3] = (uint8_t)(a * 255.0f + 0.5f);
}

GLuint Tex3D::Lookup(int page, int x, int y, int w, int h, int format)
{
	if (!m_ram || w <= 0 || h <= 0 || w > 2048 || h > 1024) return 0;

	uint64_t key = ((uint64_t)(page & 1) << 62) ^ ((uint64_t)(x & 2047) << 48) ^ ((uint64_t)(y & 1023) << 36) ^
	               ((uint64_t)w << 24) ^ ((uint64_t)h << 12) ^ (uint64_t)(format & 0xFF);
	Entry& e = m_cache[key];
	if (e.gen != m_gen) {                                   // (re)hash the level-0 rectangle
		const uint16_t* bank = m_ram + (size_t)(page & 1) * 2048 * 1024;
		uint64_t hsh = Hash64(&format, sizeof format);
		hsh = Hash64(&w, sizeof w, hsh); hsh = Hash64(&h, sizeof h, hsh);
		for (int j = 0; j < h; j++) {
			int yy = (y + j) & 1023;
			for (int i = 0; i < w; ) {                      // contiguous run (wrap at 2048)
				int xx = (x + i) & 2047, run = std::min(w - i, 2048 - xx);
				hsh = Hash64(bank + (size_t)yy * 2048 + xx, (size_t)run * 2, hsh);
				i += run;
			}
		}
		e.hash = hsh; e.gen = m_gen;
		if (Get().dumping() && !m_dumped[hsh]) { m_dumped[hsh] = true; Dump(hsh, page, x, y, w, h, format); }
		auto it = m_tex.find(hsh);
		e.tex = (it != m_tex.end()) ? it->second : (m_tex[hsh] = (Get().packing() ? LoadReplacement(hsh) : 0));
	}
	return e.tex;
}

void Tex3D::Dump(uint64_t hash, int page, int x, int y, int w, int h, int format)
{
	if (m_dumpCount > 40000) return;
	const uint16_t* bank = m_ram + (size_t)(page & 1) * 2048 * 1024;
	std::vector<uint8_t> rgba((size_t)w * h * 4);
	for (int j = 0; j < h; j++)
		for (int i = 0; i < w; i++)
			Decode(format, bank[(size_t)((y + j) & 1023) * 2048 + ((x + i) & 2047)], &rgba[((size_t)(h - 1 - j) * w + i) * 4]);   // stored upside down: dump upright
	std::string dir = Get().dumpDir + "/3d";
	MakeDirs(dir);
	SavePNG(dir + "/" + HashName(hash) + ".png", w, h, rgba.data());
	if (FILE* f = fopen((dir + "/index.csv").c_str(), "a")) {
		fprintf(f, "%s,%d,%d,%d,%d,%d,%d\n", HashName(hash).c_str(), page & 1, x, y, w, h, format);
		fclose(f);
	}
	m_dumpCount++;
}

GLuint Tex3D::LoadReplacement(uint64_t hash)
{
	std::string path = Get().packDir + "/3d/" + HashName(hash) + ".png";
	int w, h; std::vector<uint8_t> px;
	if (!LoadPNG(path, w, h, px)) return 0;
	for (int j = 0; j < h / 2; j++)                         // back to hardware row order
		std::swap_ranges(&px[(size_t)j * w * 4], &px[(size_t)(j + 1) * w * 4], &px[(size_t)(h - 1 - j) * w * 4]);
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	glGenerateMipmap(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

GLuint Tex3D::Sampler(int wu, int wv)
{
	int idx = ((wu >= 2) ? 2 : 0) | ((wv >= 2) ? 1 : 0);
	if (!m_samplers[idx]) {
		GLuint s; glGenSamplers(1, &s);
		glSamplerParameteri(s, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
		glSamplerParameteri(s, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glSamplerParameteri(s, GL_TEXTURE_WRAP_S, (idx & 2) ? GL_MIRRORED_REPEAT : GL_REPEAT);
		glSamplerParameteri(s, GL_TEXTURE_WRAP_T, (idx & 1) ? GL_MIRRORED_REPEAT : GL_REPEAT);
		glSamplerParameterf(s, GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.0f);
		m_samplers[idx] = s;
	}
	return m_samplers[idx];
}

}
