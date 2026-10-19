// Windows/Wine integration check. Uses a hidden fake host; never touches an open editor.
// Build: i686-w64-mingw32-g++ -std=c++17 -static menu_check.cpp -o menu-check.exe -luser32
// Run: menu-check.exe <scmhd.sdp> <report.txt>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "../../hd/src/addon_commands.h"

#define CHECK(test) do { if (!(test)) { std::fprintf(report, "FAIL line %d: %s (%lu)\n", __LINE__, #test, GetLastError()); return 1; } } while (0)
static void pump() {
    Sleep(160);
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *report = std::fopen(argv[2], "w");
    if (!report) return 2;
    setvbuf(report, nullptr, _IONBF, 0);
    HMODULE plugin = LoadLibraryA(argv[1]); CHECK(plugin);
    using Init = BOOL (__stdcall *)(HWND, HINSTANCE, void *, void *, void *, DWORD *);
    using Label = BOOL (__stdcall *)(DWORD, char *, WORD);
    auto init = reinterpret_cast<Init>(GetProcAddress(plugin, "InitPlugin")); CHECK(init);
    auto label = reinterpret_cast<Label>(GetProcAddress(plugin, "PluginGetMenuString")); CHECK(label);
    WNDCLASSA cls{}; cls.lpfnWndProc = DefWindowProcA; cls.hInstance = GetModuleHandleA(nullptr); cls.lpszClassName = "FanAddonMenuCheck";
    CHECK(RegisterClassA(&cls));
    HWND host = CreateWindowA(cls.lpszClassName, "Menu check", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, nullptr, nullptr, cls.hInstance, nullptr); CHECK(host);
    HMENU bar = CreateMenu(), triggers = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuA(triggers, MF_STRING, 111, "Native trigger command");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(triggers), "&Triggers");
    AppendMenuA(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), "&Help"); SetMenu(host, bar);
    DWORD codes[8]{}; CHECK(init(host, cls.hInstance, nullptr, nullptr, nullptr, codes));
    for (int i = 0; i < 7; ++i) {
        char text[128]{}; CHECK(label(codes[i], text, sizeof text));
        CHECK(!strcmp(text, addon_commands::entries[i].label));
        AppendMenuA(triggers, MF_STRING, 35000+i, text);
    }
    pump(); CHECK(GetMenuItemCount(bar) == 2); // partial host registration must not move commands
    char text[128]{}; CHECK(label(codes[7], text, sizeof text)); AppendMenuA(triggers, MF_STRING, 35007, text);
    pump(); CHECK(GetMenuItemCount(bar) == 3);
    HMENU added = GetSubMenu(bar, 1); CHECK(added && added != triggers && added != help);
    CHECK(GetMenuItemCount(triggers) == 1 && GetMenuItemID(triggers, 0) == 111);
    CHECK(GetMenuItemCount(added) == 10);
    SendMessageW(host, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(added), 0);
    CHECK(!(GetMenuState(added, 35006, MF_BYCOMMAND) & MF_CHECKED));
    CHECK(!(GetMenuState(added, 35007, MF_BYCOMMAND) & MF_CHECKED));
    SendMessageW(host, WM_COMMAND, 35006, 0);
    SendMessageW(host, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(added), 0);
    CHECK(GetMenuState(added, 35006, MF_BYCOMMAND) & MF_CHECKED);
    CHECK(!(GetMenuState(added, 35007, MF_BYCOMMAND) & MF_CHECKED));
    SendMessageW(host, WM_COMMAND, 35007, 0);
    SendMessageW(host, WM_COMMAND, 35006, 0);
    SendMessageW(host, WM_INITMENUPOPUP, reinterpret_cast<WPARAM>(added), 0);
    CHECK(!(GetMenuState(added, 35006, MF_BYCOMMAND) & MF_CHECKED));
    CHECK(GetMenuState(added, 35007, MF_BYCOMMAND) & MF_CHECKED);
    SendMessageW(host, WM_INITMENU, reinterpret_cast<WPARAM>(bar), 0);
    CHECK(GetMenuItemCount(bar) == 3); // no duplicate New menus
    DestroyWindow(host);
    std::fputs("PASS: deferred menu move, native command preservation, single New menu, session-default hitboxes and independent toggles.\n", report);
    std::fclose(report);
}
