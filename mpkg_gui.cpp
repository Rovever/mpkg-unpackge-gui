/*
 * mpkg_gui.cpp - MPKG 解包工具（纯 Win32 GUI，无第三方依赖）
 *
 * 解包算法：mpkg-unpackge_fixed.cpp（顺序读取数据区）
 * 格式来源：https://github.com/beyonik/mpkg-extractor  （作者 beyonik，src/main.c，PKGM0014）
 *          原项目未声明开源许可，此处仅作格式参考与署名标注。
 *
 * 仅链接 Windows 系统库：user32 / comctl32 / comdlg32 / shell32 / ole32。
 * 编译：rc /fo mpkg_gui.res mpkg_gui.rc
 *       cl /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE
 *       /Fe:mpkg_gui.exe mpkg_gui.cpp mpkg_gui.res /link /SUBSYSTEM:WINDOWS
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <set>

#include "mpkg_core.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

/* 与 mpkg_gui.rc 中的图标资源 ID 保持一致 */
#define IDI_APPICON 4001

namespace fs = std::filesystem;
using namespace std::chrono;

enum : int {
    ID_BTN_FILES = 101, ID_BTN_FOLDER, ID_BTN_REMOVE, ID_BTN_CLEAR,
    ID_BTN_OUT, ID_BTN_OUT_CLEAR, ID_BTN_START,
    ID_LIST, ID_OUT_EDIT, ID_STATUS, ID_TIP, ID_CREDIT
};

static const UINT WM_APP_ROW    = WM_APP + 1;  /* wParam=行号, lParam=new std::wstring 状态 */
static const UINT WM_APP_STATUS = WM_APP + 2;  /* lParam=new std::wstring 底部文字 */
static const UINT WM_APP_DONE   = WM_APP + 3;  /* lParam=new std::wstring 汇总文字 */

static HWND   g_hwnd = nullptr;
static HWND   g_list = nullptr;
static HWND   g_outEdit = nullptr;
static HWND   g_status = nullptr;
static HWND   g_btnFiles = nullptr, g_btnFolder = nullptr, g_btnRemove = nullptr;
static HWND   g_btnClear = nullptr, g_btnOut = nullptr, g_btnOutClear = nullptr;
static HWND   g_btnStart = nullptr;
static HFONT  g_font = nullptr;

static std::vector<fs::path> g_files;
static std::vector<std::wstring> g_rowStatus;   /* 每行完整状态文本，供悬停提示显示 */
static std::set<std::wstring> g_seen;
static std::wstring g_outDir;
static bool g_busy = false;
static std::thread g_worker;

static const wchar_t* kDefaultOutHint = L"（默认：各文件解包到其原所在文件夹）";

/* 所有布局尺寸以 96 DPI 为基准，按当前显示器 DPI 等比缩放，
   否则高 DPI 下字体会被放大而控件仍是原尺寸，文字会被裁切。 */
static int g_dpi = 96;
static int S(int px) { return MulDiv(px, g_dpi, 96); }

/* 64 位下 int -> HMENU 需要先扩到指针宽度，避免 C4312 */
static HMENU CtlId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

static void PostText(UINT msg, WPARAM wp, const std::wstring& text) {
    auto* p = new std::wstring(text);
    if (!PostMessageW(g_hwnd, msg, wp, reinterpret_cast<LPARAM>(p))) delete p;
}

static void SetRowText(int row, const std::wstring& text) {
    if (row >= 0 && row < static_cast<int>(g_rowStatus.size()))
        g_rowStatus[static_cast<size_t>(row)] = text;
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.iSubItem = 1;
    item.pszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(g_list, LVM_SETITEMW, 0, reinterpret_cast<LPARAM>(&item));
    if (g_busy) {
        SendMessageW(g_list, LVM_ENSUREVISIBLE, static_cast<WPARAM>(row), FALSE);
    }
}

static void AddRow(const fs::path& p) {
    std::wstring s = p.wstring();
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = static_cast<int>(g_files.size());
    item.pszText = const_cast<LPWSTR>(s.c_str());
    const int idx = static_cast<int>(
        SendMessageW(g_list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item)));
    if (idx < 0) return;
    g_rowStatus.push_back(L"等待");
    SetRowText(idx, L"等待");
}

static void UpdateCount() {
    if (g_busy) return;
    std::wstring s = L"就绪 · 共 " + std::to_wstring(g_files.size()) + L" 个 mpkg 文件";
    SetWindowTextW(g_status, s.c_str());
}

static void AddPaths(const std::vector<fs::path>& paths) {
    size_t added = 0;
    for (const auto& p : paths) {
        std::vector<fs::path> found;
        mpkg::Collect(p, found);
        for (const auto& f : found) {
            if (g_seen.insert(mpkg::Key(f)).second) {
                g_files.push_back(f);
                AddRow(f);
                ++added;
            }
        }
    }
    if (added == 0 && !paths.empty() && IsWindowVisible(g_hwnd)) {
        MessageBoxW(g_hwnd, L"没有找到新的 .mpkg 文件（可能已存在或目录中不含 mpkg）。",
                    L"提示", MB_OK | MB_ICONINFORMATION);
    }
    UpdateCount();
}

static void RemoveSelected() {
    std::vector<int> rows;
    int i = static_cast<int>(SendMessageW(g_list, LVM_GETNEXTITEM,
                                          static_cast<WPARAM>(-1), LVNI_SELECTED));
    while (i >= 0) {
        rows.push_back(i);
        i = static_cast<int>(SendMessageW(g_list, LVM_GETNEXTITEM,
                                          static_cast<WPARAM>(i), LVNI_SELECTED));
    }
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const int idx = *it;
        if (idx < 0 || idx >= static_cast<int>(g_files.size())) continue;
        g_seen.erase(mpkg::Key(g_files[static_cast<size_t>(idx)]));
        g_files.erase(g_files.begin() + idx);
        g_rowStatus.erase(g_rowStatus.begin() + idx);
        SendMessageW(g_list, LVM_DELETEITEM, static_cast<WPARAM>(idx), 0);
    }
    UpdateCount();
}

static void ClearList() {
    SendMessageW(g_list, LVM_DELETEALLITEMS, 0, 0);
    g_files.clear();
    g_rowStatus.clear();
    g_seen.clear();
    UpdateCount();
}

/* GetOpenFileNameW + OFN_ALLOWMULTISELECT：返回用户选中的完整路径列表 */
static std::vector<fs::path> PickFiles() {
    std::vector<wchar_t> buf(65536, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(OPENFILENAMEW);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"MPKG 文件 (*.mpkg)\0*.mpkg\0所有文件 (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"选择一个或多个 mpkg 文件";
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return {};

    std::vector<fs::path> result;
    const wchar_t* p = buf.data();
    const std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') {
        result.push_back(fs::path(first));
    } else {
        while (*p != L'\0') {
            const std::wstring name = p;
            result.push_back(fs::path(first) / name);
            p += name.size() + 1;
        }
    }
    return result;
}

static std::wstring PickFolder(const wchar_t* title) {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_NEWDIALOGSTYLE | BIF_RETURNONLYFSDIRS | BIF_EDITBOX;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return L"";
    wchar_t path[4096] = {0};
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    return ok ? std::wstring(path) : L"";
}

static std::wstring FormatSeconds(double sec) {
    wchar_t buf[64];
    swprintf(buf, 64, L"%.2f", sec);
    return std::wstring(buf);
}

/* 后台线程：逐个解包，用 PostMessage 把状态交回 UI 线程刷新 */
static void Worker(std::vector<fs::path> files, std::wstring outDir) {
    const auto t0 = steady_clock::now();
    int ok = 0, fail = 0;
    std::map<std::wstring, int> usedNames;

    for (size_t i = 0; i < files.size(); ++i) {
        const fs::path& file = files[i];
        PostText(WM_APP_ROW, static_cast<WPARAM>(i), L"处理中…");
        PostText(WM_APP_STATUS, 0,
                 L"正在处理 " + std::to_wstring(i + 1) + L"/" +
                 std::to_wstring(files.size()) + L"：" + file.filename().wstring());

        const auto t1 = steady_clock::now();
        try {
            const fs::path root = outDir.empty() ? file.parent_path() : fs::path(outDir);
            const std::wstring stem = file.stem().wstring();
            /* 同一批里同名文件分别落到 stem / stem (2) / stem (3)，避免互相覆盖 */
            const std::wstring key = mpkg::Key(root / stem);
            const int n = ++usedNames[key];
            const std::wstring sub = (n == 1) ? stem
                                               : stem + L" (" + std::to_wstring(n) + L")";
            const mpkg::Result r = mpkg::Unpack(file, root, sub);
            const double sec = duration<double>(steady_clock::now() - t1).count();
            PostText(WM_APP_ROW, static_cast<WPARAM>(i),
                     L"完成 · " + FormatSeconds(sec) + L" s · " +
                     std::to_wstring(r.count) + L" 个文件 → " +
                     r.folder.filename().wstring());
            ++ok;
        } catch (const mpkg::Error& e) {
            PostText(WM_APP_ROW, static_cast<WPARAM>(i), L"失败：" + e.message);
            ++fail;
        } catch (const std::exception& e) {
            PostText(WM_APP_ROW, static_cast<WPARAM>(i),
                     L"失败：" + mpkg::Utf8ToWide(e.what()));
            ++fail;
        } catch (...) {
            PostText(WM_APP_ROW, static_cast<WPARAM>(i), L"失败：未知错误");
            ++fail;
        }
    }

    const double total = duration<double>(steady_clock::now() - t0).count();
    PostText(WM_APP_DONE, 0,
             L"完成：成功 " + std::to_wstring(ok) + L" 个，失败 " +
             std::to_wstring(fail) + L" 个，总用时 " + FormatSeconds(total) + L" 秒");
}

static void StartWork() {
    if (g_busy) return;
    if (g_files.empty()) {
        MessageBoxW(g_hwnd, L"请先添加要解包的 .mpkg 文件。", L"提示",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    g_busy = true;
    for (HWND b : {g_btnFiles, g_btnFolder, g_btnRemove, g_btnClear,
                   g_btnOut, g_btnOutClear, g_btnStart})
        EnableWindow(b, FALSE);
    SetWindowTextW(g_status, L"正在处理…");
    if (g_worker.joinable()) g_worker.join();
    g_worker = std::thread(Worker, g_files, g_outDir);
}

static void FinishWork(const std::wstring& summary) {
    SetWindowTextW(g_status, summary.c_str());
    for (HWND b : {g_btnFiles, g_btnFolder, g_btnRemove, g_btnClear,
                   g_btnOut, g_btnOutClear, g_btnStart})
        EnableWindow(b, TRUE);
    g_busy = false;
    if (g_worker.joinable()) g_worker.join();
}

static void SetFont(HWND h) {
    if (g_font) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
}

static HWND MakeButton(int id, const wchar_t* text, HWND parent) {
    HWND h = CreateWindowExW(0, L"BUTTON", text,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 10, 10, parent, CtlId(id),
                             nullptr, nullptr);
    SetFont(h);
    return h;
}

static HWND MakeLabel(int id, const wchar_t* text, HWND parent) {
    HWND h = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                             0, 0, 10, 10, parent, CtlId(id),
                             nullptr, nullptr);
    SetFont(h);
    return h;
}

static void Layout(int w, int h) {
    const int M      = S(12);
    const int labelW = S(84);
    const int bh     = S(30);
    const int x0     = M + labelW;

    /* 第一行：文件 / 文件夹 / 移除 / 清空 */
    int x = x0;
    MoveWindow(g_btnFiles,  x, M, S(148), bh, TRUE); x += S(154);
    MoveWindow(g_btnFolder, x, M, S(160), bh, TRUE); x += S(166);
    MoveWindow(g_btnRemove, x, M, S(96),  bh, TRUE); x += S(102);
    MoveWindow(g_btnClear,  x, M, S(96),  bh, TRUE);

    /* 第二行：输出目录 */
    const int row2   = M + bh + S(10);
    const int rightW = S(104) + S(8) + S(104);
    const int editW  = std::max(S(160), w - x0 - rightW - M - S(16));
    MoveWindow(g_outEdit, x0, row2, editW, bh, TRUE);
    const int bx = x0 + editW + S(8);
    MoveWindow(g_btnOut, bx, row2, S(104), bh, TRUE);
    MoveWindow(g_btnOutClear, bx + S(110), row2, S(104), bh, TRUE);

    /* 提示 + 列表 + 底部按钮/状态/署名 */
    const int tipY    = row2 + bh + S(10);
    const int listY   = tipY + S(24);
    const int bottomH = S(82);
    const int listH   = std::max(S(90), h - listY - bottomH);
    MoveWindow(g_list, M, listY, w - 2 * M, listH, TRUE);

    const int btnY = listY + listH + S(15);
    MoveWindow(g_btnStart, w - M - S(132), btnY, S(132), S(32), TRUE);
    MoveWindow(g_status, M, btnY + S(4), std::max(S(160), w - 2 * M - S(146)), S(26), TRUE);
    MoveWindow(GetDlgItem(g_hwnd, ID_CREDIT), M, h - S(28), w - 2 * M, S(20), TRUE);
    MoveWindow(GetDlgItem(g_hwnd, ID_TIP), M, tipY, w - 2 * M, S(22), TRUE);
    MoveWindow(GetDlgItem(g_hwnd, 2001), M, M + S(6), labelW - S(4), S(22), TRUE);
    MoveWindow(GetDlgItem(g_hwnd, 2002), M, row2 + S(6), labelW - S(4), S(22), TRUE);

    /* 列宽跟随窗口 */
    const int listW = w - 2 * M;
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 0,
                 static_cast<LPARAM>(listW * 62 / 100));
    SendMessageW(g_list, LVM_SETCOLUMNWIDTH, 1,
                 static_cast<LPARAM>(listW - listW * 62 / 100 - S(4)));
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_btnFiles    = MakeButton(ID_BTN_FILES,     L"选择文件(可多选)…", hwnd);
        g_btnFolder   = MakeButton(ID_BTN_FOLDER,    L"选择文件夹(递归)…", hwnd);
        g_btnRemove   = MakeButton(ID_BTN_REMOVE,    L"移除选中", hwnd);
        g_btnClear    = MakeButton(ID_BTN_CLEAR,     L"清空列表", hwnd);
        g_btnOut      = MakeButton(ID_BTN_OUT,       L"选择输出…", hwnd);
        g_btnOutClear = MakeButton(ID_BTN_OUT_CLEAR, L"恢复默认", hwnd);
        g_btnStart    = MakeButton(ID_BTN_START,     L"开始处理", hwnd);

        MakeLabel(2001, L"① 添加文件", hwnd);
        MakeLabel(2002, L"② 输出目录", hwnd);
        MakeLabel(ID_TIP, L"文件列表（可直接把文件或文件夹拖进窗口，文件夹会递归搜索 .mpkg）", hwnd);
        MakeLabel(ID_CREDIT,
                  L"格式参考 github.com/beyonik/mpkg-extractor（beyonik，原项目未声明开源许可）",
                  hwnd);

        g_outEdit = CreateWindowExW(0, L"EDIT", kDefaultOutHint,
                        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_READONLY | ES_AUTOHSCROLL,
                        0, 0, 10, 10, hwnd, CtlId(ID_OUT_EDIT), nullptr, nullptr);
        SetFont(g_outEdit);

        g_status = CreateWindowExW(0, L"STATIC", L"就绪 · 共 0 个 mpkg 文件",
                        WS_CHILD | WS_VISIBLE | SS_LEFT,
                        0, 0, 10, 10, hwnd, CtlId(ID_STATUS), nullptr, nullptr);
        SetFont(g_status);

        g_list = CreateWindowExW(0, WC_LISTVIEWW, L"",
                        WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                        LVS_REPORT | LVS_SHOWSELALWAYS | LVS_ALIGNLEFT | LVS_NOSORTHEADER,
                        0, 0, 10, 10, hwnd, CtlId(ID_LIST), nullptr, nullptr);
        SetFont(g_list);
        SendMessageW(g_list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                     LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
                     LVS_EX_LABELTIP | LVS_EX_INFOTIP);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(L"文件");
        col.cx = 400; col.iSubItem = 0;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
        col.pszText = const_cast<LPWSTR>(L"状态");
        col.cx = 240; col.iSubItem = 1;
        SendMessageW(g_list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

        DragAcceptFiles(hwnd, TRUE);
        /* 以管理员运行时，允许资源管理器发来的拖拽消息穿过 UIPI */
        using CWMF = BOOL(WINAPI*)(UINT, DWORD);
        if (HMODULE u = GetModuleHandleW(L"user32.dll")) {
            if (auto fn = reinterpret_cast<CWMF>(GetProcAddress(u, "ChangeWindowMessageFilter"))) {
                fn(WM_DROPFILES, 1);      /* MSGFLT_ADD */
                fn(0x0049, 1);            /* WM_COPYGLOBALDATA */
            }
        }
        return 0;
    }

    case WM_SIZE:
        if (wp != SIZE_MINIMIZED) Layout(LOWORD(lp), HIWORD(lp));
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = S(660);
        mmi->ptMinTrackSize.y = S(440);
        return 0;
    }

    case WM_DROPFILES: {
        if (g_busy) { DragFinish(reinterpret_cast<HDROP>(wp)); return 0; }
        HDROP drop = reinterpret_cast<HDROP>(wp);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<fs::path> paths;
        for (UINT i = 0; i < count; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(static_cast<size_t>(len) + 1, L'\0');
            DragQueryFileW(drop, i, &s[0], len + 1);
            s.resize(wcslen(s.c_str()));
            paths.push_back(fs::path(s));
        }
        DragFinish(drop);
        AddPaths(paths);
        return 0;
    }

    case WM_COMMAND:
        if (g_busy) return 0;
        switch (LOWORD(wp)) {
        case ID_BTN_FILES:     AddPaths(PickFiles()); break;
        case ID_BTN_FOLDER: {
            std::wstring dir = PickFolder(L"选择要搜索 mpkg 文件的文件夹（含子文件夹）");
            if (!dir.empty()) AddPaths({fs::path(dir)});
            break;
        }
        case ID_BTN_OUT: {
            std::wstring dir = PickFolder(L"选择输出文件夹（所有结果放到这里）");
            if (!dir.empty()) {
                g_outDir = dir;
                SetWindowTextW(g_outEdit, g_outDir.c_str());
            }
            break;
        }
        case ID_BTN_OUT_CLEAR:
            g_outDir.clear();
            SetWindowTextW(g_outEdit, kDefaultOutHint);
            break;
        case ID_BTN_REMOVE:    RemoveSelected(); break;
        case ID_BTN_CLEAR:     ClearList(); break;
        case ID_BTN_START:     StartWork(); break;
        }
        return 0;

    case WM_NOTIFY: {
        auto* nm = reinterpret_cast<NMHDR*>(lp);
        if (nm->idFrom == ID_LIST && nm->code == LVN_GETINFOTIPW) {
            auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lp);
            const int row = tip->iItem;
            if (tip->pszText && tip->cchTextMax > 0 && row >= 0 &&
                row < static_cast<int>(g_rowStatus.size())) {
                const std::wstring& s = g_rowStatus[static_cast<size_t>(row)];
                const size_t n = std::min(static_cast<size_t>(tip->cchTextMax - 1), s.size());
                memcpy(tip->pszText, s.c_str(), n * sizeof(wchar_t));
                tip->pszText[n] = L'\0';
            }
            return 0;
        }
        break;
    }

    case WM_APP_ROW: {
        auto* p = reinterpret_cast<std::wstring*>(lp);
        SetRowText(static_cast<int>(wp), *p);
        delete p;
        return 0;
    }
    case WM_APP_STATUS: {
        auto* p = reinterpret_cast<std::wstring*>(lp);
        SetWindowTextW(g_status, p->c_str());
        delete p;
        return 0;
    }
    case WM_APP_DONE: {
        auto* p = reinterpret_cast<std::wstring*>(lp);
        FinishWork(*p);
        delete p;
        return 0;
    }

    case WM_CLOSE:
        if (g_busy) {
            MessageBoxW(hwnd, L"正在解包，请等待处理完成后再关闭窗口。", L"请稍候",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_worker.joinable()) g_worker.detach();
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    OleInitialize(nullptr);
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    HDC dc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(dc, LOGPIXELSY);
    if (g_dpi < 96) g_dpi = 96;
    ReleaseDC(nullptr, dc);
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                         L"Microsoft YaHei");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    /* 优先用 mpkg_gui.rc 编入的应用图标；未链接 .res（例如单元测试）时回退系统默认图标 */
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MpkgGuiWnd";
    if (!RegisterClassExW(&wc)) { OleUninitialize(); return 1; }

    const int w = S(860), h = S(600);
    const int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    const int y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    HWND hwnd = CreateWindowExW(0, L"MpkgGuiWnd", L"mpkg 解包工具",
                                WS_OVERLAPPEDWINDOW, x, y, w, h,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) { OleUninitialize(); return 1; }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    /* 支持把文件/文件夹直接拖到 exe 图标上启动：命令行参数当作待处理路径 */
    int argCount = 0;
    LPWSTR* argList = CommandLineToArgvW(GetCommandLineW(), &argCount);
    if (argList) {
        std::vector<fs::path> initial;
        for (int i = 1; i < argCount; ++i)
            if (argList[i][0] != L'-') initial.emplace_back(argList[i]);
        LocalFree(argList);
        if (!initial.empty()) AddPaths(initial);
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    OleUninitialize();
    return 0;
}
