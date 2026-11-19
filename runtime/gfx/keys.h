// fastball, our own: the keyboard as code outside the window's event loop polls it (hid.c's keyboard controls, the
// diagnostic keys F8, F9 and F12). The Win32 window answers with GetAsyncKeyState (gfx_window_win.c), the SDL3 window
// from SDL's keyboard state and the key presses its event loop saw (gfx_window_sdl.c). Keys are numbered as Windows'
// virtual keys: a letter or digit is its character ('A', '1'), the rest are below. Any thread.
#pragma once

enum {
    KEY_BACK = 0x08, KEY_TAB = 0x09, KEY_RETURN = 0x0D, KEY_ESCAPE = 0x1B, KEY_SPACE = 0x20,
    KEY_LEFT = 0x25, KEY_UP = 0x26, KEY_RIGHT = 0x27, KEY_DOWN = 0x28,
    KEY_F1 = 0x70, KEY_F8 = 0x77, KEY_F9 = 0x78, KEY_F12 = 0x7B,
};

int wsys_focused(void);            // a window of this process has the keyboard focus
int wsys_key_down(int key);        // 1 while key is held, focus or not: for a caller that checked wsys_focused once
// 1 while key is held and a window of this process has the keyboard focus.
int wsys_key_held(int key);
// The same, or pressed since the last call for that key (a tap shorter than the polling interval still counts).
int wsys_key_tapped(int key);
