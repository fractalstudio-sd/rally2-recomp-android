// Persistent display settings and the in-game settings menu (F2 / Tab).
#pragma once
#include <SDL.h>
#include <string>

namespace Settings {
#ifdef __ANDROID__
struct Values { int fullscreen = 1, aspect = 0, winh = 0, ires = -1, vsync = 0; bool iresSet = false; };   // ires: -1 auto (adaptive), 0 native (fixed), N fixed height
#else
struct Values { int fullscreen = 0, aspect = 0, winh = 0, ires = 1080, vsync = 0; bool iresSet = false; };
#endif
enum Action { NONE = 0, OPENED, CLOSED, APPLY };
double AspectValue(int idx);                    // 0 = follow the display
bool Load(Values &v);
bool Save(const Values &v);
void Init(const Values &current);               // values the running instance was started with
Action Update(const Uint8 *keys);               // feed the keyboard state every loop iteration
bool IsOpen();
void Draw(int W, int H);                        // call after the frame was presented into the window
int SteerFeel();                                // 0 arcade (linear), 1 soft, 2 softer
int FFBLevel();                                 // vibration force feedback: 0 off .. 3 high
int SteerMode();                                // 0 = d-pad/keys (default), 1 = touch wheel, 2 = tilt, 3 = mouse
const Values &Pending();
void SetLeaderboardUrl(const std::string &url);   // adds "Online leaderboard" (opens the page) when not empty                        // what APPLY should save
}
