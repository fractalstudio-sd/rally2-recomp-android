// Persistent display settings and the in-game settings menu. GPLv3.
#include "settings.h"
#include "touch.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace Settings {
namespace {
const double kAspect[] = {0.0, 4.0 / 3.0, 16.0 / 10.0, 16.0 / 9.0, 21.0 / 9.0};
const char *kAspectName[] = {"Auto", "4:3", "16:10", "16:9", "21:9"};
const int kWinH[] = {0, 720, 900, 1080, 1440};
const int kIres[] = {-1, 0, 480, 720, 1080, 1440, 2160};   // -1 auto (adaptive), 0 native (fixed)
const int NA = 5, NW = 5, NI = 7;
enum Item { MODE, ASPECT, WINH, IRES, VSYNC, STEER, TINV, TRANGE, TZERO, FEEL, PEDALS, FFB, APPLY_ITEM, CANCEL_ITEM, LEADER_ITEM, NITEMS };

bool g_open = false;
std::string g_lbUrl;       // online leaderboard page (empty: not configured)
int g_sel = 0;
Values g_start, g_pend;
std::vector<int> g_items;
#ifdef __ANDROID__
int g_steer = 0;          // new installs: the digital arrows
#else
int g_steer = 0;
#endif          // 0 directional/keys, 1 touch wheel, 2 tilt, 3 mouse
int g_tinv = 0;
const int kTRange[] = {25, 30, 40, 50, 60};
int g_trange = 2;          // index into kTRange: tilt angle for full lock
int g_tzero = 0;           // tilt neutral, tenths of a degree
#ifdef __ANDROID__
int g_feel = 1;
#else
int g_feel = 0;
#endif            // steering response: 0 arcade (linear), 1 soft, 2 softer (calmer around the centre)
int g_ffb = 2;             // vibration force feedback: 0 off, 1 low, 2 medium, 3 high
int g_apedal = 0;          // touch pedals: 0 on/off (a touch is full pedal), 1 pressure by thumb position
void ApplyTilt() { Touch::SetTilt((float)kTRange[g_trange], g_tzero * 0.1f); }
#ifdef __ANDROID__
const int kSteerModes[] = {0, 1, 2, 4};
const char *kSteerName[] = {"D-pad", "Steering wheel", "Tilt", "Mouse", "Swipe tracking"};
#else
const int kSteerModes[] = {0, 3, 1};
const char *kSteerName[] = {"Keyboard / pad", "Steering wheel", "Tilt", "Mouse", "Swipe tracking"};
#endif
#ifdef __ANDROID__
int NSteer() { int n = 2; if (Touch::Enabled()) n = 4; return n; }
#else
int NSteer() { int n = 2; if (Touch::Enabled()) n = 3; return n; }
#endif
void Rebuild()
{
	g_items.clear();
#ifndef __ANDROID__
	g_items.push_back(MODE);
#endif
	g_items.push_back(ASPECT);
#ifndef __ANDROID__
	g_items.push_back(WINH);
#endif
	g_items.push_back(IRES); g_items.push_back(VSYNC); g_items.push_back(STEER);
	if (g_steer == 1 || g_steer == 2 || g_steer == 4) g_items.push_back(FEEL);
	if (g_steer == 2) { g_items.push_back(TINV); g_items.push_back(TRANGE); g_items.push_back(TZERO); }
#ifdef __ANDROID__
	g_items.push_back(PEDALS);
	g_items.push_back(FFB);
#endif
	if (!g_lbUrl.empty()) g_items.push_back(LEADER_ITEM);
	g_items.push_back(APPLY_ITEM); g_items.push_back(CANCEL_ITEM);
	if (g_sel >= (int)g_items.size()) g_sel = (int)g_items.size() - 1;
}

int Find(const int *tab, int n, int v) { for (int i = 0; i < n; i++) if (tab[i] == v) return i; return 0; }

std::string Path()
{
	char *p = SDL_GetPrefPath("srally2", "srally2");
	std::string s = p ? std::string(p) + "settings.txt" : std::string();
	if (p) SDL_free(p);
	return s;
}

void Cycle(int item, int dir)
{
	switch (item) {
	case MODE: g_pend.fullscreen ^= 1; break;
	case ASPECT: g_pend.aspect = (g_pend.aspect + dir + NA) % NA; break;
	case WINH: { int i = (Find(kWinH, NW, g_pend.winh) + dir + NW) % NW; g_pend.winh = kWinH[i]; } break;
	case IRES: { int i = (Find(kIres, NI, g_pend.ires) + dir + NI) % NI; g_pend.ires = kIres[i]; } break;
	case VSYNC: g_pend.vsync ^= 1; break;
	case STEER: {
		int n = NSteer(), i = 0; for (int k = 0; k < n; k++) if (kSteerModes[k] == g_steer) i = k;
		g_steer = kSteerModes[(i + dir + n) % n]; Touch::SetSteer(g_steer, g_tinv); Rebuild();
	} break;
	case TINV: g_tinv ^= 1; Touch::SetSteer(g_steer, g_tinv); break;
	case TRANGE: g_trange = (g_trange + dir + 5) % 5; ApplyTilt(); break;
	case TZERO: g_tzero = (int)lrintf(Touch::TiltRoll() * 10.0f + g_tinv * 0.0f); ApplyTilt(); break;   // hold the phone the way you drive, then tap
	case FEEL: g_feel = (g_feel + dir + 3) % 3; break;
	case FFB: g_ffb = (g_ffb + dir + 4) % 4; break;
	case PEDALS: g_apedal ^= 1; Touch::SetAnalogPedals(g_apedal != 0); break;
	}
}
} // namespace

double AspectValue(int idx) { return (idx >= 0 && idx < NA) ? kAspect[idx] : 0.0; }

bool Load(Values &v)
{
	std::string p = Path(); if (p.empty()) return false;
	FILE *f = fopen(p.c_str(), "r"); if (!f) return false;
	char k[64]; int x; int oldIres = -2;
	while (fscanf(f, "%63[^=]=%d\n", k, &x) == 2) {
		if (!strcmp(k, "fullscreen")) v.fullscreen = x ? 1 : 0;
		else if (!strcmp(k, "aspect") && x >= 0 && x < NA) v.aspect = x;
		else if (!strcmp(k, "winh")) v.winh = x;
		else if (!strcmp(k, "ires")) oldIres = x;                                  // older files: 0 meant adaptive
		else if (!strcmp(k, "ires2")) { v.ires = x; v.iresSet = true; }
		else if (!strcmp(k, "vsync")) v.vsync = x ? 1 : 0;
		else if (!strcmp(k, "trange") && x >= 0 && x < 5) g_trange = x;
		else if (!strcmp(k, "tzero")) g_tzero = x;
		else if (!strcmp(k, "feel") && x >= 0 && x < 3) g_feel = x;
		else if (!strcmp(k, "ffb") && x >= 0 && x < 4) g_ffb = x;
		else if (!strcmp(k, "steer")) { for (int m : kSteerModes) if (m == x) g_steer = x; }
		else if (!strcmp(k, "tinv")) g_tinv = x ? 1 : 0;
		else if (!strcmp(k, "apedal")) g_apedal = x ? 1 : 0;
	}
	fclose(f);
	if (!v.iresSet && oldIres > 0) v.ires = oldIres;
	return true;
}

bool Save(const Values &v)
{
	std::string p = Path(); if (p.empty()) return false;
	FILE *f = fopen(p.c_str(), "w"); if (!f) return false;
	fprintf(f, "fullscreen=%d\naspect=%d\nwinh=%d\nires2=%d\nvsync=%d\nsteer=%d\ntinv=%d\ntrange=%d\ntzero=%d\nfeel=%d\nffb=%d\napedal=%d\n", v.fullscreen, v.aspect, v.winh, v.ires, v.vsync, g_steer, g_tinv, g_trange, g_tzero, g_feel, g_ffb, g_apedal);
	fclose(f);
	return true;
}

void Init(const Values &current)
{
	g_start = current; g_pend = current;
	Touch::SetSteer(g_steer, g_tinv); ApplyTilt(); Touch::SetAnalogPedals(g_apedal != 0);
	Rebuild();
}
int FFBLevel() { return g_ffb; }
void SetLeaderboardUrl(const std::string &u) { g_lbUrl = u; Rebuild(); }
int SteerFeel() { return g_feel; }
bool IsOpen() { return g_open; }
const Values &Pending() { return g_pend; }
int SteerMode() { return ((g_steer == 1 || g_steer == 4) && !Touch::Enabled()) ? 0 : g_steer; }

Action Update(const Uint8 *ks)
{
	static bool prev[SDL_NUM_SCANCODES] = {};
	static const int watched[] = {SDL_SCANCODE_F2, SDL_SCANCODE_TAB, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT,
		SDL_SCANCODE_RIGHT, SDL_SCANCODE_RETURN, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_AC_BACK};
	bool edge[SDL_NUM_SCANCODES] = {};
	for (int sc : watched) { edge[sc] = ks[sc] && !prev[sc]; prev[sc] = ks[sc] != 0; }
	const int n = (int)g_items.size();
	if (!g_open) {
		bool req = Touch::MenuRequested();
		if (edge[SDL_SCANCODE_F2] || edge[SDL_SCANCODE_TAB] || req) { g_open = true; g_sel = 0; g_pend = g_start; Touch::SetMenuOpen(true); return OPENED; }
		return NONE;
	}
	auto close = [&](Action a) { g_open = false; Touch::SetMenuOpen(false); if (a != APPLY) Save(g_start); return a; };
	if (edge[SDL_SCANCODE_F2] || edge[SDL_SCANCODE_TAB] || edge[SDL_SCANCODE_ESCAPE] || edge[SDL_SCANCODE_AC_BACK]) return close(CLOSED);
	int ti, td;
	if (Touch::MenuTap(ti, td) && ti >= 0 && ti < n) {
		g_sel = ti;
		int id = g_items[ti];
		if (id == APPLY_ITEM) return close(APPLY);
		if (id == CANCEL_ITEM) return close(CLOSED);
		if (id == LEADER_ITEM) { SDL_OpenURL(g_lbUrl.c_str()); return NONE; }
		Cycle(id, td);
		return NONE;
	}
	if (edge[SDL_SCANCODE_UP]) g_sel = (g_sel + n - 1) % n;
	if (edge[SDL_SCANCODE_DOWN]) g_sel = (g_sel + 1) % n;
	int cur = g_items[g_sel];
	if (edge[SDL_SCANCODE_LEFT] && cur < APPLY_ITEM) Cycle(cur, -1);
	if (edge[SDL_SCANCODE_RIGHT] && cur < APPLY_ITEM) Cycle(cur, +1);
	if (edge[SDL_SCANCODE_RETURN]) {
		if (cur == APPLY_ITEM) return close(APPLY);
		if (cur == CANCEL_ITEM) return close(CLOSED);
		if (cur == LEADER_ITEM) { SDL_OpenURL(g_lbUrl.c_str()); return NONE; }
		if (cur < APPLY_ITEM) Cycle(cur, +1);
	}
	return NONE;
}

void Draw(int W, int H)
{
	if (!g_open) return;
	char line[NITEMS][64];
	snprintf(line[MODE], 64, "Mode\t< %s >", g_pend.fullscreen ? "Fullscreen" : "Windowed");
	snprintf(line[ASPECT], 64, "Aspect ratio\t< %s >", kAspectName[g_pend.aspect]);
	if (g_pend.winh) snprintf(line[WINH], 64, "Window height\t< %d >", g_pend.winh); else snprintf(line[WINH], 64, "Window height\t< Auto >");
	if (g_pend.ires > 0) snprintf(line[IRES], 64, "Render height\t< %d >", g_pend.ires);
	else snprintf(line[IRES], 64, "Render height\t< %s >", g_pend.ires == 0 ? "Native (fixed)" : "Auto (adaptive)");
	snprintf(line[VSYNC], 64, "VSync\t< %s >", g_pend.vsync ? "On" : "Off");
	snprintf(line[STEER], 64, "Steering\t< %s >", kSteerName[g_steer]);
	snprintf(line[TINV], 64, "Tilt direction\t< %s >", g_tinv ? "Inverted" : "Normal");
	{ static const char *fe[] = {"Arcade (linear)", "Soft", "Softer"}; snprintf(line[FEEL], 64, "Steering feel\t< %s >", fe[g_feel]); }
	snprintf(line[TRANGE], 64, "Tilt full lock\t< %d deg >", kTRange[g_trange]);
	snprintf(line[TZERO], 64, "Tilt neutral\t< auto / tap to set >");
	snprintf(line[PEDALS], 64, "Pedal sensitivity\t< %s >", g_apedal ? "On" : "Off");
	{ static const char *fn[] = {"Off", "Low", "Medium", "High"}; snprintf(line[FFB], 64, "Vibration (FFB)\t< %s >", fn[g_ffb]); }
	snprintf(line[APPLY_ITEM], 64, "Apply and restart");
	snprintf(line[CANCEL_ITEM], 64, "Cancel");
	snprintf(line[LEADER_ITEM], 64, "Online leaderboard\t>");
	const char *ptrs[NITEMS];
	int n = (int)g_items.size();
	for (int i = 0; i < n; i++) ptrs[i] = line[g_items[i]];
#ifdef __ANDROID__
	Touch::DrawMenu(W, H, "Settings", ptrs, n, g_sel, "Tap a row to change   Back closes");
#else
	Touch::DrawMenu(W, H, "Settings", ptrs, n, g_sel, "Up/Down select   Left/Right change   Enter OK   Esc close");
#endif
}
} // namespace Settings
