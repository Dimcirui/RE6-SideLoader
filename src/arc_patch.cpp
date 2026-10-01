// Rebuilds an RE6 ARC (version 7) with loose files replacing / adding entries.
//
// Layout (see tools/re6arc/arc.py, from RE6-ARC-Tool): "ARC\0", u16 version 7, u16 count, count * 0x50 entries
// (char name[64], u32 type hash, u32 compressed size, u32 size | flags << 29, u32 offset), zero padding to
// 0x8000, zlib streams. Unchanged entries keep their compressed bytes; loose files are written as zlib
// streams made of stored (uncompressed) deflate blocks, which every zlib inflater accepts and costs no
// compression time.
#include "loose.h"

#include <unordered_map>

namespace loose {
namespace {

const uint32_t kMagic = 0x00435241;  // "ARC\0"
const uint16_t kVersion = 7;
const uint32_t kAlign = 0x8000;
const uint32_t kSizeMask = 0x1FFFFFFF;
const uint32_t kDefaultFlags = 2u << 29;
const DWORD kChunk = 1 << 20;

#pragma pack(push, 1)
struct Header {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
};
struct Entry {
    char name[64];
    uint32_t type;
    uint32_t csize;
    uint32_t sizeFlags;
    uint32_t offset;
};
#pragma pack(pop)
static_assert(sizeof(Entry) == 0x50, "ARC entry size");

struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    bool ok() const { return h != INVALID_HANDLE_VALUE; }
};

bool ReadAt(HANDLE h, uint64_t off, void* buf, DWORD n) {
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)off;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return false;
    auto* p = (uint8_t*)buf;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(h, p, n, &got, nullptr) || got == 0) return false;
        p += got;
        n -= got;
    }
    return true;
}

bool WriteAll(HANDLE h, const void* buf, DWORD n) {
    auto* p = (const uint8_t*)buf;
    while (n) {
        DWORD put = 0;
        if (!WriteFile(h, p, n, &put, nullptr) || put == 0) return false;
        p += put;
        n -= put;
    }
    return true;
}

std::string Lower(const char* s, size_t n) {
    std::string out(s, n);
    for (auto& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

std::string MapKey(const std::string& key, uint32_t type) {
    char buf[16];
    wsprintfA(buf, "|%08x", type);
    return key + buf;
}

bool ReadTable(HANDLE h, std::vector<Entry>* entries, uint64_t* fileSize) {
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) return false;
    *fileSize = (uint64_t)sz.QuadPart;
    Header hd;
    if (!ReadAt(h, 0, &hd, sizeof(hd)) || hd.magic != kMagic || hd.version != kVersion) return false;
    entries->resize(hd.count);
    if (hd.count && !ReadAt(h, sizeof(hd), entries->data(), (DWORD)(hd.count * sizeof(Entry)))) return false;
    for (auto& e : *entries)
        if ((uint64_t)e.offset + e.csize > *fileSize) return false;
    return true;
}

std::string EntryName(const Entry& e) {
    size_t n = 0;
    while (n < sizeof(e.name) && e.name[n]) n++;
    return std::string(e.name, n);
}

// zlib stream of stored deflate blocks
uint32_t StoredSize(uint64_t n) {
    uint64_t blocks = n ? (n + 65534) / 65535 : 1;
    return (uint32_t)(2 + blocks * 5 + n + 4);
}

bool WriteStored(HANDLE out, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> buf;
    buf.reserve(StoredSize(data.size()));
    buf.push_back(0x78);
    buf.push_back(0x01);
    size_t pos = 0;
    do {
        size_t len = data.size() - pos;
        if (len > 65535) len = 65535;
        bool last = pos + len == data.size();
        buf.push_back(last ? 1 : 0);
        buf.push_back((uint8_t)(len & 0xFF));
        buf.push_back((uint8_t)(len >> 8));
        buf.push_back((uint8_t)(~len & 0xFF));
        buf.push_back((uint8_t)((~len >> 8) & 0xFF));
        buf.insert(buf.end(), data.begin() + pos, data.begin() + pos + len);
        pos += len;
    } while (pos < data.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    uint32_t adler = (b << 16) | a;
    for (int s = 24; s >= 0; s -= 8) buf.push_back((uint8_t)(adler >> s));
    return WriteAll(out, buf.data(), (DWORD)buf.size());
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>* data) {
    Handle f;
    f.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!f.ok()) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f.h, &sz) || sz.QuadPart > kSizeMask) return false;
    data->resize((size_t)sz.QuadPart);
    return data->empty() || ReadAt(f.h, 0, data->data(), (DWORD)data->size());
}

void MakeDirs(const std::wstring& dir) {
    for (size_t i = 3; i <= dir.size(); i++)
        if (i == dir.size() || dir[i] == L'\\') CreateDirectoryW(dir.substr(0, i).c_str(), nullptr);
}

struct Plan {
    Entry e;
    int ov;  // index into overrides, -1 = copy from source
};

}  // namespace

bool ReadArcKeys(const std::wstring& src, std::vector<std::pair<std::string, uint32_t>>* keys) {
    Handle f;
    f.h = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                      nullptr);
    if (!f.ok()) return false;
    std::vector<Entry> entries;
    uint64_t size;
    if (!ReadTable(f.h, &entries, &size)) return false;
    for (auto& e : entries) {
        std::string n = EntryName(e);
        keys->emplace_back(Lower(n.data(), n.size()), e.type);
    }
    return true;
}

bool PatchArc(const std::wstring& src, const std::vector<Override>& ovs, const std::wstring& dst, int* replaced,
              int* added) {
    *replaced = *added = 0;
    Handle in;
    in.h = CreateFileW(src.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                       FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (!in.ok()) {
        Log(L"  cannot open %s (error %u)", src.c_str(), GetLastError());
        return false;
    }
    std::vector<Entry> entries;
    uint64_t srcSize;
    if (!ReadTable(in.h, &entries, &srcSize)) {
        Log(L"  %s is not an RE6 ARC (version 7) or is truncated", src.c_str());
        return false;
    }

    std::unordered_map<std::string, int> byKey;
    for (int i = 0; i < (int)ovs.size(); i++) byKey.emplace(MapKey(ovs[i].key, ovs[i].type), i);
    std::vector<bool> used(ovs.size(), false);

    std::vector<Plan> plan;
    plan.reserve(entries.size() + ovs.size());
    for (auto& e : entries) {
        std::string n = EntryName(e);
        auto it = byKey.find(MapKey(Lower(n.data(), n.size()), e.type));
        int ov = it == byKey.end() ? -1 : it->second;
        if (ov >= 0) used[ov] = true;
        plan.push_back({e, ov});
    }
    for (int i = 0; i < (int)ovs.size(); i++) {
        if (used[i] || !ovs[i].canAdd) continue;
        if (ovs[i].name.size() >= sizeof(Entry::name)) {
            Log(L"  skipped (entry name longer than 63 bytes): %s", ovs[i].file.c_str());
            continue;
        }
        Plan p{};
        memcpy(p.e.name, ovs[i].name.data(), ovs[i].name.size());
        p.e.type = ovs[i].type;
        p.e.sizeFlags = kDefaultFlags;
        p.ov = i;
        plan.push_back(p);
    }
    if (plan.size() > 0xFFFF) {
        Log(L"  too many entries (%u)", (unsigned)plan.size());
        return false;
    }

    // stored zlib sizes are known from the file sizes, so the table can be written before the data;
    // p.e.offset keeps the SOURCE offset here, output offsets are assigned while writing the table
    uint64_t tableEnd = sizeof(Header) + plan.size() * sizeof(Entry);
    uint64_t dataStart = plan.empty() ? tableEnd : (tableEnd + kAlign - 1) / kAlign * kAlign;
    uint64_t total = dataStart;
    for (auto& p : plan) {
        if (p.ov >= 0) {
            const Override& o = ovs[p.ov];
            if (o.size > kSizeMask) {
                Log(L"  file too large for an ARC entry: %s", o.file.c_str());
                return false;
            }
            p.e.csize = StoredSize(o.size);
            p.e.sizeFlags = ((uint32_t)o.size & kSizeMask) | (p.e.sizeFlags & ~kSizeMask);
        }
        total += p.e.csize;
    }
    if (total > 0xFFFFFFFFull) {
        Log(L"  patched arc would exceed 4 GB");
        return false;
    }

    MakeDirs(dst.substr(0, dst.find_last_of(L'\\')));
    std::wstring tmp = dst + L".tmp";
    {
        Handle out;
        out.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (!out.ok()) {
            Log(L"  cannot create %s (error %u)", tmp.c_str(), GetLastError());
            return false;
        }
        Header hd{kMagic, kVersion, (uint16_t)plan.size()};
        std::vector<Entry> table(plan.size());
        uint64_t off = dataStart;
        for (size_t i = 0; i < plan.size(); i++) {
            table[i] = plan[i].e;
            table[i].offset = (uint32_t)off;
            off += plan[i].e.csize;
        }
        std::vector<uint8_t> pad((size_t)(dataStart - tableEnd), 0);
        bool ok = WriteAll(out.h, &hd, sizeof(hd)) &&
                  (table.empty() || WriteAll(out.h, table.data(), (DWORD)(table.size() * sizeof(Entry)))) &&
                  (pad.empty() || WriteAll(out.h, pad.data(), (DWORD)pad.size()));
        std::vector<uint8_t> buf;
        for (size_t i = 0; ok && i < plan.size(); i++) {
            const Plan& p = plan[i];
            if (p.ov >= 0) {
                if (!ReadWhole(ovs[p.ov].file, &buf) || buf.size() != ovs[p.ov].size) {
                    Log(L"  cannot read %s (changed while patching?)", ovs[p.ov].file.c_str());
                    ok = false;
                    break;
                }
                ok = WriteStored(out.h, buf);
                ++*(i < entries.size() ? replaced : added);
            } else {
                buf.resize(kChunk);
                uint32_t left = p.e.csize;
                uint64_t at = p.e.offset;
                while (ok && left) {
                    DWORD n = left < kChunk ? left : kChunk;
                    ok = ReadAt(in.h, at, buf.data(), n) && WriteAll(out.h, buf.data(), n);
                    at += n;
                    left -= n;
                }
            }
        }
        if (!ok) {
            Log(L"  write failed (error %u)", GetLastError());
            CloseHandle(out.h);
            out.h = INVALID_HANDLE_VALUE;
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (!MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"  cannot replace %s (error %u) - is the game still reading the old copy?", dst.c_str(),
            GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

}  // namespace loose
