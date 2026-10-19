#include "verify.h"
#include <cstdio>
int main(int argc, char **argv) {
    if (argc != 2) { std::puts("Usage: installer-check.exe <ScmDraft executable>"); return 2; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, nullptr, 0);
    if (!n) return 2;
    std::wstring path(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[1], -1, &path[0], n);
    path.resize(n - 1);
    std::printf("SHA256: %s\n", fan_edit::sha256(path).c_str());
    bool ok = fan_edit::supported(path);
    std::puts(ok ? "Supported ScmDraft build" : "Unsupported or unreadable file");
    return ok ? 0 : 1;
}
