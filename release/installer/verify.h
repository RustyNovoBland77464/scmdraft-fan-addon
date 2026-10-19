#pragma once
#include <windows.h>
#include <wincrypt.h>
#include <string>

namespace fan_edit {
constexpr char supportedHash[] = "751060e3e09eba3ce043d54a1f39ac1143ee9d6df0f9444cf0a2caf3de28f72e";
inline std::string sha256(const std::wstring &path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    bool ok = !!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT);
    if (ok) ok = !!CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    BYTE buf[65536]; DWORD n = 0;
    while (ok) {
        if (!ReadFile(f, buf, sizeof buf, &n, nullptr)) { ok = false; break; }
        if (!n) break;
        ok = !!CryptHashData(hash, buf, n, 0);
    }
    BYTE digest[32]; DWORD size = sizeof digest;
    if (ok) ok = !!CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0);
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(f);
    if (!ok || size != 32) return {};
    std::string result;
    for (BYTE b : digest) { result += "0123456789abcdef"[b >> 4]; result += "0123456789abcdef"[b & 15]; }
    return result;
}
inline bool supported(const std::wstring &path) { return sha256(path) == supportedHash; }
inline std::wstring parent(const std::wstring &path) {
    const auto end = path.find_last_of(L"/\\");
    return end == std::wstring::npos ? L"." : path.substr(0, end);
}
inline bool fileExists(const std::wstring &path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}
}
