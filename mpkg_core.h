/*
 * mpkg_core.h - MPKG 解包核心
 *
 * 来源：
 *   1) 本目录 mpkg-unpackge_fixed.cpp（用户修改版，采用「顺序读取数据区」方式）
 *   2) 原始项目 https://github.com/beyonik/mpkg-extractor （作者 beyonik，src/main.c，
 *      按 start_pos + header_end 定位数据区；header 标识 PKGM0014）
 *
 * 本文件保持 fixed.cpp 的解包算法不变（4 字节头长 + 头部 + 文件数 +
 * [文件名长度 + 文件名 + 4 字节偏移 + 4 字节大小] * N + 连续数据区），
 * 仅补充：数值合理性校验、文件名 UTF-8/ANSI 自动解码、路径穿越防护、
 * 递归搜集与去重，供 GUI 与回归测试共用。
 *
 * 依赖：C++17 标准库 + Windows 系统 API（MultiByteToWideChar），无第三方库。
 */
#ifndef MPKG_CORE_H
#define MPKG_CORE_H

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#include <windows.h>

namespace fs = std::filesystem;

namespace mpkg {

constexpr std::size_t kBufferSize = 1024 * 1024;
constexpr uint32_t kMaxHeaderLen = 256;
constexpr uint32_t kMaxNameLen = 4096;
constexpr uint32_t kMaxFileCount = 100000;

/* 用宽字符承载错误信息，避免中文在 GUI 里二次转码 */
struct Error {
    std::wstring message;
};

struct Entry {
    std::wstring name;
    uint32_t size = 0;
};

struct Result {
    std::wstring version;   /* 头部版本字符串，如 PKGM0014 */
    uint32_t count = 0;     /* 包内文件数 */
    uint64_t bytes = 0;     /* 实际写出的字节数 */
    fs::path folder;        /* 本次解包的目标文件夹 */
};

/* 包内文件名可能是 UTF-8，也可能沿用系统 ANSI(GBK)：先严格按 UTF-8 解码，
   失败再回退到当前代码页。纯 ASCII 两种情况结果一致。 */
inline std::wstring DecodeName(const std::string& s) {
    if (s.empty()) return L"";
    const int len = static_cast<int>(s.size());
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), len, nullptr, 0);
    if (n > 0) {
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.c_str(), len, &w[0], n);
        while (!w.empty() && w.back() == L'\0') w.pop_back();
        return w;
    }
    n = MultiByteToWideChar(CP_ACP, 0, s.c_str(), len, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), len, &w[0], n);
    while (!w.empty() && w.back() == L'\0') w.pop_back();
    return w;
}

inline std::wstring Utf8ToWide(const std::string& s) { return DecodeName(s); }

inline uint32_t ReadU32(std::ifstream& in) {
    unsigned char b[4] = {0, 0, 0, 0};
    in.read(reinterpret_cast<char*>(b), 4);
    if (in.gcount() != 4)
        throw Error{L"读取 4 字节整数失败：文件不完整或已损坏"};
    return static_cast<uint32_t>(b[0]) |
           (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

inline void CopyData(std::ifstream& in, std::ofstream& out, uint32_t length) {
    std::vector<char> buf(kBufferSize);
    uint64_t remaining = length;
    while (remaining > 0) {
        const std::size_t want = static_cast<std::size_t>(
            std::min<uint64_t>(kBufferSize, remaining));
        in.read(buf.data(), static_cast<std::streamsize>(want));
        const std::streamsize got = in.gcount();
        if (got <= 0)
            throw Error{L"数据区提前结束，预期 " + std::to_wstring(length) + L" 字节，剩余 " +
                        std::to_wstring(remaining) + L" 字节（文件可能损坏）"};
        out.write(buf.data(), got);
        if (!out) throw Error{L"写入输出文件失败（磁盘已满或无权限？）"};
        remaining -= static_cast<uint64_t>(got);
    }
}

/* 拒绝绝对路径与 .. 段，避免包内恶意文件名写出到目标目录之外 */
inline void CheckRelativeName(const std::wstring& name) {
    if (name.empty()) throw Error{L"包内含空文件名"};
    if (name.find(L'\0') != std::wstring::npos) throw Error{L"包内文件名含非法字符"};
    const fs::path p(name);
    if (p.is_absolute() || p.has_root_name())
        throw Error{L"包内含绝对路径，已拒绝：" + name};
    for (const auto& part : p) {
        if (part == L"..") throw Error{L"包内含越界路径，已拒绝：" + name};
    }
}

inline bool IsMpkg(const fs::path& p) {
    std::wstring ext = p.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return ext == L".mpkg";
}

inline std::wstring Key(const fs::path& p) {
    std::error_code ec;
    const fs::path canonical = fs::weakly_canonical(p, ec);
    std::wstring s = (ec ? p : canonical).wstring();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(c));
    });
    return s;
}

/* 传入单个 .mpkg 文件则直接加入；传入目录则递归搜索所有 .mpkg */
inline void Collect(const fs::path& p, std::vector<fs::path>& out) {
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        fs::recursive_directory_iterator it(
            p, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) return;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            const auto& entry = *it;
            if (entry.is_regular_file(ec) && !ec && IsMpkg(entry.path()))
                out.push_back(entry.path());
            ec.clear();
        }
    } else if (IsMpkg(p)) {
        out.push_back(p);
    }
}

/* 把一条路径（文件或目录）展开后去重加入列表 */
inline size_t AddUnique(const fs::path& p, std::vector<fs::path>& list,
                        std::set<std::wstring>& seen) {
    std::vector<fs::path> found;
    Collect(p, found);
    size_t added = 0;
    for (auto& f : found) {
        if (seen.insert(Key(f)).second) {
            list.push_back(std::move(f));
            ++added;
        }
    }
    return added;
}

/*
 * 解包单个 mpkg。
 *   outputDir 为输出根目录；默认在其下建立「源文件名(不含扩展名)」子文件夹，
 *   与 fixed.cpp 一致。subName 非空时改用它（GUI 用它规避同名文件互相覆盖）。
 *   若 outputDir 为空，调用方应传入源文件所在目录。
 */
inline Result Unpack(const fs::path& input, const fs::path& outputDir,
                     const std::wstring& subName = L"") {
    std::ifstream in(input, std::ios::binary);
    if (!in.is_open()) throw Error{L"无法打开文件：" + input.wstring()};

    Result result;
    const uint32_t headerLen = ReadU32(in);
    if (headerLen == 0 || headerLen > kMaxHeaderLen)
        throw Error{L"头部长度异常（" + std::to_wstring(headerLen) + L"），文件可能不是 mpkg"};

    std::string headerBytes(static_cast<size_t>(headerLen), '\0');
    in.read(&headerBytes[0], static_cast<std::streamsize>(headerLen));
    if (in.gcount() != static_cast<std::streamsize>(headerLen))
        throw Error{L"读取头部失败：文件不完整"};
    result.version = DecodeName(headerBytes);

    const uint32_t count = ReadU32(in);
    if (count > kMaxFileCount)
        throw Error{L"文件数量异常（" + std::to_wstring(count) + L"），已拒绝处理"};
    result.count = count;

    std::vector<Entry> entries;
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t nameLen = ReadU32(in);
        if (nameLen == 0 || nameLen > kMaxNameLen)
            throw Error{L"第 " + std::to_wstring(i + 1) + L" 项文件名长度异常（" +
                        std::to_wstring(nameLen) + L"）"};
        std::string rawName(static_cast<size_t>(nameLen), '\0');
        in.read(&rawName[0], static_cast<std::streamsize>(nameLen));
        if (in.gcount() != static_cast<std::streamsize>(nameLen))
            throw Error{L"读取第 " + std::to_wstring(i + 1) + L" 项文件名失败"};
        in.ignore(4);  /* start_pos：fixed.cpp 的顺序读取方式跳过该字段 */
        if (!in) throw Error{L"读取第 " + std::to_wstring(i + 1) + L" 项偏移字段失败"};
        const uint32_t fileSize = ReadU32(in);

        std::wstring name = DecodeName(rawName);
        CheckRelativeName(name);
        entries.push_back({std::move(name), fileSize});
    }

    const fs::path root = outputDir.empty() ? input.parent_path() : outputDir;
    const std::wstring folderName = subName.empty() ? input.stem().wstring() : subName;
    result.folder = root / folderName;
    std::error_code ec;
    fs::create_directories(result.folder, ec);
    if (ec && !fs::is_directory(result.folder))
        throw Error{L"无法创建输出文件夹：" + result.folder.wstring()};

    for (const auto& entry : entries) {
        const fs::path out = result.folder / entry.name;
        fs::create_directories(out.parent_path(), ec);
        if (ec) throw Error{L"无法创建子目录：" + out.parent_path().wstring()};
        std::ofstream file(out, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) throw Error{L"无法创建输出文件：" + out.wstring()};
        CopyData(in, file, entry.size);
        result.bytes += entry.size;
    }
    return result;
}

}  // namespace mpkg
#endif  // MPKG_CORE_H
