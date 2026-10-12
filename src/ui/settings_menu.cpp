/*
 * The port's settings menu on Windows.
 *
 * F2, or both sticks of a pad pressed in, opens it over the game. The host
 * holds the game at the frame it was about to show (host.c, menu_hold) and
 * calls fc_menu_frame for each frame of the menu instead; the frame the game
 * had drawn stays behind it, dimmed. It is Dear ImGui on the game's own
 * Direct3D 9 device, whichever renderer that device is, and it is there
 * only while it is open: nothing of the menu's is on the device while the
 * game runs, so the game's own Reset never meets it.
 *
 * What it changes:
 *
 *   The game's own settings, the ones its launcher used to set and its
 *   OPTION screen does not have. The game keeps each in a variable, reads
 *   them from config.ini once at start-up and writes them all back as it
 *   leaves (0x004B06A0, which names every key and its variable). A row sets
 *   the variable and writes the key at once, so the file is right whether or
 *   not the game leaves cleanly. The game looks at them when it makes its
 *   device: they take effect at the next start.
 *
 *   The port's settings, in a section of their own in the same file,
 *   [Recomp]: Fps and Mouse (taken at once), Renderer and Borderless (at the
 *   next start).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "imgui.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_win32.h"
#include "settings_menu.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

/* ---- the game's settings, where the game keeps them ---------------------------------- */
#define G8(at)  (*(volatile uint8_t*)(uintptr_t)(at))
#define G32(at) (*(volatile int32_t*)(uintptr_t)(at))
#define FC_SETTINGS_FILE 0x007A8600u    /* wchar_t[]: config.ini's full path, as the game worked it out */
#define FC_WIDTH         0x007A8110u    /* i32: WidthDX9 */
#define FC_HEIGHT        0x007A8114u    /* i32: HeightDX9 */

struct Choice { const char* name; int value; };
struct Row {
    const char* label;
    const wchar_t* section;
    const wchar_t* key;
    uint32_t at;                        /* the game's variable */
    int bytes;                          /* 1 or 4 */
    const Choice* choices;
    int count;
    const char* help;
};

static const Choice k_off_on[] = { { "Off", 0 }, { "On", 1 } };
static const Choice k_on_off[] = { { "On", 0 }, { "Off", 1 } };      /* the game's "...Off" keys */
static const Choice k_samples[] = { { "Off", 0 }, { "2x", 2 }, { "4x", 4 }, { "8x", 8 } };
static const Choice k_filter[] = { { "Nearest", 1 }, { "Linear", 2 }, { "Anisotropic", 3 } };
static const Choice k_shadows[] = { { "Off", 0 }, { "Simple", 1 }, { "Full", 2 } };
#define CHOICES(a) a, (int)(sizeof a / sizeof a[0])

#define FC_WINDOW_MODE 0x007A859Au      /* byte: WindowMode, 1 a window */
static const Row k_screen[] = {
    { "Vertical sync", L"Video", L"WaitVSync", 0x007A859B, 1, CHOICES(k_off_on), "Wait for the screen before showing each frame: no tearing." },
    { "Anti-aliasing", L"Video", L"FullAntiAlias", 0x007A8598, 1, CHOICES(k_samples), "Smoother edges, for some of the graphics card's time." },
    { "Keep the picture's shape", L"Video", L"KeepAspect", 0x007A8118, 1, CHOICES(k_off_on), "Bars at the sides rather than a stretched picture." },
};
static const Row k_quality[] = {
    { "Texture filter", L"Video", L"TextureFilter", 0x007A8590, 1, CHOICES(k_filter), "How textures are smoothed; anisotropic is the sharpest at a slant." },
    { "Mipmaps", L"Video", L"UseMipmap", 0x007A859D, 1, CHOICES(k_off_on), "Smaller copies of textures for things far away: less shimmer." },
    { "Shadows", L"Video", L"ShadowQuality", 0x007A859F, 1, CHOICES(k_shadows), "The shadows characters cast." },
    { "High resolution text", L"Video", L"HighResoText", 0x007A85A0, 1, CHOICES(k_off_on), "Text drawn at the screen's own resolution." },
    { "High resolution art", L"Video", L"HighResoAssets", 0x007A85A2, 1, CHOICES(k_off_on), "The larger interface art of the 2014 release." },
};
static const Row k_game[] = {
    { "Logo movie", L"Video", L"LogoPlayOff", 0x007A85AA, 1, CHOICES(k_on_off), "The logos before the title screen." },
    { "Opening movie", L"Video", L"OpeningPlayOff", 0x007A85AB, 1, CHOICES(k_on_off), "The opening, after a new game's first scenes." },
};

static const wchar_t* settings_file(void) {
    const wchar_t* path = (const wchar_t*)(uintptr_t)FC_SETTINGS_FILE;
    return path[0] ? path : NULL;
}
static void write_key(const wchar_t* section, const wchar_t* key, const wchar_t* text) {
    const wchar_t* path = settings_file();
    if (!path || !WritePrivateProfileStringW(section, key, text, path))
        fprintf(stderr, "[menu] %ls could not be written to the settings file (error %lu)\n", key, path ? GetLastError() : 0ul);
}
static void write_int(const wchar_t* section, const wchar_t* key, int value) {
    wchar_t text[16];
    swprintf(text, 16, L"%d", value);
    write_key(section, key, text);
}
static int row_value(const Row& r) { return r.bytes == 1 ? (int)G8(r.at) : (int)G32(r.at); }
static void row_set(const Row& r, int value) {
    if (r.bytes == 1) G8(r.at) = (uint8_t)value; else G32(r.at) = value;
    write_int(r.section, r.key, value);
    fprintf(stderr, "[menu] %ls = %d\n", r.key, value);
}

/* ---- the port's own settings ---------------------------------------------------------- */
static const Choice k_fps[] = { { "30", 30 }, { "60", 60 }, { "90", 90 }, { "120", 120 }, { "125 (the game's own)", 125 },
                                { "144", 144 }, { "165", 165 }, { "240", 240 }, { "No limit", 0 } };
static const struct { const char* name; const char* word; } k_renderers[] = {
    { "Direct3D 9", "d3d9" }, { "Vulkan", "vulkan" }, { "Direct3D 12", "d3d12" },
};
static char g_renderer_next[16];        /* what the settings file asks for at the next start */
static int g_borderless_next;           /* and whether the window is to be borderless */

static void renderer_read(void) {
    wchar_t text[16] = L"";
    const wchar_t* path = settings_file();
    if (path) GetPrivateProfileStringW(L"Recomp", L"Renderer", L"", text, 16, path);
    snprintf(g_renderer_next, sizeof g_renderer_next, "%ls", text);
    if (!g_renderer_next[0]) snprintf(g_renderer_next, sizeof g_renderer_next, "%s", fc_host_renderer());
    g_borderless_next = path && GetPrivateProfileIntW(L"Recomp", L"Borderless", 0, path) != 0;
}

/* ---- resolutions: what the display offers, and what is set ----------------------------- */
static struct { int w, h; char name[24]; } g_modes[96];
static int g_nmodes;

static void modes_add(int w, int h) {
    if (w < 640 || h < 480) return;
    for (int i = 0; i < g_nmodes; i++) if (g_modes[i].w == w && g_modes[i].h == h) return;
    if (g_nmodes == (int)(sizeof g_modes / sizeof g_modes[0])) return;
    int at = g_nmodes++;
    while (at > 0 && (g_modes[at - 1].w > w || (g_modes[at - 1].w == w && g_modes[at - 1].h > h))) g_modes[at] = g_modes[at - 1], at--;
    g_modes[at].w = w, g_modes[at].h = h;
    snprintf(g_modes[at].name, sizeof g_modes[at].name, "%d x %d", w, h);
}
static void modes_list(IDirect3DDevice9* dev) {
    IDirect3D9* d3d = NULL;
    g_nmodes = 0;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
        UINT n = d3d->GetAdapterModeCount(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8);
        for (UINT i = 0; i < n; i++) {
            D3DDISPLAYMODE m;
            if (SUCCEEDED(d3d->EnumAdapterModes(D3DADAPTER_DEFAULT, D3DFMT_X8R8G8B8, i, &m))) modes_add((int)m.Width, (int)m.Height);
        }
        d3d->Release();
    }
    modes_add(1280, 720);
    modes_add(G32(FC_WIDTH), G32(FC_HEIGHT));      /* a size set by hand stays on the list */
}

/* ---- the menu's state ------------------------------------------------------------------ */
static HWND g_hwnd;
static WNDPROC g_prev_proc;
static bool g_context, g_open, g_win32_up, g_dx9_up;
static IDirect3DTexture9* g_background;
static int g_width, g_height;           /* the back buffer's */
static int g_grab_in;                   /* tests: frames until the menu opens by itself */
static int g_grab_frame;                /* tests: > 0 while it is taking its pictures */
static int g_grab_change;               /* tests: and it changes two settings while it is open */

static LRESULT CALLBACK menu_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (g_open) {
        ImGui_ImplWin32_WndProcHandler(h, m, w, l);
        switch (m) {                    /* what the menu is given, the game does not also get */
        case WM_KEYDOWN: case WM_KEYUP: case WM_CHAR:
        case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
            return 0;
        case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_SYSCHAR:
            return DefWindowProcW(h, m, w, l);        /* Alt+F4 still closes the game */
        }
    }
    if (!fc_host_mouse()) switch (m) {  /* the mouse switched off: the game is not told of it, and shows no pointer */
    case WM_MOUSEMOVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        break;
    }
    return CallWindowProcW(g_prev_proc, h, m, w, l);
}

void fc_menu_attach(HWND window) {
    if (!window || window == g_hwnd) return;
    g_hwnd = window;
    g_prev_proc = (WNDPROC)SetWindowLongPtrW(window, GWLP_WNDPROC, (LONG_PTR)menu_proc);
}

void fc_menu_grab_after(int frames, int change) { g_grab_in = frames, g_grab_change = change; }

/* Both sticks pressed in, on the first pad. A pad that is not there is asked
 * for again only now and then: asking is slow. */
static bool sticks_pressed(void) {
    typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } pad_t;
    typedef struct { DWORD packet; pad_t pad; } state_t;
    typedef DWORD (WINAPI* get_state_t)(DWORD, state_t*);
    static get_state_t get_state;
    static bool tried;
    static DWORD again;                 /* when to ask a pad that was not there again */
    if (!tried) {
        static const char* const names[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
        tried = true;
        for (int i = 0; i < 3 && !get_state; i++) {
            HMODULE m = LoadLibraryA(names[i]);
            if (m) get_state = (get_state_t)(void*)GetProcAddress(m, "XInputGetState");
        }
    }
    if (!get_state || (again && (LONG)(GetTickCount() - again) < 0)) return false;
    state_t s;
    memset(&s, 0, sizeof s);
    if (get_state(0, &s) != ERROR_SUCCESS) { again = GetTickCount() + 3000; if (!again) again = 1; return false; }
    again = 0;
    return (s.pad.buttons & 0x00C0) == 0x00C0;      /* XINPUT_GAMEPAD_LEFT_THUMB | RIGHT_THUMB */
}

int fc_menu_wanted(void) {
    static bool was;
    if (g_grab_in > 0 && --g_grab_in == 0) { g_grab_frame = 1; return 1; }
    if (!g_hwnd || GetForegroundWindow() != g_hwnd) { was = false; return 0; }
    bool now = (GetAsyncKeyState(VK_F2) & 0x8000) != 0 || sticks_pressed();
    bool pressed = now && !was;
    was = now;
    return pressed;
}

static void look(float scale) {
    ImGuiStyle s;
    ImGui::StyleColorsDark(&s);
    const ImVec4 gold(222 / 255.0f, 176 / 255.0f, 92 / 255.0f, 1.0f);
    s.WindowRounding = 6.0f, s.FrameRounding = 4.0f, s.PopupRounding = 4.0f, s.TabRounding = 4.0f;
    s.WindowPadding = ImVec2(22, 18), s.FramePadding = ImVec2(10, 6), s.ItemSpacing = ImVec2(12, 9);
    s.WindowBorderSize = 1.0f;
    s.Colors[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.96f);
    s.Colors[ImGuiCol_Border] = ImVec4(gold.x, gold.y, gold.z, 0.55f);
    s.Colors[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.17f, 0.20f, 1.0f);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.24f, 0.26f, 1.0f);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.28f, 0.24f, 1.0f);
    s.Colors[ImGuiCol_Header] = ImVec4(gold.x, gold.y, gold.z, 0.30f);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(gold.x, gold.y, gold.z, 0.60f);
    s.Colors[ImGuiCol_Button] = ImVec4(0.20f, 0.21f, 0.24f, 1.0f);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(gold.x, gold.y, gold.z, 0.65f);
    s.Colors[ImGuiCol_Tab] = ImVec4(0.14f, 0.15f, 0.18f, 1.0f);
    s.Colors[ImGuiCol_TabHovered] = ImVec4(gold.x, gold.y, gold.z, 0.45f);
    s.Colors[ImGuiCol_TabSelected] = ImVec4(gold.x, gold.y, gold.z, 0.35f);
    s.Colors[ImGuiCol_CheckMark] = gold;
    s.Colors[ImGuiCol_NavCursor] = gold;
    s.Colors[ImGuiCol_Separator] = ImVec4(gold.x, gold.y, gold.z, 0.35f);
    s.ScaleAllSizes(scale);
    s.FontScaleMain = scale;
    ImGui::GetStyle() = s;
}

void fc_menu_open(void* device) {
    IDirect3DDevice9* dev = (IDirect3DDevice9*)device;
    IDirect3DSurface9 *back = NULL, *into = NULL;
    D3DSURFACE_DESC desc;
    if (g_open || !g_hwnd) return;
    if (!g_context) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = NULL, io.LogFilename = NULL;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        io.MouseDrawCursor = true;                    /* the game hides Windows' own pointer */
        char font[MAX_PATH];
        UINT n = GetWindowsDirectoryA(font, MAX_PATH - 24);
        snprintf(font + n, sizeof font - n, "\\Fonts\\segoeui.ttf");
        if (GetFileAttributesA(font) == INVALID_FILE_ATTRIBUTES || !io.Fonts->AddFontFromFileTTF(font, 18.0f)) io.Fonts->AddFontDefault();
        g_context = true;
    }
    memset(&desc, 0, sizeof desc);
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        back->GetDesc(&desc);
        /* the frame the game has just drawn, kept to stand behind the menu */
        if (SUCCEEDED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_background, NULL))
            && g_background && SUCCEEDED(g_background->GetSurfaceLevel(0, &into))) {
            if (FAILED(dev->StretchRect(back, NULL, into, NULL, D3DTEXF_NONE))) g_background->Release(), g_background = NULL;
            into->Release();
        }
        back->Release();
    }
    g_width = desc.Width ? (int)desc.Width : 1280, g_height = desc.Height ? (int)desc.Height : 720;
    look((float)g_height / 720.0f);
    g_win32_up = ImGui_ImplWin32_Init(g_hwnd);
    g_dx9_up = ImGui_ImplDX9_Init(dev);
    modes_list(dev);
    renderer_read();
    g_open = g_win32_up && g_dx9_up;
    if (!g_open) fc_menu_close();
    else fprintf(stderr, "[menu] open, over a %dx%d frame\n", g_width, g_height);
}

void fc_menu_close(void) {
    if (g_dx9_up) ImGui_ImplDX9_Shutdown(), g_dx9_up = false;
    if (g_win32_up) ImGui_ImplWin32_Shutdown(), g_win32_up = false;
    if (g_background) g_background->Release(), g_background = NULL;
    if (g_open) fprintf(stderr, "[menu] closed\n");
    g_open = false, g_grab_frame = 0;
}

/* A row: its name, and a box of the things it can be. */
static bool choose(const char* label, const char** names, int count, int* at, const char* help) {
    bool changed = false;
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
    if (ImGui::BeginCombo("##v", *at >= 0 && *at < count ? names[*at] : "?")) {
        for (int i = 0; i < count; i++) {
            bool on = i == *at;
            if (ImGui::Selectable(names[i], on) && !on) *at = i, changed = true;
            if (on) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (help && (ImGui::IsItemHovered() || ImGui::IsItemFocused())) ImGui::SetItemTooltip("%s", help);
    ImGui::PopID();
    return changed;
}
static void rows(const Row* list, int count) {
    for (int r = 0; r < count; r++) {
        const char* names[8];
        int at = -1, value = row_value(list[r]);
        for (int i = 0; i < list[r].count && i < 8; i++) {
            names[i] = list[r].choices[i].name;
            if (list[r].choices[i].value == value) at = i;
        }
        if (choose(list[r].label, names, list[r].count, &at, list[r].help)) row_set(list[r], list[r].choices[at].value);
    }
}
static bool table(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) return false;
    ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthStretch, 0.46f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.54f);
    return true;
}

int fc_menu_frame(void* device) {
    IDirect3DDevice9* dev = (IDirect3DDevice9*)device;
    if (!g_open) return 0;
    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplDX9_NewFrame();
    ImGui_ImplWin32_NewFrame();
    io.DisplaySize = ImVec2((float)g_width, (float)g_height);       /* the back buffer's size, whatever the window's */
    ImGui::NewFrame();
    const float k = (float)g_height / 720.0f;
    bool leave = false;

    if (g_background)
        ImGui::GetBackgroundDrawList()->AddImage(ImTextureRef((ImTextureID)(intptr_t)g_background), ImVec2(0, 0), io.DisplaySize,
                                                 ImVec2(0, 0), ImVec2(1, 1), IM_COL32(96, 96, 96, 255));

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(800.0f * k, 560.0f * k), ImGuiCond_Always);
    ImGui::Begin("Settings", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(NULL, ImGui::GetStyle().FontSizeBase * 1.35f);
    ImGui::TextColored(ImVec4(222 / 255.0f, 176 / 255.0f, 92 / 255.0f, 1.0f), "Settings");
    ImGui::PopFont();
    ImGui::Separator();

    if (ImGui::BeginTabBar("pages")) {
        /* a test's pictures: each page in turn */
        ImGuiTabItemFlags second = g_grab_frame == 4 ? ImGuiTabItemFlags_SetSelected : 0;
        ImGuiTabItemFlags third = g_grab_frame == 8 ? ImGuiTabItemFlags_SetSelected : 0;
        if (ImGui::BeginTabItem("Display")) {
            if (table("display")) {
                const char* names[96];
                int at = -1;
                for (int i = 0; i < g_nmodes; i++) {
                    names[i] = g_modes[i].name;
                    if (g_modes[i].w == G32(FC_WIDTH) && g_modes[i].h == G32(FC_HEIGHT)) at = i;
                }
                if (choose("Resolution", names, g_nmodes, &at, "The size of the picture: of the window, or of the screen when the game has all of it.")) {
                    G32(FC_WIDTH) = g_modes[at].w, G32(FC_HEIGHT) = g_modes[at].h;
                    write_int(L"Video", L"WidthDX9", g_modes[at].w);
                    write_int(L"Video", L"HeightDX9", g_modes[at].h);
                    fprintf(stderr, "[menu] resolution = %dx%d\n", g_modes[at].w, g_modes[at].h);
                }
                {   /* a window, a borderless window (the port's own), or the game's full screen */
                    const char* modes[3] = { "Window", "Borderless window", "Full screen" };
                    int mode = !G8(FC_WINDOW_MODE) ? 2 : g_borderless_next ? 1 : 0;
                    if (choose("Display", modes, 3, &mode, "A window; a window without a frame that covers the screen; or the screen itself, switched to the resolution above.")) {
                        G8(FC_WINDOW_MODE) = mode == 2 ? 0 : 1;
                        g_borderless_next = mode == 1;
                        write_int(L"Video", L"WindowMode", mode == 2 ? 0 : 1);
                        write_int(L"Recomp", L"Borderless", g_borderless_next);
                        fprintf(stderr, "[menu] display = %s (at the next start)\n", modes[mode]);
                    }
                }
                rows(k_screen, (int)(sizeof k_screen / sizeof k_screen[0]));

                const char* rnames[3];
                int rcount = 0, rat = -1, rmap[3];
                for (int i = 0; i < 3; i++) {
                    if (i == 1 && !fc_host_has_dxvk()) continue;         /* no dxvk\d3d9.dll to draw with */
                    if (!strcmp(k_renderers[i].word, g_renderer_next)) rat = rcount;
                    rmap[rcount] = i, rnames[rcount++] = k_renderers[i].name;
                }
                if (choose("Renderer", rnames, rcount, &rat, "What draws the game: Windows' Direct3D 9, Vulkan (DXVK) or Direct3D 12.")) {
                    snprintf(g_renderer_next, sizeof g_renderer_next, "%s", k_renderers[rmap[rat]].word);
                    wchar_t word[16];
                    swprintf(word, 16, L"%hs", g_renderer_next);
                    write_key(L"Recomp", L"Renderer", word);
                    fprintf(stderr, "[menu] renderer = %s (at the next start)\n", g_renderer_next);
                }

                const char* fnames[10];
                int fat = -1, fps = fc_host_fps(), fcount = (int)(sizeof k_fps / sizeof k_fps[0]);
                char other[32];
                for (int i = 0; i < fcount; i++) {
                    fnames[i] = k_fps[i].name;
                    if (k_fps[i].value == fps) fat = i;
                }
                if (fat < 0) {                                    /* a rate set by hand, or the game's own limiter */
                    if (fps < 0) snprintf(other, sizeof other, "The game's own limiter");
                    else snprintf(other, sizeof other, "%d", fps);
                    fnames[fcount] = other, fat = fcount++;
                }
                if (choose("Frame rate", fnames, fcount, &fat, "Frames a second. Taken at once.") && fat < (int)(sizeof k_fps / sizeof k_fps[0])) {
                    fc_host_set_fps(k_fps[fat].value);
                    write_int(L"Recomp", L"Fps", k_fps[fat].value);
                    fprintf(stderr, "[menu] frame rate = %d\n", k_fps[fat].value);
                }
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Quality", NULL, second)) {
            if (table("quality")) {
                rows(k_quality, (int)(sizeof k_quality / sizeof k_quality[0]));
                ImGui::EndTable();
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game", NULL, third)) {
            if (table("game")) {
                const char* onoff[2] = { "On", "Off" };
                int mouse = fc_host_mouse() ? 0 : 1;
                if (choose("Mouse", onoff, 2, &mouse, "Off: no pointer, and the mouse does nothing in the game. For a pad or the keyboard alone. Taken at once.")) {
                    fc_host_set_mouse(mouse == 0);
                    write_int(L"Recomp", L"Mouse", mouse == 0);
                    fprintf(stderr, "[menu] mouse = %s\n", onoff[mouse]);
                }
                rows(k_game, (int)(sizeof k_game / sizeof k_game[0]));
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextWrapped("Music, sound effects, voices, the camera and the mini map are in the game's own OPTION screen.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    /* the foot of the panel */
    float foot = ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() * 2.0f + ImGui::GetStyle().WindowPadding.y;
    if (ImGui::GetWindowHeight() - foot > ImGui::GetCursorPosY()) ImGui::SetCursorPosY(ImGui::GetWindowHeight() - foot);
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.72f, 0.76f, 1.0f));
    ImGui::TextWrapped("Saved as you change them. The frame rate and the mouse change at once; the rest the next time the game starts.");
    ImGui::PopStyleColor();
    if (ImGui::Button("Back to the game")) leave = true;
    ImGui::SameLine();
    ImGui::TextDisabled("F2 or Esc");
    bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    if (ImGui::IsKeyPressed(ImGuiKey_F2, false)) leave = true;
    if (!popup && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) leave = true;
    ImGui::End();

    ImGui::EndFrame();
    if (!g_background) dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (SUCCEEDED(dev->BeginScene())) {
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
        dev->EndScene();
    }
    if (g_grab_frame) {                                           /* a test: a picture of each page and out */
        if (g_grab_frame == 1 && g_grab_change) {                 /* and two settings changed, as a row changes them */
            row_set(k_screen[0], 1);
            fc_host_set_fps(60);
            write_int(L"Recomp", L"Fps", 60);
        }
        if (g_grab_frame == 3) fc_host_shot(dev, "menu_display");
        if (g_grab_frame == 7) fc_host_shot(dev, "menu_quality");
        if (g_grab_frame == 11) fc_host_shot(dev, "menu_game"), leave = true;
        g_grab_frame++;
    }
    if (leave) {
        /* The Present the game is waiting in shows the back buffer next: the game's own frame again. */
        IDirect3DSurface9 *back = NULL, *from = NULL;
        if (g_background && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
            if (SUCCEEDED(g_background->GetSurfaceLevel(0, &from))) dev->StretchRect(from, NULL, back, NULL, D3DTEXF_NONE), from->Release();
            back->Release();
        }
        fc_menu_close();
    }
    return g_open;
}
