// Synthetic host: exercise the real plugin's MinHook/IAT setup at a non-default image base.
// No original executable or game assets are needed. Build with MinGW, then run under Windows/Wine:
// i686-w64-mingw32-g++ -std=c++17 -static -s host_hooks_check.cpp -o host-hooks-check.exe \
//   -Wl,--image-base,0x14000000,--section-start,.hookpad=0x14030000 -lgdi32 -luser32
// host-hooks-check.exe <scmhd.sdp> <report.txt>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

// Keep the fixture's fake hook targets away from the program and CRT code.
__attribute__((section(".hookpad"))) unsigned char hookPad[0x180000] = {1};
void *volatile requiredImports[] = {
    reinterpret_cast<void *>(&StretchBlt), reinterpret_cast<void *>(&BitBlt),
    reinterpret_cast<void *>(&TranslateAcceleratorW)
};
#define CHECK(test) do { if (!(test)) { std::fprintf(report, "FAIL line %d: %s (%lu)\n", __LINE__, #test, GetLastError()); return 1; } } while (0)
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *report = std::fopen(argv[2], "w");
    if (!report) return 2;
    setvbuf(report, nullptr, _IONBF, 0);
    auto *base = reinterpret_cast<unsigned char *>(GetModuleHandleW(nullptr));
    CHECK(reinterpret_cast<uintptr_t>(base) != 0x400000);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base+dos->e_lfanew);
    DWORD old;
    CHECK(VirtualProtect(&nt->FileHeader.TimeDateStamp, sizeof(DWORD), PAGE_READWRITE, &old));
    nt->FileHeader.TimeDateStamp = 0x5ef3c9f5;
    CHECK(VirtualProtect(&nt->FileHeader.TimeDateStamp, sizeof(DWORD), old, &old));
    const uintptr_t targets[] = {
        0x44cff0, 0x486570, 0x488360, 0x453170, 0x501570, 0x488700,
        0x4868d0, 0x487e60, 0x486e40, 0x49f310, 0x43e1d0, 0x438f00,
        0x4b5730, 0x446500, 0x446110, 0x44ad20, 0x44aa10, 0x44bdb0
    };
    for (uintptr_t address : targets) {
        auto *target = base+(address-0x400000);
        CHECK(target >= hookPad && target+16 <= hookPad+sizeof(hookPad));
        CHECK(VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &old));
        // A normal hookable x86 prologue, followed by a return. Not called by this fixture.
        const unsigned char code[16] = {0x55,0x8b,0xec,0x83,0xec,0x08,0x8b,0xe5,0x5d,0xc3,0x90,0x90,0x90,0x90,0x90,0x90};
        std::memcpy(target, code, sizeof(code));
        CHECK(FlushInstructionCache(GetCurrentProcess(), target, sizeof(code)));
    }
    HMODULE plugin = LoadLibraryA(argv[1]); CHECK(plugin);
    char logPath[MAX_PATH]{}; CHECK(GetModuleFileNameA(plugin,logPath,MAX_PATH));
    char *ext = std::strrchr(logPath,'.'); CHECK(ext); std::strcpy(ext,".log");
    FILE *log = std::fopen(logPath,"rb"); CHECK(log);
    char lines[16384]{}; std::fread(lines,1,sizeof(lines)-1,log); std::fclose(log);
    CHECK(std::strstr(lines,"hooks installed"));
    CHECK(std::strstr(lines,"host image base"));
    CHECK(!std::strstr(lines,"FAILED") && !std::strstr(lines,"failed:"));
    std::fprintf(report,"PASS: all 18 hooks and three imports installed at image base %p.\n",base);
    std::fclose(report);
}
