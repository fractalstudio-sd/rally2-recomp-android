#include "tex2d.h"
#include "pack_util.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

namespace Pack {

bool Tex2D::Enabled() const { return Get().dumping() || Get().packing(); }

void Tex2D::InitPack()
{
	m_init = true;
	if (const char* s = getenv("M3_PACK_SCALE")) m_scale = atoi(s);
}

const std::vector<uint8_t>* Tex2D::Tile(uint64_t h)
{
	auto it = m_tiles.find(h);
	if (it != m_tiles.end()) return it->second.empty() ? nullptr : &it->second;
	std::vector<uint8_t> px; int w = 0, hh = 0;
	std::string path = Get().packDir + "/2d_tiles/tile_" + HashName(h) + ".png";
	if (FileExists(path) && LoadPNG(path, w, hh, px) && w == hh && w >= 8 && w % 8 == 0) {
		int s = w / 8;
		if (m_scale == 0) m_scale = s;
		if (s != m_scale) px.clear();               // inconsistent scale: ignore
	} else px.clear();
	auto& e = m_tiles[h]; e = std::move(px);
	return e.empty() ? nullptr : &e;
}

static bool AllClear(const uint32_t* p, int stride, int w, int h)
{
	for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (p[y * stride + x] >> 24) return false;
	return true;
}

GLuint Tex2D::Process(int li, const uint32_t* buf, float& aspect)
{
	aspect = 4.0f / 3.0f;
	if (!Enabled() || !buf) return 0;
	if (!m_init) InitPack();
	const int W = 496, H = 384, STR = 512;
	Layer& L = m_layer[li & 1];

	// cheap whole-layer hash (per row, skipping padding)
	uint64_t lh = 0xcbf29ce484222325ull;
	for (int y = 0; y < H; y++) lh = Hash64(buf + y * STR, W * 4, lh);
	if (L.valid && lh == L.lastHash) { aspect = L.aspect; return L.tex; }
	L.valid = true; L.lastHash = lh; L.tex = 0; L.aspect = aspect;

	Config& c = Get();
	if (c.dumping()) {
		if (m_dumpCount < 20000) {
			if (!AllClear(buf, STR, W, H) && !m_dumped[lh]) {
				m_dumped[lh] = true;
				std::vector<uint8_t> img(W * H * 4);
				for (int y = 0; y < H; y++) memcpy(&img[y * W * 4], buf + y * STR, W * 4);
				MakeDirs(c.dumpDir + "/2d_layers");
				SavePNG(c.dumpDir + "/2d_layers/layer_" + HashName(lh) + ".png", W, H, img.data());
				m_dumpCount++;
			}
			for (int ty = 0; ty < H / 8; ty++) for (int tx = 0; tx < W / 8; tx++) {
				uint32_t t[64];
				for (int y = 0; y < 8; y++) memcpy(t + y * 8, buf + (ty * 8 + y) * STR + tx * 8, 32);
				bool clear = true; for (int i = 0; i < 64; i++) if (t[i] >> 24) { clear = false; break; }
				if (clear) continue;
				uint64_t th = Hash64(t, sizeof t);
				if (m_dumped[th]) continue;
				m_dumped[th] = true;
				MakeDirs(c.dumpDir + "/2d_tiles");
				SavePNG(c.dumpDir + "/2d_tiles/tile_" + HashName(th) + ".png", 8, 8, (const uint8_t*)t);
				m_dumpCount++;
			}
		}
	}
	if (!c.packing()) return 0;

	// whole-layer replacement (any size, 4:3 or wide)
	std::vector<uint8_t> px; int w = 0, h = 0;
	std::string lp = c.packDir + "/2d_layers/layer_" + HashName(lh) + ".png";
	bool haveLayer = FileExists(lp) && LoadPNG(lp, w, h, px);
	if (!haveLayer) {
		// tile composition
		static std::vector<const std::vector<uint8_t>*> sel; sel.assign((W / 8) * (H / 8), nullptr);
		int hits = 0;
		for (int ty = 0; ty < H / 8; ty++) for (int tx = 0; tx < W / 8; tx++) {
			uint32_t t[64];
			for (int y = 0; y < 8; y++) memcpy(t + y * 8, buf + (ty * 8 + y) * STR + tx * 8, 32);
			bool clear = true; for (int i = 0; i < 64; i++) if (t[i] >> 24) { clear = false; break; }
			if (clear) continue;
			auto* r = Tile(Hash64(t, sizeof t));
			sel[ty * (W / 8) + tx] = r; if (r) hits++;
		}
		if (getenv("M3_PACKDBG")) fprintf(stderr, "[2d] layer %d hits=%d scale=%d\n", li, hits, m_scale);
		if (!hits || m_scale <= 0) return 0;
		int S = m_scale; w = W * S; h = H * S; px.assign((size_t)w * h * 4, 0);
		for (int ty = 0; ty < H / 8; ty++) for (int tx = 0; tx < W / 8; tx++) {
			auto* r = sel[ty * (W / 8) + tx];
			int ts = 8 * S;
			for (int y = 0; y < ts; y++) {
				uint8_t* d = &px[((size_t)(ty * ts + y) * w + tx * ts) * 4];
				if (r) memcpy(d, &(*r)[(size_t)y * ts * 4], ts * 4);
				else for (int x = 0; x < ts; x++)      // nearest upscale of the original pixels
					memcpy(d + x * 4, buf + (ty * 8 + y / S) * STR + tx * 8 + x / S, 4);
			}
		}
	} else if (w <= 0 || h <= 0) return 0;

	if (!L.tex) glGenTextures(1, &L.tex);
	glBindTexture(GL_TEXTURE_2D, L.tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	L.aspect = aspect = (float)w / (float)h;
	return L.tex;
}

}
