// Installs only our two payload files. Original editor and game data stay user-supplied.
#include "verify.h"
#include <commdlg.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <vector>

static void message(const std::wstring &s, UINT style = MB_ICONERROR) {
    MessageBoxW(nullptr, s.c_str(), L"ScmDraft Fan Edit — Setup", MB_OK | style);
}
static bool running(const std::wstring &exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof entry;
    bool found = false;
    if (Process32FirstW(snap, &entry)) do {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!p) continue;
        wchar_t path[32768]; DWORD size = 32768;
        if (QueryFullProcessImageNameW(p, 0, path, &size) && !_wcsicmp(path, exe.c_str())) found = true;
        CloseHandle(p);
    } while (!found && Process32NextW(snap, &entry));
    CloseHandle(snap);
    return found;
}
static void configureGameFolder() {
    if (MessageBoxW(nullptr, L"Choose your local StarCraft installation for HD graphics and mining?\n\n"
        L"Choose the game folder containing Data. You can skip this to use automatic detection.",
        L"ScmDraft Fan Edit — Game Data", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    for (;;) {
        BROWSEINFOW browse{};
        browse.lpszTitle = L"Choose the StarCraft game folder containing Data";
        browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE item = SHBrowseForFolderW(&browse);
        if (!item) break;
        wchar_t folder[MAX_PATH]{};
        bool got = !!SHGetPathFromIDListW(item, folder);
        CoTaskMemFree(item);
        const std::wstring path = folder;
        DWORD attrs = GetFileAttributesW((path + L"\\Data").c_str());
        if (!got || attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            message(L"This folder does not contain a Data directory. Choose the game installation folder.");
            continue;
        }
        LONG result = RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\ScmDraft HD", L"StarcraftPath",
            REG_SZ, folder, static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
        if (result != ERROR_SUCCESS) message(L"Could not save the game folder. The plugin will use automatic detection.");
        break;
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
}
static const std::vector<std::wstring> payloadNames{
    L"scmhd.sdp", L"minesim.exe", L"ScmDraft-Fan-Edit-NOTICES.txt"};
struct Payload {
    std::wstring directory;
    ~Payload() {
        if (directory.empty()) return;
        for (const auto &name : payloadNames) DeleteFileW((directory + L"\\" + name).c_str());
        RemoveDirectoryW(directory.c_str());
    }
    bool extract() {
        wchar_t temp[MAX_PATH]{}, folder[MAX_PATH]{};
        DWORD length = GetTempPathW(MAX_PATH, temp);
        if (!length || length >= MAX_PATH || !GetTempFileNameW(temp, L"sfe", 0, folder)) return false;
        DeleteFileW(folder);
        if (!CreateDirectoryW(folder, nullptr)) return false;
        directory = folder;
        for (size_t i = 0; i < payloadNames.size(); ++i) {
            HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(201 + i), RT_RCDATA);
            if (!resource) return false;
            DWORD size = SizeofResource(nullptr, resource);
            const void *bytes = LockResource(LoadResource(nullptr, resource));
            if (!size || !bytes) return false;
            HANDLE file = CreateFileW((directory + L"\\" + payloadNames[i]).c_str(), GENERIC_WRITE,
                0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
            CloseHandle(file);
            if (!ok) return false;
        }
        return true;
    }
};
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t exe[32768]{};
    OPENFILENAMEW dialog{}; dialog.lStructSize = sizeof dialog;
    dialog.lpstrTitle = L"Select your existing ScmDraft 2 executable";
    dialog.lpstrFilter = L"ScmDraft executable (*.exe)\0*.exe\0\0";
    dialog.lpstrFile = exe; dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return 0;
    if (!fan_edit::supported(exe)) {
        message(L"This editor build is not supported. Nothing was installed.\n\n"
                L"Fan Edit requires the exact ScmDraft 0.9.10 build documented in INSTALL.md.");
        return 1;
    }
    if (running(exe)) {
        message(L"Close this ScmDraft instance before installing, then run Setup again."); return 1;
    }
    Payload payload;
    if (!payload.extract()) {
        message(L"Could not unpack the installer. Check free disk space and download Setup again."); return 1;
    }
    std::wstring source = payload.directory;
    std::wstring target = fan_edit::parent(exe) + L"\\plugins";
    const auto &names = payloadNames;
    if (MessageBoxW(nullptr, (L"Install Fan Edit into:\n" + target +
        L"\n\nExisting Fan Edit files will be backed up. The original editor and maps are not changed.").c_str(),
        L"ScmDraft Fan Edit — Setup", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) return 0;
    if (!CreateDirectoryW(target.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        message(L"Cannot create the plugins folder. Use a writable ScmDraft installation folder."); return 1;
    }
    // Unique backup/staging directory on the destination volume: rename and rollback stay local.
    wchar_t temp[MAX_PATH];
    if (!GetTempFileNameW(target.c_str(), L"sfe", 0, temp)) {
        message(L"Cannot create a backup folder. Nothing was installed."); return 1;
    }
    DeleteFileW(temp);
    if (!CreateDirectoryW(temp, nullptr)) return 1;
    std::wstring backup = temp;
    size_t installed = 0;
    bool ok = true;
    for (const auto &name : names) {
        const auto dest = target + L"\\" + name;
        // Stage all payloads and save all existing files before replacing anything.
        if (!CopyFileW((source + L"\\" + name).c_str(), (backup + L"\\new-" + name).c_str(), TRUE)) { ok = false; break; }
        if (fan_edit::fileExists(dest) && !CopyFileW(dest.c_str(), (backup + L"\\" + name).c_str(), TRUE)) { ok = false; break; }
    }
    if (ok) for (const auto &name : names) {
        if (!MoveFileExW((backup + L"\\new-" + name).c_str(), (target + L"\\" + name).c_str(), MOVEFILE_REPLACE_EXISTING)) { ok = false; break; }
        ++installed;
    }
    if (!ok) {
        bool restored = true;
        for (size_t i = 0; i < installed; ++i) {
            const auto old = backup + L"\\" + names[i], dest = target + L"\\" + names[i];
            if (fan_edit::fileExists(old)) restored = !!CopyFileW(old.c_str(), dest.c_str(), FALSE) && restored;
            else restored = !!DeleteFileW(dest.c_str()) && restored;
        }
        message((restored ? L"Installation failed; any replaced files were restored.\n\n" :
            L"Installation failed and automatic restoration was incomplete. Restore files from:\n\n") + backup);
        return 1;
    }
    configureGameFolder();
    message(L"Fan Edit installed. Start ScmDraft normally.\n\n"
            L"See INSTALL.md for selecting your StarCraft data and enabling HD graphics.\n\n"
            L"Backup folder (if previous files existed):\n" + backup, MB_ICONINFORMATION);
    return 0;
}
