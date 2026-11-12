// Headless lockstep harness (maintainers only): runs the full Model 3 board (CPU via the selected guest backend, sound board,
// drive board, timers, IRQs) with scripted inputs and no video/audio device, and prints a hash of the main RAM and of the
// CPU registers at fixed frames. Two builds (reference lifted C vs native translator) must print identical lines.
// usage: m3_headless <images_dir> <frames> [every] [script]   script: "frame:KEY:len,..." KEY = COIN START UP DOWN LEFT RIGHT A S
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include "Supermodel.h"
#include "Game.h"
#include "ROMSet.h"
#include "Model3/Model3.h"
#include "Graphics/Render2D.h"
#include "Graphics/SuperAA.h"
#include "Inputs/Inputs.h"
#include "Inputs/InputSystem.h"
#include "Util/NewConfig.h"
#include "OSD/Logger.h"
#include "null3d.h"
#include "BlockFile.h"
extern "C" {
#include "ppc_rt.h"
CPU *ppc_cpu_state(void);
}
#ifdef XL_NATIVE
extern "C" int xl_init(const char *imagesDir, uint8_t *ram, const char *layout);
#endif

bool LoadRomImages(ROMSet &rs, const std::string &dir);
Game MakeSrally2();


static const char *kKeys[] = {"1", "3", "UP", "DOWN", "LEFT", "RIGHT", "A", "S", "5", "6", "Q", "W", "E", "R", "T", "ESCAPE", "F3", "7", "8"};
static const char *kScriptNames[] = {"START", "COIN", "UP", "DOWN", "LEFT", "RIGHT", "A", "S"};
static int g_frame = 0;
extern int g_hlFrame;   // RTC72421.cpp (headless): the clock follows the emulated frames
struct Press { int frame, key, len; };
static std::vector<Press> g_script;

class ScriptInputSystem : public CInputSystem {
public:
	ScriptInputSystem() : CInputSystem("Script") {}
	bool InitializeSystem() override { return true; }
	int GetKeyIndex(const char *n) override { for (int i = 0; i < (int)(sizeof kKeys / sizeof *kKeys); i++) if (!strcasecmp(n, kKeys[i])) return i; return -1; }
	const char *GetKeyName(int i) override { return i >= 0 && i < (int)(sizeof kKeys / sizeof *kKeys) ? kKeys[i] : nullptr; }
	bool IsKeyPressed(int, int k) const override { for (auto &p : g_script) if (p.key == k && g_frame >= p.frame && g_frame < p.frame + p.len) return true; return false; }
	int GetMouseAxisValue(int, int) const override { return 0; }
	int GetMouseWheelDir(int) const override { return 0; }
	bool IsMouseButPressed(int, int) const override { return false; }
	int GetJoyAxisValue(int, int) const override { return 0; }
	bool IsJoyPOVInDir(int, int, int) const override { return false; }
	bool IsJoyButPressed(int, int) const override { return false; }
	bool ProcessForceFeedbackCmd(int, int, ForceFeedbackCmd) override { return false; }
	int GetNumKeyboards() const override { return 1; }
	int GetNumMice() const override { return 0; }
	int GetNumJoysticks() const override { return 0; }
	const KeyDetails *GetKeyDetails(int) override { return nullptr; }
	const MouseDetails *GetMouseDetails(int) override { return nullptr; }
	const JoyDetails *GetJoyDetails(int) override { return nullptr; }
	bool Poll() override { return true; }
	void SetMouseVisibility(bool) override {}
};

static uint64_t fnv(uint64_t h, uint32_t v) { for (int i = 0; i < 4; i++) { h ^= (v >> (8 * i)) & 0xFF; h *= 1099511628211ull; } return h; }

int main(int argc, char **argv)
{
	if (argc < 3) { fprintf(stderr, "usage: %s <images_dir> <frames> [every] [script]\n", argv[0]); return 2; }
	int frames = atoi(argv[2]), every = argc > 3 ? atoi(argv[3]) : 60;
	if (argc > 4) {
		const char *q = argv[4];
		while (*q) {
			int f = 0, len = 0; char name[16] = {0};
			if (sscanf(q, "%d:%15[A-Z]:%d", &f, name, &len) == 3)
				for (int k = 0; k < 8; k++) if (!strcmp(name, kScriptNames[k])) g_script.push_back({f, k, len});
			const char *c = strchr(q, ','); if (!c) break; q = c + 1;
		}
	}
	SetLogger(std::make_shared<CConsoleErrorLogger>());
	Util::Config::Node cfg("Global");
	cfg.Set("EmulateSound", true); cfg.Set("EmulateDSB", true); cfg.Set("FlipStereo", false);
	cfg.Set("MusicVolume", 100); cfg.Set("SoundVolume", 100); cfg.Set("SimulateNet", false);
	cfg.Set("GPUMultiThreaded", false); cfg.Set("MultiThreaded", getenv("HL_MT") != nullptr);
	cfg.Set("LegacySoundDSP", false);
	cfg.Set("NoWhiteFlash", false); cfg.Set("QuadRendering", false); cfg.Set("WideScreen", false); cfg.Set("WideBackground", false);
	cfg.Set("PowerPCFrequency", 166u);
	cfg.Set("Balance", 0.0f); cfg.Set("BalanceLeftRight", 0.0f); cfg.Set("BalanceFrontRear", 0.0f); cfg.Set("NbSoundChannels", 2);
	cfg.Set("SoundFreq", 60.0f);
	cfg.Set("InputDigitalDecaySpeed", 50u); cfg.Set("InputDigitalSensitivity", 50u);
	cfg.Set("ForceFeedback", false);
	cfg.Set("SDLConstForceMax", 100u); cfg.Set("SDLConstForceThreshold", 30u); cfg.Set("SDLFrictionMax", 100u);
	cfg.Set("SDLSelfCenterMax", 100u); cfg.Set("SDLVibrateMax", 100u);

	ROMSet rs;
	if (!LoadRomImages(rs, argv[1])) { fprintf(stderr, "cannot load images from %s\n", argv[1]); return 1; }
	Game game = MakeSrally2();
	CModel3 m3(cfg);
	if (m3.Init() != Result::OKAY) return 1;
	if (m3.LoadGame(game, rs) != Result::OKAY) return 1;
	rs = ROMSet();
	std::shared_ptr<CInputSystem> isys = std::make_shared<ScriptInputSystem>();
	CInputs inputs(isys);
	if (!inputs.Initialize()) { fprintf(stderr, "inputs init failed\n"); return 1; }
	static const char *defaults[][2] = {
		{"InputStart1", "KEY_1"}, {"InputCoin1", "KEY_3"},
		{"InputServiceA", "KEY_5"}, {"InputServiceB", "KEY_7"}, {"InputTestA", "KEY_6"}, {"InputTestB", "KEY_8"},
		{"InputSteeringLeft", "KEY_LEFT"}, {"InputSteeringRight", "KEY_RIGHT"}, {"InputSteering", "NONE"},
		{"InputAccelerator", "KEY_UP"}, {"InputBrake", "KEY_DOWN"},
		{"InputGearShift1", "KEY_Q"}, {"InputGearShift2", "KEY_W"},
		{"InputGearShift3", "KEY_E"}, {"InputGearShift4", "KEY_R"}, {"InputGearShiftN", "KEY_T"},
		{"InputViewChange", "KEY_A"}, {"InputHandBrake", "KEY_S"},
		{"InputUIExit", "KEY_ESCAPE"}, {"InputUIReset", "KEY_F3"},
	};
	for (auto &d : defaults) cfg.Set(d[0], std::string(d[1]));
	inputs.LoadFromConfig(cfg);
	m3.AttachInputs(&inputs);
	CRender2D r2(cfg); NullRender3D r3;
	m3.AttachRenderers(&r2, &r3, nullptr);
#ifdef XL_NATIVE
	if (!xl_init(argv[1], m3.GetRAM(), getenv("HL_LAYOUT"))) { fprintf(stderr, "translator init failed\n"); return 1; }
#endif
	if (const char *nvp = getenv("HL_NVRAM")) { CBlockFile nv; if (nv.Load(nvp) == Result::OKAY) { m3.LoadNVRAM(&nv); nv.Close(); fprintf(stderr, "NVRAM loaded\n"); } }
	m3.Reset();
	for (g_frame = 0; g_frame < frames; g_frame++) {
		inputs.Poll(&game, 0, 0, 496, 384);
		if (getenv("HL_DBG") && g_frame <= 3) fprintf(stderr, "f%d coin %d start %d steer %d gas %d brake %d\n", g_frame, (int)inputs.coin[0]->value, (int)inputs.start[0]->value, (int)inputs.steering->value, (int)inputs.accelerator->value, (int)inputs.brake->value);
		if (g_frame + 1 >= 30 && (g_frame + 1) % 10 == 0) {   // same as the player: hidden-car bookkeeping counters kept above their threshold
			const UINT32 B = 0x4b2180;
			if (m3.Read32(B + 0x34) < 40) m3.Write32(B + 0x34, 40);
			if (m3.Read32(B + 0x40) < 1500) m3.Write32(B + 0x40, 1500);
		}
		if (getenv("HL_FR")) fprintf(stderr, "[fr] %d\n", g_frame);
		g_hlFrame = g_frame;
		m3.RunFrame();
		if (getenv("HL_DUMP") && atoi(getenv("HL_DUMP")) == g_frame + 1) { FILE *o = fopen(getenv("HL_DUMPFILE") ? getenv("HL_DUMPFILE") : "/tmp/hl_ram.bin", "wb"); for (uint32_t a = 0; a < 0x800000; a += 4) { uint32_t v = m3.Read32(a); fwrite(&v, 4, 1, o); } fwrite(ppc_cpu_state(), sizeof(CPU), 1, o); fclose(o); }
		if ((g_frame + 1) % every == 0) {
			uint64_t h = 1469598103934665603ull;
			for (uint32_t a = 0; a < 0x800000; a += 4) h = fnv(h, m3.Read32(a));
			printf("frame %d ram %016llx state %x fc %u\n", g_frame + 1, (unsigned long long)h, m3.Read32(0x22435C), m3.Read32(0x4c218c));
			fflush(stdout);
		}
	}
	return 0;
}
