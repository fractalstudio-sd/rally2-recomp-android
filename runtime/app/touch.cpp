#include "touch.h"
#include "pad.h"
#include <GL/glew.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <mutex>
#include <cmath>
#include <string>
#include <algorithm>
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "third_party/stb_truetype.h"
#include "third_party/inter_font.h"
#include "Pkgs/stb_image.h"

namespace Touch {

namespace {
enum Btn { LEFT, RIGHT, GAS, BRAKE, SHUP, SHDN, VIEW, HAND, COIN, START, MENU, TEST, SERV, BACK, NBTN };
struct Def { const char *label; int key; };
const Def defs[NBTN] = {
	{"<", SDL_SCANCODE_LEFT}, {">", SDL_SCANCODE_RIGHT}, {"GAS", SDL_SCANCODE_UP}, {"BRAKE", SDL_SCANCODE_DOWN},
	{"+", 0}, {"-", 0}, {"VIEW", SDL_SCANCODE_A}, {"HANDBRAKE", SDL_SCANCODE_S},
	{"COIN", SDL_SCANCODE_3}, {"START", SDL_SCANCODE_1}, {"MENU", 0},
	{"TEST", 0}, {"SERVICE", 0}, {"BACK", 0},   // arcade test menu / service button (KEY_6 / KEY_5)
};
struct Rect { float x, y, w, h; };

bool g_on = false;
std::atomic<uint32_t> g_mask{0};
std::atomic<uint32_t> g_testUntil{0}, g_servUntil{0};              // pressed buttons
std::atomic<int> g_gear{1};                   // 1..4: the game itself treats 'no gear' as 1st (HUD shows MT 1), so the lever has no N
std::atomic<bool> g_menuReq{false}, g_menuOpen{false};
std::atomic<int> g_menuTap{-1};
std::atomic<bool> g_gameMenu{true};           // the game is showing one of its own menus (set by the main loop)
std::atomic<uint32_t> g_extraUntil{0};
std::atomic<float> g_gasA{0}, g_brkA{0};       // analog pedal positions 0..1 from the touch pedals
std::atomic<bool> g_apedal{false};            // pedal sensitivity on: thumb position sets the pressure; off: a touch is full pedal
std::atomic<bool> g_hideRace{false};          // game menu on screen: race-only controls are neither drawn nor active
bool g_swOn = false; SDL_FingerID g_swId = 0; float g_swX0 = 0, g_swV = 0;   // mode 4: swipe-tracking steering (finger drag = wheel position)
bool g_mdOn = false; SDL_FingerID g_mdId = 0; float g_mdX0 = 0, g_mdX = 0;   // menu drag: one finger acts as the analog wheel
std::atomic<int> g_ev{0};                     // pending menu gesture: 1 tap, 2 swipe left, 3 swipe right
std::atomic<float> g_evX{0}, g_evY{0};        // swipe in a game menu = View + Handbrake pulse (hidden cars)
struct Swipe { float x, y; uint32_t t; bool ok; };
std::map<SDL_FingerID, Swipe> g_swipe;
float g_mRow0 = 0, g_mStep = 1, g_mPx = 0, g_mPw = 0; int g_mN = 0;      // menu layout from the last DrawMenu
std::atomic<bool> g_shifted{false};           // lever touched at least once (before that, neutral sends nothing)
std::mutex g_mu;
std::map<SDL_FingerID, std::pair<float, float>> g_fingers;   // normalised positions
SDL_Window *g_win = nullptr;
// analog steering: 0 none (d-pad), 1 touch wheel, 2 tilt (3 = mouse is handled by the caller)
int g_steerMode = 0, g_tiltInv = 0;
const float DEG = 0.0174532925f, LOCK = 120.0f * DEG, FULL = 100.0f * DEG;
float g_wAng = 0, g_wLast = 0;                // wheel angle (rad, clockwise positive) and last finger angle
bool g_wHeld = false; SDL_FingerID g_wFinger = 0;
float g_tilt = 0;
SDL_Joystick *g_accel = nullptr; uint32_t g_sensFirst = 0; SDL_Sensor *g_sens = nullptr, *g_gyro = nullptr; float g_roll = 0, g_tiltZero = 0, g_tiltRange = 40.0f; bool g_rollInit = false; uint32_t g_accelTry = 0, g_lastTick = 0;
GLuint g_wheelTex = 0; bool g_wheelLoaded = false, g_wheelTried = false;
int g_ww = 1, g_wh = 1;                       // last drawn size (for hit-testing)

Rect Layout(int b, int W, int H)
{
	float u = (float)H;
	float up = g_steerMode == 1 || g_steerMode == 2 ? 0.13f * u : 0.0f;     // clear the wheel
	switch (b) {
	case LEFT:  return {0.04f * u, H - 0.30f * u, 0.26f * u, 0.26f * u};
	case RIGHT: return {0.34f * u, H - 0.30f * u, 0.26f * u, 0.26f * u};
	case GAS:   return {W - 0.34f * u, H - 0.40f * u, 0.30f * u, 0.36f * u};     // analog pedals: how far down the finger is = how far the pedal goes
	case BRAKE: return {W - 0.60f * u, H - 0.40f * u, 0.22f * u, 0.36f * u};
	case SHUP:  return {W - 0.17f * u, H - 0.60f * u, 0.14f * u, 0.18f * u};
	case SHDN:  return {W - 0.46f * u, H - 0.60f * u, 0.14f * u, 0.18f * u};
	case VIEW:  return {0.04f * u, H - 0.50f * u - up, 0.18f * u, 0.14f * u};
	case HAND:  return {0.25f * u, H - 0.50f * u - up, 0.35f * u, 0.14f * u};
	case COIN:  return {W * 0.5f - 0.27f * u, 0.02f * u, 0.25f * u, 0.11f * u};
	case MENU:  return {0.04f * u, 0.06f * u, 0.16f * u, 0.09f * u};
	case TEST:  return {0.26f * u, 0.06f * u, 0.16f * u, 0.09f * u};
	case BACK:  return {0.04f * u, 0.19f * u, 0.16f * u, 0.09f * u};     // menus only: one page back
	case SERV:  return {W - 0.28f * u, 0.06f * u, 0.24f * u, 0.09f * u};   // top right, clear of the lap time
	default:    return {W * 0.5f + 0.02f * u, 0.02f * u, 0.25f * u, 0.11f * u};
	}
}

bool WheelGeom(int W, int H, float &cx, float &cy, float &R)
{
	float u = (float)H; R = 0.22f * u; cx = 0.27f * u; cy = H - 0.27f * u; (void)W; return true;
}
float Wrap(float a) { while (a > 3.14159265f) a -= 6.2831853f; while (a < -3.14159265f) a += 6.2831853f; return a; }

void Recompute()
{
	uint32_t m = 0; float gasV = 0, brkV = 0;
	for (auto &f : g_fingers) {
		if (g_wHeld && f.first == g_wFinger) continue;
		if (g_mdOn && f.first == g_mdId) continue;
		if (g_swOn && f.first == g_swId) continue;
		float px = f.second.first * g_ww, py = f.second.second * g_wh;
		for (int b = 0; b < NBTN; b++) {
			if (g_steerMode && (b == LEFT || b == RIGHT)) continue;
			if (g_hideRace.load() && b <= HAND) continue;
			if ((b == BACK || b == TEST || b == SERV) && !g_hideRace.load()) continue;   // race: no BACK, and TEST / SERVICE wait outside the race (clear of the HUD)
			Rect r = Layout(b, g_ww, g_wh);
			float padx = b >= MENU ? 0.0f : r.w * 0.15f, pady = b >= MENU ? 0.0f : r.h * 0.15f;
			if (px >= r.x - padx && px <= r.x + r.w + padx && py >= r.y - pady && py <= r.y + r.h + pady) {
				m |= 1u << b;
				if (b == GAS || b == BRAKE) {   // the lower 60% of the pad is full pedal, the top 40% eases off to 30%
					float fr = (py - r.y) / r.h; fr = fr < 0 ? 0 : fr > 1 ? 1 : fr;
					float fu = 1.0f - fr;   // sensitivity on: top of the pad = full pedal (the thumb is near the gear buttons), lower = lighter
					float v = !g_apedal.load() ? 1.0f : fu >= 0.6f ? 1.0f : 0.3f + 0.7f * (fu / 0.6f);
					if (b == GAS) { if (v > gasV) gasV = v; } else if (v > brkV) brkV = v;
				}
			}
		}
	}
	if (g_menuOpen.load()) { m = 0; gasV = brkV = 0; }
	g_gasA = gasV; g_brkA = brkV;
	uint32_t old = g_mask.exchange(m);
	uint32_t rising = m & ~old;
	if (rising & (1u << MENU)) g_menuReq = true;
	if ((rising & (1u << BACK)) && g_hideRace.load()) { g_evX = 0; g_evY = 0; g_ev = 5; }   // menus: back one page
	// TEST / SERVICE are arcade push buttons: one tap = one fixed-length pulse, whatever the finger does afterwards (a finger
	// lost to a system gesture can never leave the button stuck down)
	if (rising & (1u << TEST)) { g_testUntil = SDL_GetTicks() + 180; fprintf(stderr, "[touch] TEST pulse\n"); }
	if (rising & (1u << SERV)) { g_servUntil = SDL_GetTicks() + 180; fprintf(stderr, "[touch] SERVICE pulse\n"); }
	if (rising & (1u << SHUP)) { int g = g_gear.load(); if (g < 4) g_gear = g + 1; g_shifted = true; }
	if (rising & (1u << SHDN)) { int g = g_gear.load(); if (g > 1) g_gear = g - 1; g_shifted = true; }
}

bool OnControl(float px, float py)
{
	for (int b = 0; b < NBTN; b++) {
		if (g_steerMode && (b == LEFT || b == RIGHT)) continue;
		if (g_hideRace.load() && b <= HAND) continue;
		if ((b == BACK || b == TEST || b == SERV) && !g_hideRace.load()) continue;   // race: no BACK, and TEST / SERVICE wait outside the race (clear of the HUD)
		Rect r = Layout(b, g_ww, g_wh);
		if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h) return true;
	}
	if (g_steerMode == 1 && !g_hideRace.load()) { float cx, cy, R; WheelGeom(g_ww, g_wh, cx, cy, R); float dx = px - cx, dy = py - cy; if (dx * dx + dy * dy <= R * R * 1.56f) return true; }
	return false;
}

int SDLCALL Watch(void *, SDL_Event *e)
{
	if (!g_on) return 1;
	std::lock_guard<std::mutex> l(g_mu);
	switch (e->type) {
	case SDL_FINGERDOWN: case SDL_FINGERMOTION:
		g_fingers[e->tfinger.fingerId] = {e->tfinger.x, e->tfinger.y};
		if (e->type == SDL_FINGERDOWN) {
			bool mt = !g_menuOpen.load() && g_gameMenu.load() && !OnControl(e->tfinger.x * g_ww, e->tfinger.y * g_wh);   // COIN / START / TEST / SERVICE / MENU stay real buttons
			g_swipe[e->tfinger.fingerId] = {e->tfinger.x, e->tfinger.y, SDL_GetTicks(), mt};
			if (mt && !g_mdOn) { g_mdOn = true; g_mdId = e->tfinger.fingerId; g_mdX0 = g_mdX = e->tfinger.x; }
			if (g_steerMode == 4 && !g_swOn && !g_menuOpen.load() && !g_hideRace.load() && !(g_mdOn && g_mdId == e->tfinger.fingerId) && !OnControl(e->tfinger.x * g_ww, e->tfinger.y * g_wh)) { g_swOn = true; g_swId = e->tfinger.fingerId; g_swX0 = e->tfinger.x * g_ww; g_swV = 0; }
		} else if (g_mdOn && e->tfinger.fingerId == g_mdId) g_mdX = e->tfinger.x;
		if (e->type == SDL_FINGERMOTION && g_swOn && e->tfinger.fingerId == g_swId) {
			float x = e->tfinger.x * g_ww, range = 0.30f * g_ww, d = (x - g_swX0) / range;
			if (d > 1) { g_swX0 = x - range; d = 1; } else if (d < -1) { g_swX0 = x + range; d = -1; }   // floating centre: reversing responds at once
			g_swV = d;
		}
		if (g_steerMode == 1 && !g_menuOpen.load() && !g_hideRace.load()) {
			float cx, cy, R; WheelGeom(g_ww, g_wh, cx, cy, R);
			float dx = e->tfinger.x * g_ww - cx, dy = e->tfinger.y * g_wh - cy;
			float a = atan2f(dy, dx);
			if (e->type == SDL_FINGERDOWN && !g_wHeld && dx * dx + dy * dy <= R * R * 1.56f) { g_wHeld = true; g_wFinger = e->tfinger.fingerId; g_wLast = a; }
			else if (g_wHeld && e->tfinger.fingerId == g_wFinger) {
				g_wAng += Wrap(a - g_wLast); g_wLast = a;
				if (g_wAng > LOCK) g_wAng = LOCK;
				if (g_wAng < -LOCK) g_wAng = -LOCK;
			}
		}
		if (e->type == SDL_FINGERDOWN && g_menuOpen.load() && g_mN > 0) {
			float px = e->tfinger.x * g_ww, py = e->tfinger.y * g_wh;
			int i = (int)((py - (g_mRow0 - g_mStep * 0.5f)) / g_mStep);
			if (py >= g_mRow0 - g_mStep * 0.5f && i >= 0 && i < g_mN && px >= g_mPx && px <= g_mPx + g_mPw)
				g_menuTap = (i << 2) | (px < g_mPx + g_mPw * 0.35f ? 1 : 2);
		}
		Recompute(); break;
	case SDL_FINGERUP: {
		auto it = g_swipe.find(e->tfinger.fingerId);
		if (it != g_swipe.end()) {
			Swipe sw = it->second; g_swipe.erase(it);
			float dx = (e->tfinger.x - sw.x) * g_ww, dy = (e->tfinger.y - sw.y) * g_wh, mn = (float)(g_ww < g_wh ? g_ww : g_wh);
			uint32_t dt = SDL_GetTicks() - sw.t; float d2 = dx * dx + dy * dy;
			if (g_mdOn && e->tfinger.fingerId == g_mdId) g_mdOn = false;
			if (g_swOn && e->tfinger.fingerId == g_swId) g_swOn = false;
			if (sw.ok && g_gameMenu.load() && dt < 700) {
				float W = (float)g_ww, H = (float)g_wh, aw = H * 4.0f / 3.0f;
				if (d2 > (0.12f * mn) * (0.12f * mn)) {
					// slow horizontal drags are the analog wheel (MenuDrag); only a fast flick or a vertical swipe is an event
					if (fabsf(dx) >= fabsf(dy)) { if (dt < 220) { g_evX = 0; g_evY = 0; g_ev = dx < 0 ? 2 : 3; } }
					else { g_evX = 0; g_evY = 0; g_ev = 4; }
				} else if (d2 < (0.04f * mn) * (0.04f * mn) && dt < 350) {
					g_evX = (e->tfinger.x * W - (W - aw) * 0.5f) / aw; g_evY = e->tfinger.y; g_ev = 1;           // tap, in the 4:3 picture's own coordinates
				}
			}
		}
	}
		g_fingers.erase(e->tfinger.fingerId); if (g_wHeld && e->tfinger.fingerId == g_wFinger) g_wHeld = false; Recompute(); break;
	case SDL_APP_WILLENTERBACKGROUND: g_fingers.clear(); g_wHeld = false; Recompute(); break;
	}
	return 1;
}

// ---- Inter SemiBold (SIL OFL) baked into an atlas with stb_truetype
const int AW = 1024, AH = 512, BAKE = 64;
const float CAPH = 0.727f;                     // Inter cap height / em
stbtt_packedchar g_pc[95];
GLuint g_prog = 0, g_vao = 0, g_vbo = 0, g_fontTex = 0;
GLint u_scr = -1, u_col = -1, u_rect = -1, u_mode = -1, u_uv = -1;

const char *VS = R"(#version 410 core
uniform vec2 scr; uniform vec4 rect;       // px: x y w h
out vec2 uv;
void main() {
	vec2 c = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));
	uv = c;
	vec2 p = rect.xy + c * rect.zw;
	gl_Position = vec4(p.x / scr.x * 2.0 - 1.0, 1.0 - p.y / scr.y * 2.0, 0.0, 1.0);
})";
const char *FS = R"(#version 410 core
uniform vec4 col; uniform int mode; uniform vec4 uvr; uniform sampler2D fnt; uniform sampler2D wtex; uniform float ang;
in vec2 uv; out vec4 o;
void main() {
	if (mode == 2) { o = col; }
	else if (mode == 0) {                          // rounded box
		vec2 q = abs(uv - 0.5) * 2.0;
		float r = 0.35; vec2 d = max(q - (1.0 - r), 0.0);
		float a = 1.0 - smoothstep(r - 0.06, r, length(d) * 1.0);
		float rim = smoothstep(r - 0.22, r - 0.10, length(d)) * a;
		o = vec4(col.rgb + 0.25 * rim, col.a * (0.55 * a + 0.45 * rim));
	} else if (mode >= 3) {                       // steering wheel, rotated by ang (clockwise)
		vec2 q = (uv - 0.5) * 2.0;
		float c = cos(ang), s = sin(ang);
		vec2 p = vec2(q.x * c + q.y * s, -q.x * s + q.y * c);
		if (max(abs(p.x), abs(p.y)) > 1.0) { o = vec4(0.0); }
		else if (mode == 4) { vec4 t = texture(wtex, p * 0.5 + 0.5); o = vec4(t.rgb, t.a * col.a); }
		else {
			float r = length(p);
			float ring = smoothstep(1.0, 0.97, r) * smoothstep(0.72, 0.75, r);
			float hub = smoothstep(0.30, 0.28, r);
			float spoke = (abs(p.y) < 0.09 || (p.y > 0.0 && abs(p.x) < 0.09)) && r < 0.8 ? 1.0 : 0.0;
			float a = max(ring, max(hub, spoke));
			float lit = 0.5 + 0.5 * dot(p / max(r, 1e-3), vec2(-0.6, -0.8));
			vec3 base = vec3(0.13) + 0.10 * lit * (ring + hub);
			if (p.y < -0.8 && abs(p.x) < 0.04 && ring > 0.0) base = vec3(0.9, 0.75, 0.15);
			o = vec4(base, a * col.a);
		}
	} else {                                  // glyph from the font atlas
		float t = texture(fnt, uvr.xy + uv * uvr.zw).r;
		o = vec4(col.rgb, col.a * t);
	}
})";

GLuint Compile(GLenum t, const char *s)
{
	GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh);
	GLint ok = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) { char log[512]; glGetShaderInfoLog(sh, 512, nullptr, log); fprintf(stderr, "[touch] shader: %s\n", log); }
	return sh;
}
void GlInit()
{
	g_prog = glCreateProgram();
	GLuint v = Compile(GL_VERTEX_SHADER, VS), f = Compile(GL_FRAGMENT_SHADER, FS);
	glAttachShader(g_prog, v); glAttachShader(g_prog, f); glLinkProgram(g_prog);
	glDeleteShader(v); glDeleteShader(f);
	u_scr = glGetUniformLocation(g_prog, "scr"); u_col = glGetUniformLocation(g_prog, "col");
	u_rect = glGetUniformLocation(g_prog, "rect"); u_mode = glGetUniformLocation(g_prog, "mode");
	u_uv = glGetUniformLocation(g_prog, "uvr");
	glGenVertexArrays(1, &g_vao);
	std::vector<uint8_t> px((size_t)AW * AH, 0);
	stbtt_pack_context pc;
	stbtt_PackBegin(&pc, px.data(), AW, AH, 0, 2, nullptr);
	stbtt_PackSetOversampling(&pc, 2, 2);
	stbtt_PackFontRange(&pc, kInterTTF, 0, (float)BAKE, 32, 95, g_pc);
	stbtt_PackEnd(&pc);
	glGenTextures(1, &g_fontTex); glBindTexture(GL_TEXTURE_2D, g_fontTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, AW, AH, 0, GL_RED, GL_UNSIGNED_BYTE, px.data());
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

float TextWidth(const char *s, float scale)
{
	float w = 0;
	for (; *s; s++) if (*s >= 32 && *s < 127) w += g_pc[*s - 32].xadvance * scale;
	return w;
}

// px = cap height in pixels; align: 0 centre on cx, -1 left edge at cx, +1 right edge at cx
void DrawText(const char *s, float cx, float cy, float px, float W, float H, const float *col, float maxw = 1e9f, int align = 0)
{
	float scale = px / CAPH / BAKE;
	float w = TextWidth(s, scale);
	if (w > maxw) { scale *= maxw / w; w = maxw; px = scale * BAKE * CAPH; }
	float x = align == 0 ? cx - w * 0.5f : (align < 0 ? cx : cx - w);
	float base = cy + px * 0.5f;
	glUniform1i(u_mode, 1); glUniform4fv(u_col, 1, col);
	float xp = 0, yp = 0;
	for (; *s; s++) {
		if (*s < 32 || *s >= 127) continue;
		stbtt_aligned_quad q;
		stbtt_GetPackedQuad(g_pc, AW, AH, *s - 32, &xp, &yp, &q, 0);
		if (*s == ' ') continue;
		glUniform4f(u_uv, q.s0, q.t0, q.s1 - q.s0, q.t1 - q.t0);
		glUniform4f(u_rect, x + q.x0 * scale, base + q.y0 * scale, (q.x1 - q.x0) * scale, (q.y1 - q.y0) * scale);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	}
}

bool ReadFileRW(const char *path, std::vector<uint8_t> &out)
{
	SDL_RWops *rw = SDL_RWFromFile(path, "rb");
	if (!rw) return false;
	Sint64 n = SDL_RWsize(rw);
	if (n <= 0 || n > (64 << 20)) { SDL_RWclose(rw); return false; }
	out.resize((size_t)n);
	size_t got = SDL_RWread(rw, out.data(), 1, (size_t)n);
	SDL_RWclose(rw);
	return got == (size_t)n;
}
void LoadWheel()
{
	if (g_wheelTried) return;
	g_wheelTried = true;
	std::vector<uint8_t> buf;
	bool ok = ReadFileRW("wheel.png", buf);
	if (!ok) {
		char *bp = SDL_GetBasePath();
		if (bp) { ok = ReadFileRW((std::string(bp) + "wheel.png").c_str(), buf); SDL_free(bp); }
	}
	if (!ok) {
		char *pp = SDL_GetPrefPath("srally2", "srally2");
		if (pp) { ok = ReadFileRW((std::string(pp) + "wheel.png").c_str(), buf); SDL_free(pp); }
	}
	if (!ok) return;
	int w, h, c;
	stbi_uc *px = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &c, 4);
	if (!px) return;
	glGenTextures(1, &g_wheelTex); glBindTexture(GL_TEXTURE_2D, g_wheelTex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	glGenerateMipmap(GL_TEXTURE_2D);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	stbi_image_free(px);
	g_wheelLoaded = true;
	glBindTexture(GL_TEXTURE_2D, g_fontTex);
}
void DrawWheel(int W, int H)
{
	LoadWheel();
	float cx, cy, R; WheelGeom(W, H, cx, cy, R);
	float ang;
	{ std::lock_guard<std::mutex> l(g_mu); ang = g_wAng; }
	float col[4] = {1, 1, 1, g_wHeld ? 0.95f : 0.72f};
	glUniform4fv(u_col, 1, col);
	glUniform1f(glGetUniformLocation(g_prog, "ang"), ang);
	glUniform4f(u_rect, cx - R, cy - R, 2 * R, 2 * R);
	if (g_wheelLoaded) { glBindTexture(GL_TEXTURE_2D, g_wheelTex); glUniform1i(u_mode, 4); }
	else glUniform1i(u_mode, 3);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	if (g_wheelLoaded) glBindTexture(GL_TEXTURE_2D, g_fontTex);
}
} // namespace

void DrawMenu(int W, int H, const char *title, const char *const *lines, int n, int sel, const char *footer)
{
	if (!g_prog) GlInit();
	GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST), depth = glIsEnabled(GL_DEPTH_TEST);
	GLint prog, tex, vao, au; glGetIntegerv(GL_CURRENT_PROGRAM, &prog); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao); glGetIntegerv(GL_ACTIVE_TEXTURE, &au);
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(g_prog); glBindVertexArray(g_vao); glBindTexture(GL_TEXTURE_2D, g_fontTex);
	glUniform2f(u_scr, (float)W, (float)H);
	glUniform1i(glGetUniformLocation(g_prog, "fnt"), 0);
	float dim[4] = {0, 0, 0, 0.62f};
	glUniform1i(u_mode, 2); glUniform4fv(u_col, 1, dim); glUniform4f(u_rect, 0, 0, (float)W, (float)H);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	float ph = H * 0.042f, step = ph * 1.9f;
	float pw = ph * 22.0f, phh = step * (n + 3.2f);
	if (pw > W * 0.94f) { float s = W * 0.94f / pw; pw *= s; ph *= s; step *= s; phh = step * (n + 3.2f); }
	float px0 = (W - pw) * 0.5f, py0 = (H - phh) * 0.5f;
	float edge[4] = {0.45f, 0.55f, 0.75f, 0.95f}, panel[4] = {0.06f, 0.08f, 0.13f, 0.94f};
	float bd = ph * 0.12f;
	glUniform1i(u_mode, 2); glUniform4fv(u_col, 1, edge); glUniform4f(u_rect, px0 - bd, py0 - bd, pw + 2 * bd, phh + 2 * bd);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glUniform4fv(u_col, 1, panel); glUniform4f(u_rect, px0, py0, pw, phh);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	{ std::lock_guard<std::mutex> l(g_mu); g_mRow0 = py0 + step * 2.0f; g_mStep = step; g_mPx = px0; g_mPw = pw; g_mN = n; }
	float tcol[4] = {1.0f, 0.85f, 0.2f, 1.0f};
	DrawText(title, W * 0.5f, py0 + step * 1.0f, ph * 1.15f, (float)W, (float)H, tcol, pw * 0.9f);
	float pad = pw * 0.05f;
	for (int i = 0; i < n; i++) {
		bool on = i == sel;
		float cy = py0 + step * (2.0f + i);
		if (on) {
			float hl[4] = {1.0f, 0.85f, 0.2f, 0.16f};
			glUniform1i(u_mode, 2); glUniform4fv(u_col, 1, hl);
			glUniform4f(u_rect, px0 + pad * 0.5f, cy - step * 0.5f + step * 0.06f, pw - pad, step * 0.88f);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		}
		float c[4] = {1, 1, 1, 0.88f};
		if (on) { c[0] = 1.0f; c[1] = 0.85f; c[2] = 0.2f; c[3] = 1.0f; }
		char buf[96]; snprintf(buf, sizeof buf, "%s", lines[i]);
		char *tab = strchr(buf, '\t');
		if (tab) {
			*tab = 0;
			DrawText(buf, px0 + pad, cy, ph, (float)W, (float)H, c, pw * 0.5f, -1);
			DrawText(tab + 1, px0 + pw - pad, cy, ph, (float)W, (float)H, c, pw * 0.4f, 1);
		} else DrawText(buf, W * 0.5f, cy, ph, (float)W, (float)H, c, pw * 0.92f);
	}
	float fc[4] = {0.75f, 0.8f, 0.9f, 0.9f};
	DrawText(footer, W * 0.5f, py0 + phh - step * 0.55f, ph * 0.6f, (float)W, (float)H, fc, pw * 0.94f);
	glUseProgram(prog); glBindVertexArray(vao); glBindTexture(GL_TEXTURE_2D, tex); glActiveTexture(au);
	if (!blend) glDisable(GL_BLEND); if (scissor) glEnable(GL_SCISSOR_TEST); if (depth) glEnable(GL_DEPTH_TEST);
}

bool MenuRequested() { return g_menuReq.exchange(false); }
void SetMenuOpen(bool open) { g_menuOpen = open; if (open) { g_menuTap = -1; g_mask = 0; } else { std::lock_guard<std::mutex> l(g_mu); g_mN = 0; } }
bool MenuTap(int &item, int &dir)
{
	int t = g_menuTap.exchange(-1);
	if (t < 0) return false;
	item = t >> 2; dir = (t & 3) == 1 ? -1 : 1;
	return true;
}

void Init(SDL_Window *win, bool force)
{
	g_win = win;
#ifdef __ANDROID__
	g_on = true;
#else
	g_on = force;
#endif
	if (!g_on) return;
	SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
	if (force) SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");   // desktop testing: the mouse acts as a finger
	SDL_AddEventWatch(Watch, nullptr);
}
bool Enabled() { return g_on; }

void SetSteer(int mode, bool invertTilt)
{
	std::lock_guard<std::mutex> l(g_mu);
	if (g_steerMode != mode) { g_wHeld = false; g_wAng = 0; g_tilt = 0; g_swOn = false; g_swV = 0; g_rollInit = false; }
	g_steerMode = mode; g_tiltInv = invertTilt ? 1 : 0;
}

bool AnalogSteer(float &v)
{
	if (!g_on || (g_steerMode != 1 && g_steerMode != 2 && g_steerMode != 4)) return false;
	uint32_t now = SDL_GetTicks();
	float dt = g_lastTick ? (now - g_lastTick) * 0.001f : 0.0166f;
	g_lastTick = now;
	if (dt > 0.1f) dt = 0.1f;
	std::lock_guard<std::mutex> l(g_mu);
	if (g_menuOpen.load()) { v = 0; return true; }
	if (g_steerMode == 4) {
		if (!g_swOn) { g_swV *= expf(-dt * 9.0f); if (fabsf(g_swV) < 0.01f) g_swV = 0; }
		v = g_swV;
	} else if (g_steerMode == 1) {
		if (!g_wHeld) { g_wAng *= expf(-dt * 9.0f); if (fabsf(g_wAng) < 0.4f * DEG) g_wAng = 0; }
		v = g_wAng / FULL;
	} else {
		if (!g_sens && g_sensFirst == 0) g_sensFirst = now ? now : 1;
		if (!g_sens && now - g_sensFirst > 3000) { g_steerMode = 0; fprintf(stderr, "[touch] no accelerometer: falling back to the d-pad buttons\n"); v = 0; return false; }
		if (!g_sens) {   // accelerometer + gyro through SDL's sensor API (the joystick route is switched off on purpose)
			if (now - g_accelTry > 1000 || g_accelTry == 0) {
				g_accelTry = now ? now : 1;
				if (!SDL_WasInit(SDL_INIT_SENSOR)) SDL_InitSubSystem(SDL_INIT_SENSOR);
				for (int i = 0, n = SDL_NumSensors(); i < n; i++) {
					if (!g_sens && SDL_SensorGetDeviceType(i) == SDL_SENSOR_ACCEL) { g_sens = SDL_SensorOpen(i); fprintf(stderr, "[touch] accelerometer %s\n", g_sens ? "ok" : "open failed"); }
					if (!g_gyro && SDL_SensorGetDeviceType(i) == SDL_SENSOR_GYRO) { g_gyro = SDL_SensorOpen(i); fprintf(stderr, "[touch] gyroscope %s\n", g_gyro ? "ok" : "open failed"); }
				}
			}
		}
		float target = 0;
		if (g_sens) {
			float v3[3] = {0, 0, 0};
			if (SDL_SensorGetData(g_sens, v3, 3) == 0) {
				// gravity ("up" reaction) in the screen frame: sxr along the right edge, syu along the top edge
				float sxr, syu;
				switch (SDL_GetDisplayOrientation(0)) {
				case SDL_ORIENTATION_LANDSCAPE_FLIPPED: sxr = v3[1]; syu = -v3[0]; break;
				case SDL_ORIENTATION_PORTRAIT: sxr = v3[0]; syu = v3[1]; break;
				case SDL_ORIENTATION_PORTRAIT_FLIPPED: sxr = -v3[0]; syu = -v3[1]; break;
				default: sxr = -v3[1]; syu = v3[0]; break;     // landscape
				}
				float th = atan2f(-sxr, syu) / DEG;            // roll of the screen, clockwise = positive, any pitch
				float mag = sqrtf(v3[0] * v3[0] + v3[1] * v3[1] + v3[2] * v3[2]);
				float gz = 0; bool haveGyro = false;
				if (g_gyro) { float w3[3]; if (SDL_SensorGetData(g_gyro, w3, 3) == 0) { gz = w3[2]; haveGyro = true; } }
				if (!g_rollInit) { g_roll = th; g_rollInit = true; }
				else if (haveGyro) {   // complementary filter: the gyro gives the fast, shake-proof motion, gravity pins the long term
					g_roll += -gz / DEG * dt;
					float k = 1.0f - expf(-dt / 0.6f);
					if (fabsf(mag - 9.80665f) > 2.0f) k *= 0.05f;   // accelerating: gravity direction is unreliable
					float e = th - g_roll; if (e > 180) e -= 360; else if (e < -180) e += 360;
					g_roll += e * k;
				} else {
					float k = 1.0f - expf(-dt / 0.035f);
					if (fabsf(mag - 9.80665f) > 2.5f) k *= 0.15f;
					g_roll += (th - g_roll) * k;
				}
				float deg = g_roll - g_tiltZero;
				if (g_tiltInv) deg = -deg;
				static uint32_t lastLog = 0; if (now - lastLog > 5000) { lastLog = now; fprintf(stderr, "[touch] tilt roll %.1f deg (accel %.1f gyro %s) range %.0f\n", g_roll, th, haveGyro ? "yes" : "no", g_tiltRange); }
				const float dz = 0.8f;
				float m = fabsf(deg) - dz; if (m < 0) m = 0;
				float t = m / (g_tiltRange - dz); if (t > 1) t = 1;
				target = deg < 0 ? -t : t;                      // strictly linear, one to one
			}
		}
		g_tilt = target;
		v = g_tilt;
		g_wAng = v * FULL;
	}
	v = v < -1 ? -1 : (v > 1 ? 1 : v);
	return true;
}


void SetAnalogPedals(bool on) { g_apedal = on; }
void ResetGear() { g_gear = 1; }
bool PedalValues(float &gas, float &brake) { if (!g_on) return false; gas = g_gasA.load(); brake = g_brkA.load(); return true; }
void SetHideRace(bool on) { g_hideRace = on; }
bool MenuDrag(float &dx) { std::lock_guard<std::mutex> l(g_mu); if (!g_mdOn) return false; float aw = g_wh * 4.0f / 3.0f; dx = (g_mdX - g_mdX0) * g_ww / aw; return true; }
void SetTilt(float rangeDeg, float zeroDeg) { std::lock_guard<std::mutex> l(g_mu); g_tiltRange = rangeDeg < 10 ? 10 : rangeDeg; g_tiltZero = zeroDeg; }
void TiltCalibrate(float roll) { std::lock_guard<std::mutex> l(g_mu); g_tiltZero = roll; fprintf(stderr, "[touch] tilt neutral set to %.1f deg\n", roll); }
float TiltRoll() { std::lock_guard<std::mutex> l(g_mu); return g_roll; }
void PulseExtra() { g_extraUntil = SDL_GetTicks() + 180; }
void SetGameMenu(bool on) { g_gameMenu = on; if (!on) g_ev = 0; }
bool PopMenuEvent(int &kind, float &x, float &y) { int k = g_ev.exchange(0); if (!k) return false; kind = k; x = g_evX; y = g_evY; return true; }

#ifndef __ANDROID__
static int s_devFrame = 0;
void DevFrame() { s_devFrame++; }
#endif
bool KeyDown(int sc)
{
#ifndef __ANDROID__
	if (const char *ks = getenv("M3_KEYSCRIPT")) { const char *q = ks; while (*q) { int f = 0, c = 0, n = 0; if (sscanf(q, "%d:%d:%d", &f, &c, &n) == 3 && c == sc && s_devFrame >= f && s_devFrame < f + n) return true; const char *cm = strchr(q, ','); if (!cm) break; q = cm + 1; } }   // dev: scripted keys "frame:scancode:frames,..."
#endif
	if (!g_on || g_menuOpen.load() || Pad::HidesTouch()) return false;
	if (sc == SDL_SCANCODE_6 && (int32_t)(g_testUntil.load() - SDL_GetTicks()) > 0) return true;
	if (sc == SDL_SCANCODE_5 && (int32_t)(g_servUntil.load() - SDL_GetTicks()) > 0) return true;
	if ((sc == SDL_SCANCODE_A || sc == SDL_SCANCODE_S) && (int32_t)(g_extraUntil.load() - SDL_GetTicks()) > 0) return true;
	uint32_t m = g_mask.load();
	for (int b = 0; b < NBTN; b++) if ((m >> b) & 1 && defs[b].key == sc && b != GAS && b != BRAKE) return true;   // pedals are analog (PedalValues)
	int g = g_gear.load();
	static const int gk[5] = {SDL_SCANCODE_T, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R};
	return g >= 1 && g <= 4 && sc == gk[g];
}

void Draw(int W, int H)
{
	if (!g_on) return;
	{ std::lock_guard<std::mutex> l(g_mu); g_ww = W; g_wh = H; if (!g_fingers.empty()) Pad::NoteTouch(); }
	static int dbg = getenv("M3_TOUCHDBG") ? atoi(getenv("M3_TOUCHDBG")) : 0;
	if (dbg == 1) return;
	if (!g_prog) GlInit();
	if (dbg == 2) return;
	GLboolean blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST), depth = glIsEnabled(GL_DEPTH_TEST);
	GLint prog, tex, vao, au; glGetIntegerv(GL_CURRENT_PROGRAM, &prog); glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao); glGetIntegerv(GL_ACTIVE_TEXTURE, &au);
	glActiveTexture(GL_TEXTURE0);
	glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	glUseProgram(g_prog); glBindVertexArray(g_vao); glBindTexture(GL_TEXTURE_2D, g_fontTex);
	glUniform2f(u_scr, (float)W, (float)H);
	glUniform1i(glGetUniformLocation(g_prog, "fnt"), 0);
	uint32_t m = g_mask.load();
	for (int b = 0; b < NBTN; b++) {
		if (g_menuOpen.load() || Pad::HidesTouch()) break;
		if (g_steerMode && (b == LEFT || b == RIGHT)) continue;
		if (g_hideRace.load() && b <= HAND) continue;
		if ((b == BACK || b == TEST || b == SERV) && !g_hideRace.load()) continue;   // race: no BACK, and TEST / SERVICE wait outside the race (clear of the HUD)
		bool on = (m >> b) & 1;
		Rect r = Layout(b, W, H);
		float col[4] = {0.10f, 0.12f, 0.16f, on ? 0.85f : 0.45f};
		if (b == GAS) { col[0] = 0.10f; col[1] = 0.55f; col[2] = 0.20f; }
		if (b == BRAKE) { col[0] = 0.65f; col[1] = 0.12f; col[2] = 0.12f; }
		if (b == TEST || b == SERV) { col[0] = 0.25f; col[1] = 0.25f; col[2] = 0.28f; col[3] = on ? 0.85f : 0.28f; }
		if (b == BACK) { col[0] = 0.55f; col[1] = 0.25f; col[2] = 0.20f; col[3] = on ? 0.9f : 0.55f; }
		if (b == MENU) { col[0] = 0.20f; col[1] = 0.25f; col[2] = 0.40f; col[3] = on ? 0.85f : 0.28f; }
		if (b == COIN || b == START) { col[0] = 0.55f; col[1] = 0.45f; col[2] = 0.05f; }
		glUniform1i(u_mode, 0); glUniform4fv(u_col, 1, col); glUniform4f(u_rect, r.x, r.y, r.w, r.h);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		float tc[4] = {1, 1, 1, on ? 1.0f : 0.8f};
		float ph = r.h * ((b == LEFT || b == RIGHT || b == SHUP || b == SHDN) ? 0.45f : 0.30f);
		if (b == HAND) ph = r.h * 0.26f;
		if (b == GAS || b == BRAKE) {   // travel bar
			float v = b == GAS ? g_gasA.load() : g_brkA.load();
			if (on && v > 0) { float fc[4] = {col[0] + 0.25f, col[1] + 0.25f, col[2] + 0.25f, 0.85f}; float fh = r.h * v; glUniform4fv(u_col, 1, fc); glUniform4f(u_rect, r.x, r.y + r.h - fh, r.w, fh); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
		}
		DrawText(defs[b].label, r.x + r.w * 0.5f, r.y + r.h * 0.5f, ph, (float)W, (float)H, tc, r.w * 0.82f);
	}
	if ((g_steerMode == 1 || g_steerMode == 2) && !g_menuOpen.load() && !g_hideRace.load() && !Pad::HidesTouch()) DrawWheel(W, H);
	if (!g_menuOpen.load() && !g_hideRace.load() && !Pad::HidesTouch())
	{   // current gear indicator between the shift buttons
		Rect a = Layout(SHDN, W, H), c = Layout(SHUP, W, H);
		char t[8]; int g = g_gear.load(); snprintf(t, sizeof t, "%d", g);
		float tc[4] = {1, 1, 1, 0.95f};
		DrawText(t, (a.x + a.w + c.x) * 0.5f, a.y + a.h * 0.5f, a.h * 0.45f, (float)W, (float)H, tc, 0.13f * H);
	}
	glUseProgram(prog); glBindVertexArray(vao); glBindTexture(GL_TEXTURE_2D, tex); glActiveTexture(au);
	if (!blend) glDisable(GL_BLEND); if (scissor) glEnable(GL_SCISSOR_TEST); if (depth) glEnable(GL_DEPTH_TEST);
}

} // namespace Touch
