/*
 * self_test.cpp - 回归验证（不显示任何窗口、不需要真实 mpkg 文件）
 *
 * 通过 #define 把 GUI 的 wWinMain 改名后整体包含进来，因此可以：
 *   1) 直接调用 mpkg::Unpack / Collect / AddUnique 验证解包核心；
 *   2) 创建一个隐藏主窗口，验证 WM_CREATE 控件创建、布局、
 *      递归添加、去重、移除选中、清空列表等 UI 逻辑。
 *
 * 运行后会在 tests/fixtures 生成一批合成 mpkg 样本，可直接拖进 GUI 试跑。
 */
#define NOMINMAX
#define wWinMain mpkg_gui_disabled_entry
#include "../mpkg_gui.cpp"
#undef wWinMain

#include <iostream>
#include <fstream>
#include <sstream>

static int g_pass = 0, g_fail = 0;

static void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << "\n";
    ok ? ++g_pass : ++g_fail;
}

static void PutU32(std::ofstream& f, uint32_t v) {
    const char b[4] = {
        static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
        static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)
    };
    f.write(b, 4);
}

struct Item { std::string name, data; };

static void MakeMpkg(const fs::path& p, const std::vector<Item>& items) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    PutU32(f, 8);
    f.write("PKGM0014", 8);
    PutU32(f, static_cast<uint32_t>(items.size()));
    uint32_t offset = 0;
    for (const auto& it : items) {
        PutU32(f, static_cast<uint32_t>(it.name.size()));
        f.write(it.name.data(), static_cast<std::streamsize>(it.name.size()));
        PutU32(f, offset);                                       /* start_pos */
        PutU32(f, static_cast<uint32_t>(it.data.size()));        /* file_size */
        offset += static_cast<uint32_t>(it.data.size());
    }
    for (const auto& it : items)
        f.write(it.data.data(), static_cast<std::streamsize>(it.data.size()));
}

static std::string ReadBytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/* 构造真实的 HDROP 并发送 WM_DROPFILES，验证拖拽消息解析（DragFinish 会释放内存） */
static void SendDrop(HWND hwnd, const std::vector<fs::path>& paths) {
    std::wstring names;
    for (const auto& p : paths) { names += p.wstring(); names += L'\0'; }
    names += L'\0';
    const size_t bytes = names.size() * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GHND, sizeof(DROPFILES) + bytes);
    if (!h) return;
    auto* df = reinterpret_cast<DROPFILES*>(GlobalLock(h));
    df->pFiles = sizeof(DROPFILES);
    df->pt.x = df->pt.y = 0;
    df->fNC = FALSE;
    df->fWide = TRUE;
    memcpy(reinterpret_cast<char*>(df) + sizeof(DROPFILES), names.c_str(), bytes);
    GlobalUnlock(h);
    SendMessageW(hwnd, WM_DROPFILES, reinterpret_cast<WPARAM>(h), 0);
}

int main(int argc, char** argv) {
    const fs::path root = fs::path(argv[0]).parent_path() / "fixtures";
    fs::remove_all(root);
    const fs::path src = root / "src";
    const fs::path out = root / "out";
    fs::create_directories(out);

    const std::string readme = "hello mpkg";
    const std::string logo(2560, 'L');
    const std::string png = std::string("\x89PNG\r\n\x1a\n", 8) + std::string(100, 'P');
    const std::string utf8Name = u8"中文目录/图片.png";

    MakeMpkg(src / "sample.mpkg", {
        {"readme.txt", readme},
        {"assets/logo.bin", logo},
        {utf8Name, png},
    });
    MakeMpkg(src / "nested" / "deep.mpkg", {{"a.txt", std::string(1234, 'A')}});
    MakeMpkg(src / "nested" / "sample.mpkg", {{"b.txt", std::string(50, 'B')}});
    MakeMpkg(src / "gbk.mpkg", {{"\xD6\xD0\xCE\xC4.bin", "gbk name"}});

    {   /* 损坏样本：头部长度字段异常 */
        std::ofstream f(src / "broken.mpkg", std::ios::binary);
        PutU32(f, 0xFFFFFF00u);
        f.write("xx", 2);
    }
    { std::ofstream f(src / "note.txt"); f << "not an mpkg"; }

    std::cout << "=== core: format / unpack / validation ===\n";

    std::vector<fs::path> collected;
    mpkg::Collect(src, collected);
    Check(collected.size() == 5, "recursive collect finds 5 .mpkg (non-recursive file ignored)");

    std::vector<fs::path> list;
    std::set<std::wstring> seen;
    const size_t a1 = mpkg::AddUnique(src, list, seen);
    const size_t a2 = mpkg::AddUnique(src / "sample.mpkg", list, seen);
    Check(a1 == 5 && a2 == 0 && list.size() == 5, "unique add: duplicates ignored");

    try {
        const mpkg::Result r = mpkg::Unpack(src / "sample.mpkg", out);
        Check(r.version == L"PKGM0014", "header version parsed");
        Check(r.count == 3, "file count = 3");
        Check(r.bytes == readme.size() + logo.size() + png.size(), "bytes written match payload sizes");
        Check(r.folder.filename() == L"sample", "output folder named after source stem");
        Check(ReadBytes(r.folder / L"readme.txt") == "hello mpkg", "plain file content intact");
        Check(ReadBytes(r.folder / L"assets" / L"logo.bin") == logo, "nested binary content intact");
        Check(fs::exists(r.folder / L"中文目录" / L"图片.png"), "UTF-8 Chinese nested name decoded");
        Check(ReadBytes(r.folder / L"中文目录" / L"图片.png") == png, "Chinese-named file content intact");
    } catch (const mpkg::Error& e) {
        Check(false, "unpack sample threw unexpectedly");
    }

    try {
        const mpkg::Result r2 = mpkg::Unpack(src / "nested" / "sample.mpkg", out, L"sample (2)");
        Check(r2.folder.filename() == L"sample (2)", "subName override prevents same-stem overwrite");
        Check(ReadBytes(r2.folder / L"b.txt") == std::string(50, 'B'), "second package content intact");
    } catch (const mpkg::Error&) {
        Check(false, "unpack second sample threw unexpectedly");
    }

    try {
        const mpkg::Result r3 = mpkg::Unpack(src / "gbk.mpkg", out);
        Check(fs::exists(r3.folder / L"中文.bin"), "GBK (ANSI) filename fallback decoded");
    } catch (const mpkg::Error&) {
        Check(false, "GBK name package threw unexpectedly");
    }

    bool rejected = false;
    try { mpkg::Unpack(src / "broken.mpkg", out); }
    catch (const mpkg::Error&) { rejected = true; }
    Check(rejected, "corrupted header length rejected instead of huge allocation");

    bool traversalRejected = false;
    {
        MakeMpkg(src / "_traversal.mpkg", {{"../escape.txt", "bad"}});
        try { mpkg::Unpack(src / "_traversal.mpkg", out); }
        catch (const mpkg::Error&) { traversalRejected = true; }
        fs::remove(src / "_traversal.mpkg");
    }
    Check(traversalRejected, "path traversal (..) in package name rejected");

    std::cout << "=== gui: hidden window / controls / list operations ===\n";

    OleInitialize(nullptr);
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MpkgGuiSmokeWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(0, L"MpkgGuiSmokeWnd", L"smoke",
                                WS_OVERLAPPEDWINDOW, 0, 0, 860, 600,
                                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(hwnd != nullptr && g_hwnd == hwnd, "hidden main window + WM_CREATE succeeded");
    SendMessageW(hwnd, WM_SIZE, SIZE_RESTORED, MAKELPARAM(860, 600));
    Check(GetDlgItem(hwnd, ID_LIST) != nullptr, "file listview created");
    Check(GetDlgItem(hwnd, ID_OUT_EDIT) != nullptr, "output edit created");
    Check(GetDlgItem(hwnd, ID_BTN_START) != nullptr, "start button created");
    Check(GetDlgItem(hwnd, ID_BTN_FILES) != nullptr, "multi-select files button created");

    HWND header = reinterpret_cast<HWND>(SendMessageW(g_list, LVM_GETHEADER, 0, 0));
    const int cols = header ? static_cast<int>(SendMessageW(header, HDM_GETITEMCOUNT, 0, 0)) : -1;
    Check(cols == 2, "listview has 2 columns (file / status)");

    AddPaths({src});
    int rows = static_cast<int>(SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0));
    Check(g_files.size() == 5 && rows == 5, "folder drop recursively adds 5 rows");

    AddPaths({src});
    Check(g_files.size() == 5, "dropping same folder again adds nothing (dedup, no dialog)");

    LVITEMW sel{};
    sel.stateMask = LVIS_SELECTED;
    sel.state = LVIS_SELECTED;
    SendMessageW(g_list, LVM_SETITEMSTATE, 0, reinterpret_cast<LPARAM>(&sel));
    RemoveSelected();
    Check(g_files.size() == 4, "remove selected deletes exactly one row");
    ClearList();
    rows = static_cast<int>(SendMessageW(g_list, LVM_GETITEMCOUNT, 0, 0));
    Check(g_files.empty() && rows == 0, "clear list empties both model and view");

    SendDrop(hwnd, {src / "sample.mpkg", src / "nested"});
    Check(g_files.size() == 3, "WM_DROPFILES handles mixed file + folder drop");

    /* 端到端：后台线程解包 -> PostMessage 刷新 -> 汇总用时 */
    StartWork();
    MSG msg{};
    int guard = 0;
    while (g_busy && guard++ < 10000 && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Check(!g_busy, "worker finished, buttons re-enabled");
    wchar_t summary[512] = {0};
    GetWindowTextW(g_status, summary, 512);
    Check(wcsstr(summary, L"完成") != nullptr && wcsstr(summary, L"秒") != nullptr,
          "completion summary shows counts and elapsed time");
    wchar_t rowText[256] = {0};
    LVITEMW getItem{};
    getItem.iSubItem = 1;
    getItem.pszText = rowText;
    getItem.cchTextMax = 256;
    SendMessageW(g_list, LVM_GETITEMTEXTW, 0, reinterpret_cast<LPARAM>(&getItem));
    Check(wcsstr(rowText, L"完成") != nullptr || wcsstr(rowText, L"失败") != nullptr,
          "rows updated from 'waiting' to final status");

    /* 输出目录留空时，Unpack 应落到源文件所在目录 */
    try {
        const mpkg::Result r4 = mpkg::Unpack(src / "nested" / "deep.mpkg", fs::path(), L"deep_inplace");
        Check(r4.folder.parent_path() == (src / "nested"), "empty output dir -> source file's own folder");
        fs::remove_all(r4.folder);
    } catch (const mpkg::Error&) {
        Check(false, "in-place output threw unexpectedly");
    }

    DestroyWindow(hwnd);
    OleUninitialize();

    std::cout << "\n=== " << g_pass << " passed, " << g_fail << " failed ===\n";
    std::cout << "fixtures kept at: " << root.u8string() << "\n";
    return g_fail == 0 ? 0 : 1;
}
