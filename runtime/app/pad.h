// Gamepad support: any SDL game controller (USB or Bluetooth, hot-plugged), unknown pads get a generic mapping.
#pragma once
#include <SDL.h>

namespace Pad {
void Init();                                // load pad.ini / gamecontrollerdb.txt from the pref dir, open attached pads
void Update();                              // once per frame, before the input system is polled
bool KeyDown(int scancode);                 // OR-ed into the keyboard state (buttons, d-pad, gears)
// analog values for this frame (steer -1..1, gas/brake 0..1); each returns false when the pad does not drive that control
bool Steer(float &v);
bool Gas(float &v);
bool Brake(float &v);
bool HidesTouch();                          // a pad is connected and was used after the last touch: hide the overlay
void NoteTouch();                           // a finger touched the screen: show the overlay again
int Count();
}
