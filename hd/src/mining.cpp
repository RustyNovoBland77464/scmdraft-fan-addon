// "Simulate Mining..." for ScmDraft 2 (0.9.10), part of scmhd.sdp.
//
// Exports the open map from memory (unsaved edits included) and runs minesim.exe (next to the
// plugin; OpenBW engine + SC:R data) on it in the background, then shows its report: minerals per
// minute for CC / Nexus / Hatchery at every start location and placed resource depot, gas, and per
// field round-trip times.
//
// Finding the open map: the render engine sits 0x970 bytes before its backbuffer (which the blit
// hooks see); engine+0xb0 is the CEngineMap, map+0x6fc its StarcraftMap/CHK object, and
// 0x4b1af0 StarcraftMap::SaveToMemory(format = chk+0x50, &size, &buffer) serializes it like Save.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <cmath>
#include <cctype>
#include <algorithm>

void logf(const char *fmt, ...);
std::string scmhdStarcraftPath();
void *scmhdActiveMapChk();
void scmhdLock();
void scmhdUnlock();
bool scmhdExportActiveMap(std::vector<uint8_t> &out, std::string &err);

namespace mining {

template <class T> T &at(void *p, int off) { return *reinterpret_cast<T *>(static_cast<uint8_t *>(p) + off); }

constexpr uintptr_t kSaveToMemory = 0x004b1af0;
using SaveToMemoryFn = HRESULT(__fastcall *)(void *, void *, uint32_t, int *, void **);

// ---- minimal JSON reader (minesim's own output) -------------------------------------------------
struct J {
    enum T { NUL, NUM, STR, BOOL, ARR, OBJ } t = NUL;
    double n = 0; std::string s; bool b = false;
    std::vector<J> a; std::vector<std::pair<std::string, J>> o;
    const J &operator[](const char *k) const { static J none; for (auto &kv : o) if (kv.first == k) return kv.second; return none; }
};
struct JP {
    const char *p, *e;
    void ws() { while (p < e && isspace((unsigned char)*p)) p++; }
    J val() {
        ws(); J v;
        if (p >= e) return v;
        if (*p == '{') {
            v.t = J::OBJ; p++;
            for (ws(); p < e && *p != '}'; ws()) { J k = val(); ws(); if (*p == ':') p++; v.o.push_back({k.s, val()}); ws(); if (*p == ',') p++; }
            p++;
        } else if (*p == '[') {
            v.t = J::ARR; p++;
            for (ws(); p < e && *p != ']'; ws()) { v.a.push_back(val()); ws(); if (*p == ',') p++; }
            p++;
        } else if (*p == '"') {
            v.t = J::STR; p++;
            while (p < e && *p != '"') v.s += *p++;
            p++;
        } else if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
            v.t = J::BOOL; v.b = *p == 't'; p += v.b ? 4 : 5;
        } else {
            v.t = J::NUM; char *end; v.n = strtod(p, &end); p = end;
        }
        return v;
    }
};

// ---- overlay data ------------------------------------------------------------------------------
struct Field { int x, y; double trip[3] = {}; };          // trip frames per race (T, P, Z), 9 workers
struct Base { int x, y; double perMin[3] = {}; std::vector<Field> fields; };
std::vector<Base> g_bases;
bool g_overlay;
int raceIndex(const std::string &r) { return r == "terran" ? 0 : r == "protoss" ? 1 : 2; }

void loadOverlay(const std::string &file) {
    g_bases.clear();
    std::string text;
    if (FILE *f = fopen(file.c_str(), "rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) text.append(b, n); fclose(f); }
    JP jp{text.data(), text.data() + text.size()};
    J root = jp.val();
    for (const J &jb : root["bases"].a) {
        Base b; b.x = (int)jb["x"].n; b.y = (int)jb["y"].n;
        for (const J &jp2 : jb["patches"].a) if (jp2.a.size() == 2) b.fields.push_back({(int)jp2.a[0].n, (int)jp2.a[1].n});
        int firstWorkers = -1;
        for (const J &r : jb["results"].a) {
            int w = (int)r["workers"].n;
            if (firstWorkers < 0) firstWorkers = w;
            if (w != firstWorkers) continue;
            int ri = raceIndex(r["race"].s);
            b.perMin[ri] = r["minerals_per_min"].n;
            const J &trips = r["patch_trip_frames"];
            for (size_t i = 0; i < trips.a.size() && i < b.fields.size(); i++) b.fields[i].trip[ri] = trips.a[i].n;
        }
        g_bases.push_back(b);
    }
    logf("mining: overlay with %zu bases", g_bases.size());
}

HWND g_main, g_wnd, g_edit;
HMODULE g_self;
volatile bool g_running;
constexpr UINT WM_SIM_DONE = WM_APP + 41;

std::string pluginDir() {
    char path[MAX_PATH]; GetModuleFileNameA(g_self, path, MAX_PATH);
    *strrchr(path, '\\') = 0;
    return path;
}

bool exportChk(const std::string &file, std::string &err) {
    std::vector<uint8_t> chk;
    if (!scmhdExportActiveMap(chk, err)) return false;
    FILE *f = fopen(file.c_str(), "wb");
    bool ok = f && fwrite(chk.data(), 1, chk.size(), f) == chk.size();
    if (f) fclose(f);
    if (!ok) err = "Could not write " + file;
    logf("mining: exported %zu bytes of CHK to %s", chk.size(), file.c_str());
    return ok;
}

struct Job { std::string exe, args, out; };

DWORD WINAPI worker(LPVOID p) {
    Job *job = static_cast<Job *>(p);
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileA(job->out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, 0, nullptr);
    STARTUPINFOA si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out; si.hStdError = out; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + job->exe + "\" " + job->args;
    DWORD code = 1;
    if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    CloseHandle(out);
    logf("mining: minesim exited with %lu", code);
    PostMessageA(g_wnd, WM_SIM_DONE, code, reinterpret_cast<LPARAM>(job));
    return 0;
}

void setText(const std::string &s) {
    std::wstring w(s.size(), L' ');
    w.resize(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size()));
    SetWindowTextW(g_edit, w.c_str());
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_SIZE:
        if (g_edit) MoveWindow(g_edit, 0, 0, LOWORD(lp), HIWORD(lp), TRUE);
        return 0;
    case WM_SIM_DONE: {
        Job *job = reinterpret_cast<Job *>(lp);
        std::string text;
        if (FILE *f = fopen(job->out.c_str(), "rb")) {
            char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
            fclose(f);
        }
        if (wp != 0) text = "The simulation failed (exit code " + std::to_string(wp) + "):\r\n\r\n" + text;
        else {
            text += "\r\nMap overlay: each mineral field shows its round trip in frames for T / P / Z (fastest\r\n"
                    "highlighted), each base its minerals per minute. Close this window to hide it.\r\n";
            scmhdLock();  // the render thread draws the overlay from g_bases
            loadOverlay(pluginDir() + "\\mining_report.json");
            g_overlay = !g_bases.empty();
            scmhdUnlock();
            RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        }
        setText(text);
        g_running = false;
        delete job;
        return 0;
    }
    case WM_CLOSE:
        if (g_running) return 0;  // the worker thread still reports into this window
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_wnd = g_edit = nullptr;
        scmhdLock(); g_overlay = false; scmhdUnlock();
        RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ensureWindow() {
    if (g_wnd) { ShowWindow(g_wnd, SW_SHOWNORMAL); SetForegroundWindow(g_wnd); return; }
    static bool registered;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc; wc.hInstance = g_self; wc.lpszClassName = L"ScmHDMiningReport";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        registered = true;
    }
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"ScmHDMiningReport", L"Mining Simulation", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 760, 720, g_main, nullptr, g_self, nullptr);
    g_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                             0, 0, 100, 100, g_wnd, nullptr, g_self, nullptr);
    HFONT font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Courier New");
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)font, TRUE);
    RECT r; GetClientRect(g_wnd, &r); MoveWindow(g_edit, 0, 0, r.right, r.bottom, TRUE);
    ShowWindow(g_wnd, SW_SHOWNORMAL);
}

void run() {
    if (g_running) { ensureWindow(); return; }
    ensureWindow();
    std::string dir = pluginDir(), chk = dir + "\\mining_input.chk", err;
    if (!exportChk(chk, err)) { setText(err); return; }
    std::string exe = dir + "\\minesim.exe";
    if (GetFileAttributesA(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { setText("minesim.exe is missing next to the plugin (" + exe + ")."); return; }
    setText("Simulating mining for Terran, Protoss and Zerg at every base...\r\n\r\n"
            "(OpenBW runs about 3.5 game minutes per base, race and worker count; this takes a few seconds.)");
    Job *job = new Job{exe, "--casc \"" + scmhdStarcraftPath() + "\" --map \"" + chk + "\" --gas --report --json \"" + dir + "\\mining_report.json\"", dir + "\\mining_report.txt"};
    g_running = true;
    CloseHandle(CreateThread(nullptr, 0, worker, job, 0, nullptr));
}

// Draw the overlay onto a map-view blit. Destination (xd, yd, wd, hd) shows buffer pixels
// (xs, ys, ws, hs); buffer pixel * srcScale + origin = map pixel.
void drawOverlay(HDC dc, int xd, int yd, int wd, int hd, double xs, double ys, double ws, double hs, int originX, int originY, double srcScale) {
    if (!g_overlay || g_bases.empty() || wd <= 0 || hd <= 0 || ws <= 0 || hs <= 0) return;
    static HFONT font = CreateFontW(-11, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, NONANTIALIASED_QUALITY, 0, L"Tahoma");
    int saved = SaveDC(dc);
    IntersectClipRect(dc, xd, yd, xd + wd, yd + hd);
    SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    auto toScreen = [&](int mx, int my, int *sx, int *sy) {
        *sx = xd + (int)(((mx - originX) / srcScale - xs) * wd / ws);
        *sy = yd + (int)(((my - originY) / srcScale - ys) * hd / hs);
    };
    std::vector<RECT> placed;
    static const COLORREF raceColor[3] = {RGB(90, 170, 255), RGB(255, 220, 60), RGB(220, 110, 255)};
    static const char raceLetter[3] = {'T', 'P', 'Z'};
    auto label = [&](int cx, int cy, const double v[3], bool lowerIsBetter, const char *suffix) {
        char part[3][24]; SIZE sz[3]; int total = 0, h = 0;
        int best = 0;
        for (int r = 0; r < 3; r++) if (lowerIsBetter ? (v[r] > 0 && (v[best] <= 0 || v[r] < v[best])) : v[r] > v[best]) best = r;
        for (int r = 0; r < 3; r++) {
            snprintf(part[r], sizeof part[r], "%c%.0f%s", raceLetter[r], v[r], r == 2 ? suffix : " ");
            GetTextExtentPoint32A(dc, part[r], (int)strlen(part[r]), &sz[r]);
            total += sz[r].cx; h = std::max(h, (int)sz[r].cy);
        }
        RECT bg{cx - total / 2 - 2, cy - h / 2 - 1, cx + total / 2 + 2, cy + h / 2 + 1};
        // nudge down until it does not overlap labels already drawn (adjacent fields)
        for (int tries = 0; tries < 8; tries++) {
            bool hit = false;
            for (const RECT &o : placed) { RECT t; if (IntersectRect(&t, &o, &bg)) { hit = true; OffsetRect(&bg, 0, o.bottom - bg.top + 1); break; } }
            if (!hit) break;
        }
        placed.push_back(bg);
        HBRUSH br = CreateSolidBrush(RGB(16, 16, 16)); FillRect(dc, &bg, br); DeleteObject(br);
        int x = bg.left + 2;
        for (int r = 0; r < 3; r++) {
            SetTextColor(dc, r == best ? raceColor[r] : RGB(150, 150, 150));
            TextOutA(dc, x, bg.top + 1, part[r], (int)strlen(part[r]));
            x += sz[r].cx;
        }
    };
    for (const Base &b : g_bases) {  // base labels first so they keep their place
        int sx, sy; toScreen(b.x, b.y, &sx, &sy);
        label(sx, sy, b.perMin, false, "/min");
    }
    for (const Base &b : g_bases)
        for (const Field &f : b.fields) {
            int sx, sy; toScreen(f.x, f.y, &sx, &sy);
            label(sx, sy, f.trip, true, "");
        }
    RestoreDC(dc, saved);
}

void init(HMODULE self, HWND mainWnd) { g_self = self; g_main = mainWnd; }

} // namespace mining

// The open map as CHK bytes, serialized by ScmDraft itself (unsaved edits included).
bool scmhdExportActiveMap(std::vector<uint8_t> &out, std::string &err) {
    void *chk = scmhdActiveMapChk();
    if (!chk) { err = "No open map found. Open a map and let it draw once, then try again."; return false; }
    int size = 0; void *buf = nullptr;
    HRESULT hr = reinterpret_cast<mining::SaveToMemoryFn>(mining::kSaveToMemory)(chk, nullptr, mining::at<uint32_t>(chk, 0x50), &size, &buf);
    if (hr < 0 || !buf || size <= 0) { char m[96]; sprintf(m, "ScmDraft could not serialize the map (hr=0x%08lX).", hr); err = m; return false; }
    out.assign(static_cast<uint8_t *>(buf), static_cast<uint8_t *>(buf) + size);
    // the buffer comes from ScmDraft's CRT (msvcr100 operator new[])
    if (HMODULE crt = GetModuleHandleA("msvcr100.dll"))
        if (auto del = reinterpret_cast<void(__cdecl *)(void *)>(GetProcAddress(crt, "??_V@YAXPAX@Z"))) del(buf);
    return true;
}
