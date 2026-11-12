// Headless harness stubs: no video, no audio device. The emulated hardware still runs completely (the CPU writes to the
// tile generator / Real3D memories as usual), only nothing is drawn or played.
#include "Supermodel.h"
#include "Graphics/Render2D.h"
#include "Graphics/IRender3D.h"
#include "Graphics/SuperAA.h"
#include "OSD/Audio.h"
#include "OSD/Video.h"

// --- 2D renderer
CRender2D::CRender2D(const Util::Config::Node &config) : m_config(config), m_vao(0) {}
CRender2D::~CRender2D() {}
Result CRender2D::Init(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, UpscaleMode) { return Result::OKAY; }
void CRender2D::Resize(unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned) {}
void CRender2D::BeginFrame() {} void CRender2D::PreRenderFrame() {} void CRender2D::RenderFrameBottom() {}
void CRender2D::RenderFrameTop() {} void CRender2D::EndFrame() {}
void CRender2D::WriteVRAM(unsigned, uint32_t) {} void CRender2D::AttachRegisters(const uint32_t *) {}
void CRender2D::AttachPalette(const uint32_t *[2]) {} void CRender2D::AttachVRAM(const uint8_t *) {}
void CRender2D::AttachDrawBuffers(std::shared_ptr<TileGenBuffer>, std::shared_ptr<TileGenBuffer>) {}
void SetRender2DHud(const Render2DHud &) {}

// --- audio
static AudioCallbackFPtr s_cb; static void *s_cbData;
void SetAudioCallback(AudioCallbackFPtr cb, void *d) { s_cb = cb; s_cbData = d; }
void SetAudioEnabled(bool) {}
int AudioUnderRuns() { return 0; }
void SetAudioType(Game::AudioTypes) {}
Result OpenAudio(const Util::Config::Node &) { return Result::OKAY; }
bool OutputAudio(unsigned, const float *, const float *, const float *, const float *, bool) { return false; }
void CloseAudio() {}
int AudioBufferedBytes() { return -1; }
void AudioWatchdog(bool) {}
int AudioOutPeak() { return 0; }
unsigned AudioCallbacks() { return 0; }
int AudioBytesPerFrame() { return 1; }

// --- video callbacks used by CModel3::RenderFrame
bool BeginFrameVideo() { return true; }   // the GPU begin/end-of-frame bookkeeping runs (the game polls its status), the null renderers draw nothing
void EndFrameVideo() {}
#include "Graphics/New3D/GLSLShader.h"
GLSLShader::GLSLShader() {}
GLSLShader::~GLSLShader() {}
void SuperAA::Draw() {}
