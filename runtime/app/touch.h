// On-screen touch controls: multitouch buttons drawn over the picture, injected as virtual keyboard keys.
#pragma once
#include <SDL.h>

namespace Touch {
void Init(SDL_Window *win, bool force);    // force: also on desktop (mouse acts as a finger)
bool Enabled();
#ifndef __ANDROID__
void DevFrame();   // dev: frame counter for M3_KEYSCRIPT
#endif
void SetSteer(int mode, bool invertTilt);  // 0 = d-pad buttons, 1 = on-screen wheel, 2 = tilt (wheel shown as feedback)
void SetAnalogPedals(bool on);
void ResetGear();                            // lever back to 1st (a new race)              // pedal sensitivity (thumb position = pressure) on/off
bool AnalogSteer(float &v);                // wheel/tilt only: steering -1..1 (left..right); false when not in an analog touch mode
void SetGameMenu(bool on);
bool PedalValues(float &gas, float &brake);   // analog pedals from the on-screen pads (0..1)
void SetHideRace(bool on);   // hide/disable race-only controls (game menus)
bool MenuDrag(float &dx);    // a finger is dragging in a game menu: horizontal offset in 4:3-picture widths
void SetTilt(float rangeDeg, float zeroDeg);   // tilt: full-lock angle and neutral roll (deg)
void TiltCalibrate(float roll);                // set the neutral roll (deg)
float TiltRoll();                              // current filtered roll (deg), for calibrating the neutral
void PulseExtra();   // View+Handbrake pulse (extra cars page)
bool PopMenuEvent(int &kind, float &x, float &y);   // gesture made in a game menu: kind 1 = tap at (x,y) in the 4:3 picture's 0..1 coordinates, 2 = swipe left, 3 = swipe right                 // main loop: the game is in one of its own menus (a swipe then pulses View + Handbrake for the extra cars)
bool KeyDown(int scancode);                // OR-ed into the keyboard state by the SDL input system
void Draw(int winW, int winH);
bool MenuRequested();                      // true once per press of the on-screen MENU button
void SetMenuOpen(bool open);               // while open the game buttons are hidden and taps go to the menu
bool MenuTap(int &item, int &dir);         // consume a tap on a menu row (item index, dir -1 = left part, +1 = right part)
void DrawMenu(int W, int H, const char *title, const char *const *lines, int n, int sel, const char *footer);             // call after the frame was blitted, before the swap
}
