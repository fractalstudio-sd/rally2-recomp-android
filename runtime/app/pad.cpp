// Gamepad support. GPLv3.
#include "pad.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace Pad {
namespace {
enum Func { F_VIEW, F_HAND, F_SHUP, F_SHDN, F_START, F_COIN, F_TEST, F_SERVICE, NF };
const char *kFuncName[NF] = {"view", "handbrake", "shiftup", "shiftdown", "start", "coin", "test", "service"};
enum { ST, GA, BR, NAX };
const char *kAxName[NAX] = {"steer", "gas", "brake"};

struct Src { int kind = 0; int id = 0; };   // kind 1 button, 2 axis (positive half), 3 axis (negative half)
struct Bind { std::vector<Src> s; };        // any source pressed = on; a source list entry with id == -1 and kind 4 is "start+button" handled below
struct Pair { SDL_GameController *c; SDL_JoystickID id; };

std::vector<Pair> g_pads;
Bind g_f[NF];
Src g_ax[NAX];            // steer: signed axis; gas/brake: positive half
bool g_axSet[NAX] = {false, false, false};
bool g_on[NF] = {};
bool g_dir[4] = {};       // left right up down (d-pad)
float g_v[NAX] = {};
bool g_vOk[NAX] = {};
int g_gear = 0; bool g_shifted = false;
bool g_prevUp = false, g_prevDn = false;
uint32_t g_lastPad = 0, g_lastTouch = 0; int g_frame = 0;
float g_dead = 0.10f;

bool ParseSrc(const char *t, Src &o)
{
	while (*t == ' ') t++;
	int kind = 0;
	if (*t == '+') { kind = 2; t++; } else if (*t == '-') { kind = 3; t++; }
	std::string n(t); while (!n.empty() && (n.back() == ' ' || n.back() == '\r' || n.back() == '\n')) n.pop_back();
	SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(n.c_str());
	if (a != SDL_CONTROLLER_AXIS_INVALID) { o.kind = kind ? kind : 2; o.id = (int)a; return true; }
	SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(n.c_str());
	if (b != SDL_CONTROLLER_BUTTON_INVALID) { o.kind = 1; o.id = (int)b; return true; }
	return false;
}

float AxisVal(const Src &s)   // -1..1 (triggers 0..1)
{
	float best = 0;
	for (auto &p : g_pads) {
		float v = SDL_GameControllerGetAxis(p.c, (SDL_GameControllerAxis)s.id) / 32767.0f;
		if (v > 1) v = 1; if (v < -1) v = -1;
		if (fabsf(v) > fabsf(best)) best = v;
	}
	return best;
}
bool ButtonVal(int b) { for (auto &p : g_pads) if (SDL_GameControllerGetButton(p.c, (SDL_GameControllerButton)b)) return true; return false; }
bool SrcOn(const Src &s)
{
	if (s.kind == 1) return ButtonVal(s.id);
	float v = AxisVal(s);
	return s.kind == 2 ? v > 0.5f : v < -0.5f;
}
bool BindOn(const Bind &b) { for (auto &s : b.s) if (SrcOn(s)) return true; return false; }

std::string Dir() { char *p = SDL_GetPrefPath("srally2", "srally2"); std::string s = p ? p : ""; if (p) SDL_free(p); return s; }

void Defaults()
{
	auto set = [](Func f, std::initializer_list<const char *> l) { g_f[f].s.clear(); for (auto n : l) { Src s; if (ParseSrc(n, s)) g_f[f].s.push_back(s); } };
	set(F_VIEW, {"a"}); set(F_HAND, {"x", "b"}); set(F_SHUP, {"rightshoulder"}); set(F_SHDN, {"leftshoulder"});
	set(F_START, {"start"}); set(F_COIN, {"back"});
	ParseSrc("leftx", g_ax[ST]); ParseSrc("righttrigger", g_ax[GA]); ParseSrc("lefttrigger", g_ax[BR]);
	g_axSet[ST] = g_axSet[GA] = g_axSet[BR] = true;
	g_dead = 0.10f;
}

void LoadIni()
{
	std::string p = Dir() + "pad.ini"; FILE *f = fopen(p.c_str(), "r"); if (!f) return;
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '#' || line[0] == ';') continue;
		char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
		std::string k(line), v(eq + 1);
		while (!k.empty() && k.back() == ' ') k.pop_back();
		if (k == "deadzone") { g_dead = (float)atof(v.c_str()); if (g_dead < 0) g_dead = 0; if (g_dead > 0.5f) g_dead = 0.5f; continue; }
		for (int a = 0; a < NAX; a++) if (k == kAxName[a]) {
			Src s; std::string t = v; if (a != ST && !t.empty() && t[0] != '+' && t[0] != '-') t = "+" + t;
			// the digit-less names (e.g. "righttrigger") are positive halves; "none" disables the pad on that control
			if (v.compare(0, 4, "none") == 0) { g_axSet[a] = false; break; }
			if (ParseSrc(a == ST ? v.c_str() : t.c_str(), s)) { g_ax[a] = s; g_axSet[a] = true; }
		}
		for (int i = 0; i < NF; i++) if (k == kFuncName[i]) {
			g_f[i].s.clear(); std::string rest = v;
			size_t pos = 0;
			while (pos <= rest.size()) {
				size_t c = rest.find(',', pos); std::string tok = rest.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
				Src s; if (ParseSrc(tok.c_str(), s)) g_f[i].s.push_back(s);
				if (c == std::string::npos) break; pos = c + 1;
			}
		}
	}
	fclose(f);
}

// a raw joystick SDL has no mapping for: assume the usual layout (Xbox-like order, 4 axes + 2 triggers, first hat = d-pad)
void MapUnknown(int idx)
{
	SDL_Joystick *j = SDL_JoystickOpen(idx); if (!j) return;
	int na = SDL_JoystickNumAxes(j), nb = SDL_JoystickNumButtons(j), nh = SDL_JoystickNumHats(j);
	char guid[40]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof guid);
	std::string name = SDL_JoystickName(j) ? SDL_JoystickName(j) : "Generic pad";
	for (auto &ch : name) if (ch == ',') ch = ' ';
	SDL_JoystickClose(j);
	std::string m = std::string(guid) + "," + name;
	char b[64];
	static const char *bn[] = {"a", "b", "x", "y", "leftshoulder", "rightshoulder", "back", "start", "leftstick", "rightstick"};
	for (int i = 0; i < nb && i < 10; i++) { snprintf(b, sizeof b, ",%s:b%d", bn[i], i); m += b; }
	if (na >= 2) m += ",leftx:a0,lefty:a1";
	if (na >= 4) m += ",rightx:a2,righty:a3";
	if (na >= 6) m += ",lefttrigger:a4,righttrigger:a5";
	if (nh >= 1) m += ",dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8";
	m += ",platform:" + std::string(SDL_GetPlatform());
	SDL_GameControllerAddMapping(m.c_str());
}

void Rescan()
{
	for (size_t i = 0; i < g_pads.size();) {
		if (!SDL_GameControllerGetAttached(g_pads[i].c)) { SDL_GameControllerClose(g_pads[i].c); g_pads.erase(g_pads.begin() + i); }
		else i++;
	}
	int n = SDL_NumJoysticks();
	for (int i = 0; i < n; i++) {
		if (!SDL_IsGameController(i)) MapUnknown(i);
		if (!SDL_IsGameController(i)) continue;
		SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
		bool have = false; for (auto &p : g_pads) if (p.id == id) have = true;
		if (have) continue;
		SDL_GameController *c = SDL_GameControllerOpen(i);
		if (c) { g_pads.push_back({c, id}); fprintf(stderr, "[pad] connected: %s\n", SDL_GameControllerName(c)); }
	}
}

float Shape(float v)
{
	float a = fabsf(v); if (a <= g_dead) return 0;
	a = (a - g_dead) / (1.0f - g_dead); if (a > 1) a = 1;
	return v < 0 ? -a : a;
}
}

void Init()
{
	Defaults();
	std::string d = Dir();
	if (!d.empty()) SDL_GameControllerAddMappingsFromFile((d + "gamecontrollerdb.txt").c_str());
	LoadIni();
	Rescan();
}

int Count() { return (int)g_pads.size(); }

void Update()
{
	if ((g_frame++ % 60) == 0) Rescan();
	if (g_pads.empty()) { for (int i = 0; i < NF; i++) g_on[i] = false; for (int i = 0; i < 4; i++) g_dir[i] = false; for (int i = 0; i < NAX; i++) g_vOk[i] = false; return; }
	bool any = false;
	for (int i = 0; i < NF; i++) { g_on[i] = BindOn(g_f[i]); any |= g_on[i]; }
	// TEST / SERVICE: hold START and click a stick (unless rebound in pad.ini); START is then not reported
	if (g_f[F_TEST].s.empty()) { bool st = ButtonVal(SDL_CONTROLLER_BUTTON_START); g_on[F_TEST] = st && ButtonVal(SDL_CONTROLLER_BUTTON_LEFTSTICK); if (g_on[F_TEST]) g_on[F_START] = false; }
	if (g_f[F_SERVICE].s.empty()) { bool st = ButtonVal(SDL_CONTROLLER_BUTTON_START); g_on[F_SERVICE] = st && ButtonVal(SDL_CONTROLLER_BUTTON_RIGHTSTICK); if (g_on[F_SERVICE]) g_on[F_START] = false; }
	g_dir[0] = ButtonVal(SDL_CONTROLLER_BUTTON_DPAD_LEFT); g_dir[1] = ButtonVal(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
	g_dir[2] = ButtonVal(SDL_CONTROLLER_BUTTON_DPAD_UP);   g_dir[3] = ButtonVal(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
	for (int i = 0; i < 4; i++) any |= g_dir[i];
	// analog controls
	for (int a = 0; a < NAX; a++) {
		g_vOk[a] = false; if (!g_axSet[a]) continue;
		float v = AxisVal(g_ax[a]);
		if (a == ST) { v = Shape(v); if (v != 0) { g_v[a] = v; g_vOk[a] = true; any = true; } }
		else {
			if (g_ax[a].kind == 3) v = -v;
			if (g_ax[a].kind == 1) v = 0;
			// a trigger that SDL reports from -1 (rest) must not read as pressed: only the positive half counts
			float t = v > 0.04f ? (v - 0.04f) / 0.96f : 0; if (t > 1) t = 1;
			if (t > 0) { g_v[a] = t; g_vOk[a] = true; any = true; }
		}
	}
	// buttons as digital gas / steering when an analog source is not set
	if (any) g_lastPad = SDL_GetTicks() | 1;
	// sequential gears: shift down / up on the edge; neutral = 0
	bool up = g_on[F_SHUP], dn = g_on[F_SHDN];
	if (up && !g_prevUp) { g_shifted = true; if (g_gear < 4) g_gear++; }
	if (dn && !g_prevDn) { g_shifted = true; if (g_gear > 0) g_gear--; }
	g_prevUp = up; g_prevDn = dn;
}

bool Steer(float &v) { if (!g_vOk[ST]) return false; v = g_v[ST]; return true; }
bool Gas(float &v) { if (g_vOk[GA]) { v = g_v[GA]; return true; } if (g_dir[2]) { v = 1; return true; } return false; }
bool Brake(float &v) { if (g_vOk[BR]) { v = g_v[BR]; return true; } if (g_dir[3]) { v = 1; return true; } return false; }

bool KeyDown(int sc)
{
	if (g_pads.empty()) return false;
	switch (sc) {
	case SDL_SCANCODE_A: return g_on[F_VIEW];
	case SDL_SCANCODE_S: return g_on[F_HAND];
	case SDL_SCANCODE_1: return g_on[F_START];
	case SDL_SCANCODE_3: return g_on[F_COIN];
	case SDL_SCANCODE_6: return g_on[F_TEST];
	case SDL_SCANCODE_5: return g_on[F_SERVICE];
	case SDL_SCANCODE_LEFT: return g_dir[0];
	case SDL_SCANCODE_RIGHT: return g_dir[1];
	}
	static const int gk[5] = {SDL_SCANCODE_T, SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R};
	return g_shifted && sc == gk[g_gear];
}

bool HidesTouch() { return !g_pads.empty() && g_lastPad != 0 && (int32_t)(g_lastPad - g_lastTouch) > 0; }
void NoteTouch() { g_lastTouch = SDL_GetTicks() | 1; }
}
