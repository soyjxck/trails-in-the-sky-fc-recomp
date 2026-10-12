/* The port's settings menu on Windows (settings_menu.cpp): F2, or both sticks
 * of a pad pressed in, opens it over the game, which waits meanwhile. */
#ifndef FC_SETTINGS_MENU_H
#define FC_SETTINGS_MENU_H

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The menu's side, called by the host. */
void fc_menu_attach(HWND window);         /* the game's window exists: its messages pass through the menu */
int  fc_menu_wanted(void);                /* the player has asked for the menu since the last frame */
void fc_menu_open(void* device);          /* IDirect3DDevice9: the frame about to be shown becomes the background */
int  fc_menu_frame(void* device);         /* draw the menu into the back buffer; 0 once it has been closed */
void fc_menu_close(void);                 /* everything of the menu's on the device is let go */
void fc_menu_grab_after(int frames, int change);   /* for tests: open by itself at that frame, save pictures
                                                     * (and, asked to, switch vertical sync on and set 60 frames a
                                                     * second the way a row does), close */

/* The host's side, called by the menu. */
int  fc_host_fps(void);                   /* frames a second; 0 = no limit, -1 = the game's own limiter */
void fc_host_set_fps(int fps);
const char* fc_host_renderer(void);       /* "d3d9", "vulkan" or "d3d12": what is drawing now */
int  fc_host_has_dxvk(void);              /* is there a dxvk\d3d9.dll to draw with? */
int  fc_host_mouse(void);                 /* does the game have its pointer and the mouse? */
void fc_host_set_mouse(int on);
void fc_host_shot(void* device, const char* name);   /* --shots: the back buffer as <name>.png */

#ifdef __cplusplus
}
#endif

#endif
