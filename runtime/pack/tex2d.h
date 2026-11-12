// 2D (tile generator) asset dump / replacement. Works on the composed 496x384 RGBA layer buffers:
//  - 2d_layers/layer_<hash>.png : whole layer (for static screens; any size/aspect replacement accepted)
//  - 2d_tiles/tile_<hash>.png   : 8x8 blocks on the tile grid (replacement must be 8S x 8S, same S everywhere)
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include <GL/glew.h>

namespace Pack {

class Tex2D {
public:
	bool Enabled() const;
	// buf: 496x384 RGBA pixels with row stride 512 pixels. Returns hi-res GL texture (0 = use original).
	// aspect (out): width/height of the returned texture's intended on-screen area (4:3 unless a wide layer matched).
	GLuint Process(int layer, const uint32_t* buf, float& aspect);
private:
	struct Layer { uint64_t lastHash = 0; GLuint tex = 0; float aspect = 4.0f / 3.0f; bool valid = false; int tw = 0, th = 0; };
	Layer m_layer[2];
	std::unordered_map<uint64_t, std::vector<uint8_t>> m_tiles;   // hash -> RGBA (8S x 8S); empty = none
	std::unordered_map<uint64_t, bool> m_dumped;
	std::unordered_map<uint64_t, int> m_layerImg;                 // layer hash -> (unused) cache of presence
	int m_scale = 0;
	int m_dumpCount = 0;
	bool m_init = false;
	void InitPack();
	const std::vector<uint8_t>* Tile(uint64_t h);
};

}
