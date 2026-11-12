// 3D texture asset dump / replacement for the New3D renderer.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <GL/glew.h>

namespace Pack {

class Tex3D {
public:
	bool Enabled() const;                              // dumping or replacement active
	void AttachMemory(const uint16_t* textureRam);     // two banks of 2048x1024 16-bit words
	void Invalidate() { m_gen++; }                     // texture RAM level 0 changed
	// effective texture rectangle (after model offset); returns replacement GL texture or 0
	GLuint Lookup(int page, int x, int y, int w, int h, int format);
	GLuint Sampler(int wrapU, int wrapV);              // 0/1 = repeat, 2/3 = mirror (Mesh::TexWrapMode)

private:
	struct Entry { uint64_t hash = 0; uint32_t gen = 0xFFFFFFFFu; GLuint tex = 0; };
	const uint16_t* m_ram = nullptr;
	uint32_t m_gen = 0;
	std::unordered_map<uint64_t, Entry> m_cache;       // key: rect+format
	std::unordered_map<uint64_t, GLuint> m_tex;        // content hash -> texture (0 = no replacement)
	std::unordered_map<uint64_t, bool> m_dumped;
	GLuint m_samplers[4] = {0, 0, 0, 0};
	int m_dumpCount = 0;

	GLuint LoadReplacement(uint64_t hash);
	void Dump(uint64_t hash, int page, int x, int y, int w, int h, int format);
};

}
