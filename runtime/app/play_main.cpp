#include <unistd.h>
#include <sys/stat.h>
// Windowed player: native Model 3 hardware + recompiled guest, OpenGL video, SDL audio/input. GPLv3.
// usage: m3_play <images_dir> [options]
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <string>
#include <memory>
#include <cmath>
#include <GL/glew.h>
#include <vector>
#include <thread>
#include "Supermodel.h"
#include "Game.h"
#include "ROMSet.h"
#include "Model3/Model3.h"
#include "Graphics/IRender3D.h"
#include "Graphics/Render2D.h"
#include "Graphics/SuperAA.h"
#include "Graphics/New3D/New3D.h"
#include "Inputs/Inputs.h"
#include "OSD/SDLInputSystem.h"
#include "OSD/Audio.h"
#include "OSD/Outputs.h"
#include "Util/NewConfig.h"
#include "OSD/Logger.h"
#include "touch.h"
#include "pad.h"
#include "settings.h"
#include <vector>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifdef __ANDROID__
#include <dlfcn.h>
#include <unistd.h>
#include <sched.h>
static void *g_hint = nullptr;
static int (*g_hint_report)(void*, int64_t) = nullptr;
static int rd_khz(int cpu) { char p[96]; snprintf(p, sizeof p, "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu); FILE *f = fopen(p, "r"); if (!f) return -1; int v = -1; if (fscanf(f, "%d", &v) != 1) v = -1; fclose(f); return v; }
static void hint_init(int64_t target_ns)
{
	void *h = dlopen("libandroid.so", RTLD_NOW);
	if (!h) return;
	auto getm = (void *(*)())dlsym(h, "APerformanceHint_getManager");
	auto crt = (void *(*)(void *, const int *, size_t, int64_t))dlsym(h, "APerformanceHint_createSession");
	g_hint_report = (int (*)(void *, int64_t))dlsym(h, "APerformanceHint_reportActualWorkDuration");
	if (!getm || !crt || !g_hint_report) { fprintf(stderr, "[adpf] unavailable\n"); g_hint_report = nullptr; return; }
	void *m = getm();
	int tid = (int)gettid();
	if (m) g_hint = crt(m, &tid, 1, target_ns);
	fprintf(stderr, "[adpf] session %s\n", g_hint ? "ok" : "failed");
	if (!g_hint) g_hint_report = nullptr;
}
#endif

Game MakeSrally2();
void LeaderboardSetDir(const std::string &dir);
#ifndef M3_APP_TITLE
#define M3_APP_TITLE "Rally Recomp"   /* neutral name: no third-party trademarks in the app */
#endif
#ifdef XL_NATIVE
extern "C" int xl_init(const char *imagesDir, uint8_t *ram, const char *layout);
#endif
bool LoadRomImages(ROMSet &rs, const std::string &dir);

static SDL_Window *s_window;
static uint64_t s_freq;

bool BeginFrameVideo() { return true; }
static int g_dump_every = 0, g_vframe = 0;
static bool g_mtOn = false;
static GLuint s_fbo = 0, s_fbo_tex = 0, s_fbo_rb = 0; static int s_iw = 0, s_ih = 0;   // internal render target (fixed size)
bool g_prof_sync = false; static double s_ph[4] = {0,0,0,0};
double g_prof_upload = 0, g_prof_render = 0, g_prof_p[4] = {0,0,0,0};
static double s_swap_sum = 0, s_swap_max = 0; static double s_work_sum = 0, s_work_max = 0; static int s_work_n = 0; static uint64_t s_last_end = 0; static bool s_stats = false; static bool s_framelog = getenv("M3_FRAMELOG") != nullptr;
static void dump_frame()
{
	int w = s_iw, h = s_ih;
	std::unique_ptr<unsigned char[]> px(new unsigned char[w * h * 4]);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo);
	glPixelStorei(GL_PACK_ALIGNMENT, 1); glReadBuffer(GL_COLOR_ATTACHMENT0);
	glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.get());   // RGBA: the only combination GLES guarantees
	char fn[128]; snprintf(fn, sizeof fn, "gl_%05d.ppm", g_vframe);
	FILE *f = fopen(fn, "wb"); if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	std::vector<unsigned char> row((size_t)w * 3);
	for (int y = h - 1; y >= 0; y--) {
		const unsigned char *p = px.get() + (size_t)y * w * 4;
		for (int x = 0; x < w; x++) { row[x * 3] = p[x * 4]; row[x * 3 + 1] = p[x * 4 + 1]; row[x * 3 + 2] = p[x * 4 + 2]; }
		fwrite(row.data(), 1, row.size(), f);
	}
	fclose(f);
}
#ifdef __ANDROID__
#define AUDIO_TARGET_FRAMES 8.0
#else
#define AUDIO_TARGET_FRAMES 3.0
#endif
static uint64_t s_next, s_period;
static void sleep_until(uint64_t target);

// ---- dynamic internal resolution -------------------------------------------------------------------------------------
// The GPU of a phone differs from the next one by an order of magnitude, so a fixed render height is either too slow
// or needlessly soft. We watch how often the frame misses its 60 Hz deadline and step the internal height down (and,
// when there is clear headroom, back up). The presented image is always scaled to the window, so only sharpness changes.
static CRender2D *s_r2 = nullptr; static New3D::CNew3D *s_r3 = nullptr; static bool s_wideMode = true;
static int s_baseW = 0, s_baseH = 0, s_lv = 0, s_pendLv = -1; static bool s_adapt = false;
static const float kLv[] = { 1.0f, 0.85f, 0.72f, 0.60f, 0.50f };
static const int NLV = sizeof kLv / sizeof kLv[0];
static std::string s_autoPath;
static void LevelDims(int lv, int &w, int &h)
{
	h = (int)(s_baseH * kLv[lv] + 0.5f) & ~1; w = (int)((double)s_baseW * h / s_baseH + 0.5) & ~1;
}
static int s_settle = 0;
static void ApplyRes(int lv)
{
	int w, h; LevelDims(lv, w, h);
	s_lv = lv; s_iw = w; s_ih = h;
	glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
	glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_fbo_rb);
	unsigned totalX = w, totalY = h, xRes = w, yRes = h, xOff = 0, yOff = 0;
	float r = 496.0f / 384.0f, xf = (float)xRes, yf = (float)yRes;
	if (yf < xf / r) xf = yf * r;
	if (xf < yf * r) yf = xf / r;
	xOff = (xRes - (unsigned)xf) / 2; yOff = (yRes - (unsigned)yf) / 2; xRes = (unsigned)xf; yRes = (unsigned)yf;
	unsigned corr = (unsigned)(((yRes / 384.0) * 2.0) + 0.5);
	glViewport(0, 0, totalX, totalY);
	glDisable(GL_SCISSOR_TEST); glClearColor(0, 0, 0, 0); glClearDepth(1.0); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); glEnable(GL_SCISSOR_TEST);
	if (s_wideMode) glScissor(0, corr, totalX, totalY - corr * 2);
	else glScissor(xOff + corr, yOff + corr, xRes - corr * 2, yRes - corr * 2);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if (s_r2) s_r2->Resize(xOff, yOff, xRes, yRes, totalX, totalY, s_fbo);
	if (s_r3) s_r3->Init(xOff, yOff, xRes, yRes, totalX, totalY, s_fbo);
	s_settle = 45; s_next = SDL_GetPerformanceCounter() + s_period;   // forget the lag built up before the change
	fprintf(stderr, "[ires] level %d -> %dx%d\n", lv, w, h);
}
static void SaveAuto()
{
	if (s_autoPath.empty()) return;
	if (FILE *f = fopen(s_autoPath.c_str(), "w")) { fprintf(f, "%d %d\n", s_baseH, s_lv); fclose(f); }
}
// called once per frame with the lateness (ticks) of the frame against its deadline; negative = finished early
static void AdaptStep(int64_t late)
{
	if (!s_adapt || s_pendLv >= 0) return;
	static int n = 0, bad = 0; static double slack = 0; static int calm = 0; static uint64_t frame = 0, banUntil[NLV] = {0}, lastUp = 0; static int banLen[NLV] = {0};
	frame++;
	double lateMs = (double)late * 1000.0 / s_freq, perMs = 1000.0 * s_period / s_freq;
	if (s_settle > 0) { s_settle--; return; }
	if (frame < 120) return;                                   // start-up and first scene load
	n++; if (lateMs > 1.5) bad++; slack += -lateMs;
	if (n < 60) return;
	double meanSlack = slack / n; int b = bad; n = 0; bad = 0; slack = 0;
	if (b >= 12 && s_lv + 1 < NLV) {                          // a fifth of the frames late
		if (lastUp && frame - lastUp < 1800) { banLen[s_lv] = banLen[s_lv] ? banLen[s_lv] * 2 : 3600; banUntil[s_lv] = frame + banLen[s_lv]; }   // that step up did not hold: wait longer before retrying it
		s_pendLv = s_lv + 1; calm = 0; lastUp = 0; return;
	}
	if (b == 0 && meanSlack > perMs * 0.30 && s_lv > 0) {
		double a0 = kLv[s_lv], a1 = kLv[s_lv - 1];
		double cost = perMs - meanSlack, pred = cost * (a1 * a1) / (a0 * a0);
		if (pred < perMs * 0.78) calm++; else calm = 0;
		if (calm >= 8 && frame >= banUntil[s_lv - 1]) { s_pendLv = s_lv - 1; calm = 0; lastUp = frame; }
	} else if (b > 0) calm = 0;
}
void EndFrameVideo()
{
	// Present on a fixed deadline so frame times are even regardless of emulation load.
	// The period is nudged by the audio buffer fill so video and sound never drift apart.
	uint64_t t_in = SDL_GetPerformanceCounter();
#ifdef __ANDROID__
	if (g_hint && s_last_end) g_hint_report(g_hint, (int64_t)((double)(t_in - s_last_end) * 1e9 / s_freq));
	{ static int cn = 0; if (++cn == 345) { cn = 0; fprintf(stderr, "[cpu] on cpu%d, khz cpu0 %d cpu4 %d cpu7 %d\n", sched_getcpu(), rd_khz(0), rd_khz(4), rd_khz(7)); } }
#endif
	if (s_stats && s_last_end) {
		double ms = (double)(t_in - s_last_end) * 1000.0 / s_freq;
		s_work_sum += ms; if (ms > s_work_max) s_work_max = ms;
		if (ms > 35.0) fprintf(stderr, "[spike] frame %d: %.1f ms (tex upload %.1f, 3d render %.1f: viewport %.1f vbo %.1f passes %.1f final %.1f)\n", g_vframe, ms, g_prof_upload, g_prof_render, g_prof_p[0], g_prof_p[1], g_prof_p[2], g_prof_p[3]);
		for (int i = 0; i < 4; i++) s_ph[i] += g_prof_p[i];
		g_prof_upload = g_prof_render = 0; g_prof_p[0] = g_prof_p[1] = g_prof_p[2] = g_prof_p[3] = 0;
		if (++s_work_n == 120) { fprintf(stderr, "[perf] work per frame: avg %.2f ms, max %.2f ms, swap avg %.2f max %.2f (budget %.2f) audio underruns %d\n", s_work_sum / s_work_n, s_work_max, s_swap_sum / s_work_n, s_swap_max, 1000.0 * s_period / s_freq, AudioUnderRuns()); fprintf(stderr, "[phase] avg viewport %.2f vbo %.2f passes %.2f final %.2f\n", s_ph[0] / s_work_n, s_ph[1] / s_work_n, s_ph[2] / s_work_n, s_ph[3] / s_work_n); s_ph[0] = s_ph[1] = s_ph[2] = s_ph[3] = 0; s_work_sum = s_work_max = s_swap_sum = s_swap_max = 0; s_work_n = 0; }
	}
	static bool nopace = getenv("M3_NOPACE") != nullptr;   // dev: run as fast as possible
	if (s_period && !nopace) {
		AdaptStep((int64_t)(SDL_GetPerformanceCounter() - s_next));
		sleep_until(s_next);
		double scale = 1.0;
		int ab = AudioBufferedBytes();
		if (ab >= 0) {
			static double fill = -1, tgt = AUDIO_TARGET_FRAMES;
			static double fmin = 1e9, fmax = -1e9; static int nf = 0;
			double cur = (double)ab / AudioBytesPerFrame();
			fill = fill < 0 ? cur : fill + (cur - fill) * (1.0 / 90.0);
			if (cur < fmin) fmin = cur; if (cur > fmax) fmax = cur;
			double err = fill - tgt; err = err < -3 ? -3 : err > 3 ? 3 : err;
			scale = 1.0 + 0.0015 * err;
			if (++nf == 345) { fprintf(stderr, "[audio] fill min %.2f max %.2f avg %.2f frames, underruns %d, out peak %d, device pulls %u\n", fmin, fmax, fill, AudioUnderRuns(), AudioOutPeak(), AudioCallbacks()); fmin = 1e9; fmax = -1e9; nf = 0; }
		}
		uint64_t now = SDL_GetPerformanceCounter();
		s_next += (uint64_t)(s_period * scale);
		if (s_next + s_period * 15 < now) s_next = now + s_period;           // fell behind: resync, don't burst
	}
	static int dump_from = getenv("M3_DUMPFROM") ? atoi(getenv("M3_DUMPFROM")) : 0;
	if (g_dump_every && g_vframe >= dump_from && g_vframe % g_dump_every == 0) dump_frame();
	{ static int ne = 0; for (GLenum e; ne < 40 && (e = glGetError()) != GL_NO_ERROR; ne++) fprintf(stderr, "[glerr] 0x%04x at frame %d\n", e, g_vframe); }
	g_vframe++;
	if (s_framelog && g_vframe % 30 == 0) fprintf(stderr, "[frame %d]\n", g_vframe);
	// scale the fixed-size internal image into whatever the window is now (resize, other display, fullscreen)
	int dw, dh; SDL_GL_GetDrawableSize(s_window, &dw, &dh);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND);
	glViewport(0, 0, dw, dh); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
	double ia = (double)s_iw / s_ih; int bw = dw, bh = (int)(dw / ia + 0.5);
	if (bh > dh) { bh = dh; bw = (int)(dh * ia + 0.5); }
	int bx = (dw - bw) / 2, by = (dh - bh) / 2;
	glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fbo); glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
	glBlitFramebuffer(0, 0, s_iw, s_ih, bx, by, bx + bw, by + bh, GL_COLOR_BUFFER_BIT, (bw == s_iw && bh == s_ih) ? GL_NEAREST : GL_LINEAR);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	{ static const GLenum att[] = { GL_DEPTH_STENCIL_ATTACHMENT }; glBindFramebuffer(GL_FRAMEBUFFER, s_fbo); glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, att); glBindFramebuffer(GL_FRAMEBUFFER, 0); }   // its depth/stencil are never read: skip the write-back
	Touch::Draw(dw, dh);
	Settings::Draw(dw, dh);
	glEnable(GL_SCISSOR_TEST);
	uint64_t t_sw = SDL_GetPerformanceCounter();
	SDL_GL_SwapWindow(s_window);
	{ double ms = (double)(SDL_GetPerformanceCounter() - t_sw) * 1000.0 / s_freq; s_swap_sum += ms; if (ms > s_swap_max) s_swap_max = ms; }
	if (s_pendLv >= 0) { int lv = s_pendLv; s_pendLv = -1; bool down = lv > s_lv; ApplyRes(lv); if (down) SaveAuto(); }
	s_last_end = SDL_GetPerformanceCounter();
}

#ifdef __ANDROID__
#define M3_DEF_IRES (-1)
#else
#define M3_DEF_IRES 1080
#endif
struct Opts { double aspect = 0; int winh = 0; int w = 0, h = 0; bool touch = false; bool fullscreen = false, wide = true, vsync = false; int ires = M3_DEF_IRES; int mt = -1, gmt = -1; bool mtauto = false; bool unlock = true; int frames = 0; std::string dump; };

#ifdef __ANDROID__
#include <jni.h>
static void android_restart()
{
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject act = (jobject)SDL_AndroidGetActivity();
	if (!env || !act) return;
	jclass c = env->GetObjectClass(act);
	jmethodID m = env->GetMethodID(c, "restartApp", "()V");
	if (m) env->CallVoidMethod(act, m);
}
#endif

static void sleep_until(uint64_t target)
{
	uint64_t t = SDL_GetPerformanceCounter();
	while (t < target) {
		if ((int64_t)(target - t) * 1000 > 2 * (int64_t)s_freq) SDL_Delay(1);
		t = SDL_GetPerformanceCounter();
	}
}

// ---- NVRAM (EEPROM + backup RAM: settings, best times) persisted across runs
static std::string s_nvPath;
static void nv_load(CModel3 &m3)
{
	CBlockFile nv;
	if (nv.Load(s_nvPath) == Result::OKAY) { m3.LoadNVRAM(&nv); nv.Close(); fprintf(stderr, "NVRAM loaded: %s\n", s_nvPath.c_str()); }
	else fprintf(stderr, "NVRAM: none yet (%s)\n", s_nvPath.c_str());
}
static void nv_save(CModel3 &m3)
{
	std::string tmp = s_nvPath + ".tmp";
	CBlockFile nv;
	if (nv.Create(tmp, "Supermodel NVRAM State", "Supermodel NVRAM File") != Result::OKAY) { fprintf(stderr, "NVRAM: cannot write %s\n", tmp.c_str()); return; }
	m3.SaveNVRAM(&nv); nv.Close();
	if (rename(tmp.c_str(), s_nvPath.c_str()) != 0) fprintf(stderr, "NVRAM: rename failed\n");
}


// ---- Force feedback as phone vibration: the drive board's wheel commands (road vibration, cornering force) drive the vibrator
static std::atomic<float> s_ffConst{0}, s_ffVib{0};
static void FFHook(const ForceFeedbackCmd &c)
{
	static std::atomic<int> n{0}; if (n.load() < 12 && n.fetch_add(1) < 12) fprintf(stderr, "[ffb] cmd %d force %.2f\n", (int)c.id, c.force);
	switch (c.id) {
	case FFStop: s_ffConst = 0; s_ffVib = 0; break;
	case FFConstantForce: s_ffConst = c.force; break;
	case FFVibrate: s_ffVib = c.force; break;
	default: break;
	}
}
static SDL_Haptic *s_hap = nullptr; static bool s_hapTried = false; static float s_hapLvl = 0; static uint32_t s_hapT = 0;
static void HapticUpdate()
{
	int lvl = Settings::FFBLevel();   // the wheel board's forces are small on the straights, strong in corners and impacts
	float raw = 0;
	if (lvl > 0) { float c = fabsf(s_ffConst.load()); float a = c < 0.04f ? 0.0f : (c * 2.5f > 1.0f ? 1.0f : c * 2.5f), b = s_ffVib.load() * 1.3f; raw = a > b ? a : b; raw *= lvl == 1 ? 0.45f : lvl == 2 ? 0.75f : 1.0f; if (raw > 1) raw = 1; }
	if (raw < 0.04f) raw = 0;
	float q = floorf(raw * 8.0f + 0.5f) / 8.0f;
	uint32_t now = SDL_GetTicks();
	if (q == 0 && s_hapLvl == 0) return;
	if (!s_hap) {
		if (s_hapTried && now - s_hapT < 5000) return;
		s_hapTried = true; s_hapT = now;
		if (!SDL_WasInit(SDL_INIT_HAPTIC)) SDL_InitSubSystem(SDL_INIT_HAPTIC);
		if (SDL_NumHaptics() > 0) { s_hap = SDL_HapticOpen(0); if (s_hap && SDL_HapticRumbleInit(s_hap) != 0) { SDL_HapticClose(s_hap); s_hap = nullptr; } }
		fprintf(stderr, "[ffb] vibration %s (%d devices)\n", s_hap ? "ok" : "unavailable", SDL_NumHaptics());
		if (!s_hap) return;
	}
	if (q == 0) { SDL_HapticRumbleStop(s_hap); s_hapLvl = 0; return; }
	if (q != s_hapLvl || now - s_hapT > 90) { SDL_HapticRumblePlay(s_hap, q, 120); s_hapLvl = q; s_hapT = now; }
}

extern "C" { extern volatile int m3m_on, m3m_touch, m3m_tap, m3m_tapState, m3m_back, m3m_page, m3m_extras; }   // runtime/app/menu_guest.c
static bool PopMenuEv(int &k, float &x, float &y)
{
#ifndef __ANDROID__
	if (const char *ts = getenv("M3_TOUCHSIM")) {   // dev: scripted menu events "frame:kind:x:y,..." (kind 1 tap, 4 swipe up, 5 back)
		static int tf = 0; tf++; const char *q = ts;
		while (*q) { int fr = 0, kk = 0; float xx = 0, yy = 0; if (sscanf(q, "%d:%d:%f:%f", &fr, &kk, &xx, &yy) == 4 && fr == tf) { k = kk; x = xx; y = yy; return true; } const char *c = strchr(q, ','); if (!c) break; q = c + 1; }
		return false;
	}
#endif
	return Touch::PopMenuEvent(k, x, y);
}
int main(int argc, char **argv)
{
	if (argc < 2) { fprintf(stderr, "usage: %s <images_dir> [-w W -h H] [-fullscreen] [-widescreen] [-frames N]\n", argv[0]); return 2; }
	Opts o;
	Settings::Values sv;
	Settings::Load(sv);
	o.fullscreen = sv.fullscreen != 0; o.vsync = sv.vsync != 0; o.ires = sv.ires; o.winh = sv.winh;
	o.aspect = Settings::AspectValue(sv.aspect); o.wide = sv.aspect != 1;
	Settings::Init(sv);
	for (int i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "-w") && i + 1 < argc) o.w = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-h") && i + 1 < argc) o.h = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-fullscreen")) o.fullscreen = true;
		else if (!strcmp(argv[i], "-widescreen")) o.wide = true;
		else if (!strcmp(argv[i], "-43")) o.wide = false;
		else if (!strcmp(argv[i], "-dump-assets") && i + 1 < argc) setenv("M3_DUMP", argv[++i], 1);   // write original 2D/3D assets as PNG
		else if (!strcmp(argv[i], "-pack") && i + 1 < argc) setenv("M3_PACK", argv[++i], 1);          // load replacement assets
		else if (!strcmp(argv[i], "-native")) { o.fullscreen = true; if (!sv.iresSet) o.ires = 0; }   // fullscreen at the display's native pixel resolution
		else if (!strcmp(argv[i], "-ires") && i + 1 < argc) { int v = atoi(argv[++i]); if (!sv.iresSet) o.ires = v; }   // internal render height (0 = window size, -1 adaptive); the settings menu choice wins
		else if (!strcmp(argv[i], "-mtauto")) o.mtauto = true;   // experimental governor: switches sound/drive board threads at runtime (breaks audio sync on some devices)
		else if (!strcmp(argv[i], "-nounlock")) o.unlock = false;   // do not force the bookkeeping counters that unlock the hidden cars
		else if (!strcmp(argv[i], "-mt")) o.mt = 1;
		else if (!strcmp(argv[i], "-st")) { o.mt = 0; o.gmt = 0; }
		else if (!strcmp(argv[i], "-gpumt")) o.gmt = 1;
		else if (!strcmp(argv[i], "-nogpumt")) o.gmt = 0;
		else if (!strcmp(argv[i], "-vsync")) o.vsync = true;
		else if (!strcmp(argv[i], "-profsync")) g_prof_sync = true;
		else if (!strcmp(argv[i], "-touch")) o.touch = true;   // on-screen controls (always on in the Android build); mouse acts as a finger
		else if (!strcmp(argv[i], "-frames") && i + 1 < argc) o.frames = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-dump") && i + 1 < argc) g_dump_every = atoi(argv[++i]);
	}
#ifdef __ANDROID__
	{   // no console on a phone: keep a log next to the game files, and keep the pad/orientation sane
		SDL_SetHint(SDL_HINT_ACCELEROMETER_AS_JOYSTICK, "0");
		SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
		if (const char *ext = SDL_AndroidGetExternalStoragePath()) { std::string lp = std::string(ext) + "/srally2.log"; rename(lp.c_str(), (std::string(ext) + "/srally2.prev.log").c_str()); freopen(lp.c_str(), "w", stderr); setvbuf(stderr, nullptr, _IONBF, 0); dup2(fileno(stderr), 1); setvbuf(stdout, nullptr, _IONBF, 0); }
		o.fullscreen = true;
		SDL_SetThreadPriority(SDL_THREAD_PRIORITY_TIME_CRITICAL);
		s_stats = true;
	}
#endif
	SetLogger(std::make_shared<CConsoleErrorLogger>());
	Util::Config::Node cfg("Global");
	cfg.Set("EmulateSound", true); cfg.Set("EmulateDSB", true); cfg.Set("FlipStereo", false);
	cfg.Set("MusicVolume", 100); cfg.Set("SoundVolume", 100); cfg.Set("SimulateNet", false);
	{   // Threading is decided ONCE here from the real core count and never switched at runtime (switching desyncs the audio).
		// Parallel work per frame: PPC main board thread || sound board (SCSP/68k/DSB) thread || drive board thread || GL render (main thread).
		// All threads are joined at the end of every frame, so the result is deterministic and audio pacing is the same as single-threaded.
		unsigned cores = std::thread::hardware_concurrency(); if (cores == 0) cores = 1;
		bool mt  = o.mt  >= 0 ? o.mt  == 1 : cores >= 2;    // sound + drive board threads
		bool gmt = o.gmt >= 0 ? o.gmt == 1 : cores >= 3;    // PPC main board thread (needs MultiThreaded)
		if (!mt) gmt = false;
		g_mtOn = mt;
		fprintf(stderr, "cores=%u MultiThreaded=%d GPUMultiThreaded=%d\n", cores, (int)mt, (int)gmt);
		cfg.Set("GPUMultiThreaded", gmt); cfg.Set("MultiThreaded", mt);
	}
	cfg.Set("LegacySoundDSP", false);
	cfg.Set("NoWhiteFlash", false); cfg.Set("QuadRendering", false); cfg.Set("WideScreen", o.wide); cfg.Set("WideBackground", o.wide);
	cfg.Set("PowerPCFrequency", 166u);
	cfg.Set("Balance", 0.0f); cfg.Set("BalanceLeftRight", 0.0f); cfg.Set("BalanceFrontRear", 0.0f); cfg.Set("NbSoundChannels", 2);
	cfg.Set("SoundFreq", 60.0f);
	cfg.Set("InputDigitalDecaySpeed", 50u); cfg.Set("InputDigitalSensitivity", 50u);
#ifdef __ANDROID__
	cfg.Set("ForceFeedback", Settings::FFBLevel() > 0);   // the drive board only runs when asked to: it is what produces the wheel effects
#else
	cfg.Set("ForceFeedback", getenv("M3_FFB") != nullptr);
#endif
	cfg.Set("SDLConstForceMax", 100u); cfg.Set("SDLConstForceThreshold", 30u); cfg.Set("SDLFrictionMax", 100u);
	cfg.Set("SDLSelfCenterMax", 100u); cfg.Set("SDLVibrateMax", 100u);

	ROMSet rs;
	if (!LoadRomImages(rs, argv[1])) {
#ifdef __ANDROID__
		if (SDL_Init(SDL_INIT_VIDEO) == 0) {
			std::string m = std::string("Game data not found in:\n") + argv[1] + "\n\nCopy the 'images' folder built on your computer there (see android/README.md).";
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, M3_APP_TITLE, m.c_str(), nullptr);
		}
#endif
		return 1;
	}
	Game game = MakeSrally2();
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
#ifdef __ANDROID__
	// Audio output: OpenSL ES first. SDL's default on Android 8.1+ is AAudio, whose stream can come up silent on the very first
	// start of the app (after the zip picker and the data build) while still accepting writes, so nothing can even detect it.
	// OpenSL ES (and the Java AudioTrack path as second choice) start reliably every time.
	SDL_SetHint(SDL_HINT_AUDIODRIVER, "openslES,android,AAudio");
#endif
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) fprintf(stderr, "SDL audio: %s (will retry)\n", SDL_GetError());
	else fprintf(stderr, "[audio] driver %s\n", SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "?");

	SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8); SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0); SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);   // real depth/stencil live in our own FBO SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
#ifdef M3_GLES
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4); SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
	if (o.w <= 0 || o.h <= 0) {         // default: 80% of the desktop height, display aspect (widescreen) or 4:3
		SDL_Rect r; SDL_GetDisplayUsableBounds(0, &r);
		o.h = o.winh > 0 ? o.winh : (int)(r.h * 0.8);
		if (o.h > r.h) o.h = r.h;
		double ar = o.aspect > 0 ? o.aspect : (o.wide ? (double)r.w / r.h * 0.98 : 4.0 / 3.0);
		o.w = (int)(o.h * ar);
		if (o.w > r.w) { o.w = r.w; o.h = (int)(o.w / ar); }
	}
	s_window = SDL_CreateWindow(M3_APP_TITLE, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, o.w, o.h,
		SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_ALLOW_HIGHDPI | (o.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
	if (!s_window) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
	SDL_GLContext gl = SDL_GL_CreateContext(s_window);
	if (!gl) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
	SDL_GL_SetSwapInterval(o.vsync ? 1 : 0);
	Touch::Init(s_window, o.touch);
	Pad::Init();
	SDL_DisableScreenSaver();
	glewExperimental = GL_TRUE;
	if (glewInit() != GLEW_OK) { fprintf(stderr, "glew failed\n"); return 1; }
	glGetError();
	fprintf(stderr, "GL: %s\n", glGetString(GL_VERSION));

	s_stats = s_stats || getenv("M3_STATS") != nullptr;
	int dw, dh; SDL_GL_GetDrawableSize(s_window, &dw, &dh);
	if (o.aspect > 0) {
		int fw = (int)(dh * o.aspect + 0.5), fh = dh;
		if (fw > dw) { fw = dw; fh = (int)(dw / o.aspect + 0.5); }
		dw = fw; dh = fh;
	}
	if (o.ires > 0 && dh > o.ires) { dw = (int)((double)dw * o.ires / dh + 0.5); dh = o.ires; }   // cap internal resolution; scaled up on present
	dw &= ~1; dh &= ~1;
	s_baseW = dw; s_baseH = dh; s_wideMode = o.wide;
	// adaptive only when asked for ("Auto"): "Native" and fixed heights are never lowered
	s_adapt = (o.ires < 0 || getenv("M3_ADAPT") != nullptr) && getenv("M3_NOADAPT") == nullptr;
	{   // resume at the level this device settled on last time (only valid for the same base height)
		std::string dir = argv[1];
#ifdef __ANDROID__
		if (const char *ext = SDL_AndroidGetExternalStoragePath()) dir = ext;
#endif
		s_autoPath = dir + "/ires.auto";
		if (s_adapt) if (FILE *f = fopen(s_autoPath.c_str(), "r")) { int bh = 0, lv = 0; if (fscanf(f, "%d %d", &bh, &lv) == 2 && bh == s_baseH && lv > 0 && lv < NLV) s_lv = lv; fclose(f); }
		if (const char *e = getenv("M3_ADAPT_LV")) s_lv = atoi(e) % NLV;
	}
	LevelDims(s_lv, dw, dh);
	s_iw = dw; s_ih = dh;
	glGenTextures(1, &s_fbo_tex); glBindTexture(GL_TEXTURE_2D, s_fbo_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, s_iw, s_ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGenRenderbuffers(1, &s_fbo_rb); glBindRenderbuffer(GL_RENDERBUFFER, s_fbo_rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, s_iw, s_ih);   // 3D needs depth + stencil (shadows)
	glGenFramebuffers(1, &s_fbo); glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_fbo_tex, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_fbo_rb);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "internal framebuffer incomplete\n"); return 1; }
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	fprintf(stderr, "internal resolution %dx%d (%s)\n", s_iw, s_ih, o.ires < 0 ? "auto: lowered when frames are missed" : o.ires == 0 ? "native, fixed" : "fixed height");
	unsigned totalX = dw, totalY = dh, xRes = dw, yRes = dh, xOff = 0, yOff = 0;
	{                                   // 4:3 "viewable area"; in widescreen the frustum is widened and the scissor reveals the whole window
		float r = 496.0f / 384.0f;
		float xf = (float)xRes, yf = (float)yRes;
		if (yf < xf / r) xf = yf * r;
		if (xf < yf * r) yf = xf / r;
		xOff = (xRes - (unsigned)xf) / 2; yOff = (yRes - (unsigned)yf) / 2;
		xRes = (unsigned)xf; yRes = (unsigned)yf;
	}
	glViewport(0, 0, totalX, totalY);
	glClearColor(0, 0, 0, 0); glClearDepth(1.0); glDepthFunc(GL_LESS); glEnable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
	glBindFramebuffer(GL_FRAMEBUFFER, s_fbo); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT); glBindFramebuffer(GL_FRAMEBUFFER, 0);
	unsigned corr = (unsigned)(((yRes / 384.0) * 2.0) + 0.5);
	glEnable(GL_SCISSOR_TEST);
	if (o.wide) glScissor(0, corr, totalX, totalY - corr * 2);
	else glScissor(xOff + corr, yOff + corr, xRes - corr * 2, yRes - corr * 2);

	CModel3 m3(cfg);
	if (m3.Init() != Result::OKAY) return 1;
	if (m3.LoadGame(game, rs) != Result::OKAY) return 1;
	rs = ROMSet();
#ifdef XL_NATIVE
	// the game program is translated here, from the user's own ROM, into native code
	if (!xl_init(argv[1], m3.GetRAM(), getenv("M3_LAYOUT"))) {
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, M3_APP_TITLE, "The game program could not be translated (see srally2.log).", nullptr);
		return 1;
	}
#endif
	std::shared_ptr<CInputSystem> isys = std::make_shared<CSDLInputSystem>(cfg, true);
	CInputs inputs(isys);
	if (!inputs.Initialize()) { fprintf(stderr, "inputs init failed\n"); return 1; }
	{
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
	}
	m3.AttachInputs(&inputs);
	g_ffHook = FFHook;

	SetAudioType(game.audio);
	if (OpenAudio(cfg) != Result::OKAY) fprintf(stderr, "audio open failed (continuing silent)\n");

	SuperAA aa(1, CRTcolor::None); aa.Init(totalX, totalY);
	CRender2D r2(cfg);
	New3D::CNew3D r3(cfg, game.name);
	if (r2.Init(xOff, yOff, xRes, yRes, totalX, totalY, s_fbo, UpscaleMode::Bilinear) != Result::OKAY) return 1;
	if (r3.Init(xOff, yOff, xRes, yRes, totalX, totalY, s_fbo) != Result::OKAY) return 1;
	s_r2 = &r2; s_r3 = &r3;
	m3.AttachRenderers(&r2, &r3, &aa);
	{
		std::string dir = argv[1];
#ifdef __ANDROID__
		if (const char *ext = SDL_AndroidGetExternalStoragePath()) dir = ext;
#endif
		s_nvPath = dir + "/srally2.nvram";
		LeaderboardSetDir(dir);   // finished races are queued there for the online leaderboard
		{   // page address: leaderboard_url.txt (written by the user or the launcher), the player id marks your own rows
			auto line = [](const std::string &p) { std::string s; if (FILE *f = fopen(p.c_str(), "r")) { char b[512]; if (fgets(b, sizeof b, f)) s = b; fclose(f); }
				while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '/')) s.pop_back(); return s; };
			std::string u = line(dir + "/leaderboard_url.txt");
#ifdef M3_LB_URL
			if (u.empty()) u = M3_LB_URL;
#endif
			if (u.rfind("http", 0) == 0) { std::string id = line(dir + "/player_id.txt"); Settings::SetLeaderboardUrl(u + "/" + (id.empty() ? "" : "?player=" + id)); }
		}
	}
	nv_load(m3);
	m3.Reset();

	s_freq = SDL_GetPerformanceFrequency();
	uint64_t per_frame = (uint64_t)((double)s_freq / 60.0);
	s_period = per_frame; s_next = SDL_GetPerformanceCounter() + per_frame;
#ifdef __ANDROID__
	hint_init((int64_t)(1e9 / 60.0 * 0.75));
#endif
	bool quit = false; int nv_n = 0; int f = 0; uint64_t t0 = SDL_GetPerformanceCounter(); int fps_n = 0;
	bool escLatch = false;
	// adaptive threading governor: run single-threaded; if the frame work is heavy, try the sound/drive board threads for a
	// while and keep them only if they are measurably faster. -st / -mt force a mode and disable the governor.
	const unsigned gv_cores = std::thread::hardware_concurrency();
	int gv_state = 3; (void)gv_cores;   // governor retired: threading is fixed at startup      // 0 measure ST, 1 measure MT, 3 done
	double gv_sum = 0, gv_st = 0; int gv_n = 0;
	const int GV_FRAMES = 240;
	m3.SetMultiThreaded(g_mtOn);
	while (!quit) {
		Pad::Update();
		if (!inputs.Poll(&game, xOff, yOff, xRes, yRes)) quit = true;
		{   // gamepad analog controls (sticks/triggers) win over the keyboard/touch value only while they are actually moved
			float v;
			{ float tg, tb; if (Touch::PedalValues(tg, tb)) { if (tg > 0) inputs.accelerator->value = (UINT16)lrintf(tg * 255.0f); if (tb > 0) inputs.brake->value = (UINT16)lrintf(tb * 255.0f); } }
			if (Pad::Steer(v)) inputs.steering->value = (UINT16)lrintf(128.0f + v * 127.0f);
			if (Pad::Gas(v)) { UINT16 a = (UINT16)lrintf(v * 255.0f); if (a > inputs.accelerator->value) inputs.accelerator->value = a; }
			if (Pad::Brake(v)) { UINT16 a = (UINT16)lrintf(v * 255.0f); if (a > inputs.brake->value) inputs.brake->value = a; }
		}
		{   // analog steering overrides (the d-pad/keys stay the default)
			int sm = Settings::SteerMode();
			float sv = 0; bool have = false;
			if (sm == 1 || sm == 2 || sm == 4) have = Touch::AnalogSteer(sv);
			else if (sm == 3 && !Settings::IsOpen()) {
				int mx, my; SDL_GetMouseState(&mx, &my);
				int ww, wh; SDL_GetWindowSize(s_window, &ww, &wh);
				if (ww > 0) { sv = ((float)mx / ww) * 2.0f - 1.0f; if (fabsf(sv) < 0.03f) sv = 0; sv = sv * 1.15f; have = true; }
			}
			if (have && (sm == 1 || sm == 2 || sm == 4)) { int fe = Settings::SteerFeel(); if (fe) { float a = fabsf(sv); if (a > 1) a = 1; a = powf(a, fe == 1 ? 1.3f : 1.6f); sv = sv < 0 ? -a : a; } }   // calmer around the centre, still full lock at the end
			if (have) { if (sv > 1) sv = 1; if (sv < -1) sv = -1; inputs.steering->value = (UINT16)lrintf(128.0f + sv * 127.0f); }
		}
		{
			const Uint8 *mk = SDL_GetKeyboardState(nullptr);
			Settings::Action act = Settings::Update(mk);
			if (act == Settings::OPENED) SetAudioEnabled(false);
			if (act == Settings::CLOSED || act == Settings::APPLY) SetAudioEnabled(true);
			if (act == Settings::OPENED || act == Settings::CLOSED) escLatch = true;
			if (escLatch && !mk[SDL_SCANCODE_ESCAPE]) escLatch = false;
			if (act == Settings::APPLY) {
				Settings::Save(Settings::Pending());
#ifdef __ANDROID__
				nv_save(m3); CloseAudio();
				android_restart();
				for (;;) SDL_Delay(100);
#endif
				std::vector<std::string> a; a.push_back(argv[0]);
				for (int i = 1; i < argc; i++) {
					const char *x = argv[i];
					if (i >= 2 && (!strcmp(x, "-w") || !strcmp(x, "-h") || !strcmp(x, "-ires"))) { i++; continue; }
					if (i >= 2 && (!strcmp(x, "-native") || !strcmp(x, "-fullscreen") || !strcmp(x, "-widescreen") || !strcmp(x, "-43") || !strcmp(x, "-vsync"))) continue;
					a.push_back(x);
				}
				std::vector<char *> av; for (auto &t : a) av.push_back((char *)t.c_str()); av.push_back(nullptr);
				nv_save(m3); CloseAudio(); SDL_Quit();
				execvp(av[0], av.data());
				perror("restart");
				return 1;
			}
			if (Settings::IsOpen()) { EndFrameVideo(); continue; }
		}
		uint64_t gv_t0 = SDL_GetPerformanceCounter();
		if (o.unlock && g_vframe >= 30 && g_vframe % 10 == 0) {   // hidden cars (Delta/Celica) need >=30 days of operation or >=1000 games in the bookkeeping: keep those counters above the threshold
			const UINT32 B = 0x4b2180;   // only raise a counter that is below the threshold, so it then keeps counting up normally
			if (m3.Read32(B + 0x34) < 40) m3.Write32(B + 0x34, 40);
			if (m3.Read32(B + 0x40) < 1500) m3.Write32(B + 0x40, 1500);
		}
		{   // Menus, Android style: touch only. A tap highlights an item, a second tap on the highlighted item confirms it, BACK goes one
			// page back, a swipe up flips the extra cars page; there is no countdown. The game's own menu code applies these requests
			// (runtime/app/menu_guest.c), so highlight, sounds, animations and transitions are the original ones.
			// A pad (or the keyboard on desktop) keeps the cabinet behaviour: wheel moves the cursor, pedal confirms.
			static int mst = -1, stFrames = 0, gasFrames = 0;
			static float sm = 0; static bool raceUI = false;
			if (gasFrames > 0) { gasFrames--; inputs.accelerator->value = 255; }
			int st = (int)m3.Read32(0x22435C);      // the game's state: 3 game, 6 course, 9 car, 12 gearbox, 13..16 name entry, 17 stage (loading card, then the race)
			if (st != mst) { mst = st; stFrames = 0; sm = 0; } else if (stFrames < 1000000) stFrames++;
			bool race = st == 0x11 && stFrames >= 340;   // the stage card ("SNOWY START!!") lasts ~330 game frames; the driving controls appear after it
			if (race && !raceUI && Settings::SteerMode() == 2) Touch::TiltCalibrate(Touch::TiltRoll());
			if (race && !raceUI) Touch::ResetGear();   // every race starts in 1st, like the HUD   // tilt neutral = the pose the phone is held in at the start
#ifndef __ANDROID__
			if (race != raceUI) fprintf(stderr, "[ui] race controls %s at state %x frame %d\n", race ? "ON" : "off", st, f);
#endif
			raceUI = race;
			{   // widescreen: race HUD pieces out to the screen edges, clear of the on-screen controls
				Render2DHud h; bool ctl = Touch::Enabled() && !Pad::HidesTouch();
				h.on = race && s_wideMode; h.splitBottom = !ctl;   // the touch pedals/arrows own the bottom corners
				h.marginTopLeft = ctl ? 0.13f : 0.02f; h.marginBottom = 0.02f; h.marginRight = 0.02f;
				SetRender2DHud(h);
			}
			bool menu = !race;
			float t = ((int)inputs.steering->value - 128) / 127.0f;
			int smode = Settings::SteerMode();
			bool touchMenus = (smode == 1 || smode == 2 || smode == 4) && !Pad::HidesTouch();
			if (menu && touchMenus) t = 0;              // tilt/wheel noise must not move the cursor
			m3m_on = 1; m3m_touch = touchMenus ? 1 : 0; m3m_tap = -1; m3m_back = 0; m3m_page = 0;   // requests live for one game frame
			if (menu) {
				Touch::SetHideRace(true);
				bool names = st >= 13 && st <= 16;  // name entry: the letter wheel is turned by dragging a finger (analog), a tap enters the letter
				static bool wasDrag = false, spring = false; static float sm0 = 0;
				float ddx = 0; bool dragging = names && Touch::MenuDrag(ddx);
				if (dragging && !wasDrag) { sm0 = 0; spring = false; }
				if (dragging) { sm = sm0 - ddx / 0.40f;   // the letter ring follows the finger (drag left = ring turns clockwise)
				 sm = sm > 1 ? 1 : sm < -1 ? -1 : sm; }
				else {
					if (wasDrag) spring = true;
					if (spring) { float d = t - sm; sm += d > 0.12f ? 0.12f : d < -0.12f ? -0.12f : d; if (sm == t) spring = false; }   // wheel springs back to centre
					else sm = t;
				}
				wasDrag = dragging;
				int ek; float ex, ey;
				while (PopMenuEv(ek, ex, ey)) {
					if (ek == 5) { if (st == 6 || st == 9 || st == 12 || st == 15) m3m_back = 1; continue; }     // BACK
					if (ek == 4) continue;                                                            // (all cars are on one screen)
					if (ek != 1) continue;
					int it = -1;
					if (st == 3 || st == 12) it = ex < 0.5f ? 0 : 1;                                  // two big cards: left / right
					else if (st == 9) {
						static const float cx6[6] = {0.158f, 0.26f, 0.354f, 0.646f, 0.73f, 0.83f}, cy6[6] = {0.72f, 0.51f, 0.29f, 0.29f, 0.51f, 0.72f};   // card centres, 4:3 picture
						static const float cx8[8] = {0.172f, 0.272f, 0.363f, 0.637f, 0.727f, 0.83f, 0.40f, 0.618f}, cy8[8] = {0.732f, 0.513f, 0.313f, 0.313f, 0.513f, 0.732f, 0.732f, 0.732f};   // all eight cars (menu_guest.c layout8)
						bool all = m3m_extras != 0; int n = all ? 8 : 6; const float *cx = all ? cx8 : cx6, *cy = all ? cy8 : cy6;
						float rx = all ? 0.11f : 0.15f, ry = all ? 0.09f : 0.13f, bd = 1e9f;
						for (int i = 0; i < n; i++) { float dx = (ex - cx[i]) / rx, dy = (ey - cy[i]) / ry, dd = dx * dx + dy * dy; if (dd < bd && dd < 1.6f) { bd = dd; it = i; } }
					} else if (st == 6) {                                                             // course carousel: front card = current, left/right = neighbours, top = opposite
						int cur = (int)(m3.Read32(0x1371c0) & 3);
						it = ex < 0.3f ? (cur + 3) & 3 : ex > 0.7f ? (cur + 1) & 3 : ey < 0.36f ? (cur + 2) & 3 : cur;
					} else if (names) gasFrames = 6;
					if (it >= 0) { m3m_tap = it; m3m_tapState = st; }
				}
				if (touchMenus && !names) sm = 0;
			} else { sm = t; Touch::SetHideRace(false); }
			Touch::SetGameMenu(menu);
			inputs.steering->value = (UINT16)lrintf(128.0f + sm * 127.0f);
		}
#ifdef __ANDROID__
		HapticUpdate();
#endif
		AudioWatchdog(false);
#ifndef __ANDROID__
		Touch::DevFrame();
		if (const char *wt = getenv("M3_WATCH")) {   // dev: print the listed guest addresses every 20 frames
			static int wf = 0; if (++wf % 20 == 0) { fprintf(stderr, "[watch] f%d", wf); const char *q = wt; while (*q) { unsigned ad = 0; sscanf(q, "%x", &ad); fprintf(stderr, " %x=%x", ad, (unsigned)m3.Read32(ad)); const char *c = strchr(q, ','); if (!c) break; q = c + 1; } fprintf(stderr, "\n"); }
		}
#endif
		m3.RunFrame();
		if (gv_state < 2) {
			gv_sum += (double)(SDL_GetPerformanceCounter() - gv_t0) * 1000.0 / (double)s_freq;
			if (++gv_n >= GV_FRAMES) {
				double avg = gv_sum / gv_n; gv_sum = 0; gv_n = 0;
				if (gv_state == 0) {
					gv_st = avg;
					if (avg < 9.0) { fprintf(stderr, "[mt] single-thread %.2f ms/frame: fast enough, staying single-threaded\n", avg); gv_state = 3; }
					else { fprintf(stderr, "[mt] single-thread %.2f ms/frame: trying sound/drive board threads\n", avg); m3.SetMultiThreaded(true); gv_state = 1; }
				} else {
					bool keep = m3.IsMultiThreaded() && avg < gv_st * 0.85;
					fprintf(stderr, "[mt] threaded %.2f ms/frame vs %.2f single: %s\n", avg, gv_st, keep ? "keeping threads" : "back to single-threaded");
					m3.SetMultiThreaded(keep); gv_state = 3;
				}
			}
		}
		{   // diagnostics: log the first input edges so a bad device can be understood from srally2.log
			static int logged = 0; static bool pt = false, ps = false, pc = false, pst = false;
			bool t = inputs.test[0]->value != 0, s = inputs.service[0]->value != 0, c = inputs.coin[0]->value != 0, st = inputs.start[0]->value != 0;
			if (logged < 40 && (t != pt || s != ps || c != pc || st != pst)) { fprintf(stderr, "[in] frame %d test=%d service=%d coin=%d start=%d steer=%d\n", f, (int)t, (int)s, (int)c, (int)st, (int)inputs.steering->value); logged++; }
			pt = t; ps = s; pc = c; pst = st;
		}
		{   // F11 or Alt+Enter: toggle fullscreen (the image is scaled from the internal buffer, nothing else to redo)
			const Uint8 *ks = SDL_GetKeyboardState(nullptr);
			static bool prev = false;
			bool now_p = ks[SDL_SCANCODE_F11] || ((ks[SDL_SCANCODE_LALT] || ks[SDL_SCANCODE_RALT]) && ks[SDL_SCANCODE_RETURN]);
			if (now_p && !prev) SDL_SetWindowFullscreen(s_window, (SDL_GetWindowFlags(s_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
			prev = now_p;
		}
		if (inputs.uiExit->Pressed() && !escLatch) quit = true;
		if (inputs.uiReset->Pressed()) m3.Reset();
		uint64_t now = SDL_GetPerformanceCounter();
		if (++fps_n == 120) { double s = (double)(now - t0) / s_freq; char t[96]; snprintf(t, sizeof t, M3_APP_TITLE " - %.1f fps", fps_n / s); SDL_SetWindowTitle(s_window, t); t0 = now; fps_n = 0; }
		if (o.frames && ++f >= o.frames) quit = true;
		if (++nv_n >= 300) { nv_n = 0; nv_save(m3); }   // ~5 s: a phone can be killed in the background
	}
	nv_save(m3);
	CloseAudio();
	SDL_Quit();
	return 0;
}
