#pragma once
#include "Graphics/IRender3D.h"
class NullRender3D : public IRender3D {
public:
	void RenderFrame() override {} void BeginFrame() override {} void EndFrame() override {}
	void UploadTextures(unsigned, unsigned, unsigned, unsigned, unsigned) override {}
	void AttachMemory(const uint32_t *, const uint32_t *, const uint32_t *, const uint32_t *, const uint16_t *) override {}
	void SetStepping(int) override {}
	Result Init(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) override { return Result::OKAY; }
	void SetSunClamp(bool) override {} void SetBlockCulling(bool) override {}
	float GetLosValue(int) override { return 0; }
};
