#include "chompi/VirtualCard.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>

namespace fs = std::filesystem;

namespace emu
{
namespace
{
/** A firmware path as the card stores it: no drive ("0:") or leading '/'. */
std::string Normalize(const std::string& path)
{
    size_t start = path.compare(0, 2, "0:") == 0 ? 2 : 0;
    while(start < path.size() && path[start] == '/')
        start++;
    std::string out = path.substr(start);
    while(!out.empty() && out.back() == '/')
        out.pop_back();
    return out;
}

std::string KeyFor(const std::string& path)
{
    // FAT names are case-insensitive.
    std::string key = Normalize(path);
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    return key;
}

/** The directory part of a key ("" for the root). */
std::string ParentOf(const std::string& key)
{
    const size_t slash = key.rfind('/');
    return slash == std::string::npos ? std::string() : key.substr(0, slash);
}

int64_t NowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool EndsWith(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

/** Files a firmware can use: samples, wavetables and JSON settings. Hidden
 *  files (macOS's "._" copies) are left alone. */
bool IsCardFile(const std::string& name, const std::string& key)
{
    return !name.empty() && name[0] != '.' && (EndsWith(key, ".wav") || EndsWith(key, ".json"));
}

// A write is mirrored once the file has been untouched this long, so chunked
// writes (presets, sample copies) reach the disk as one file write.
constexpr int64_t kQuietMs   = 300;
constexpr auto    kFlushPoll = std::chrono::milliseconds(250);
// Mirroring copies under the lock in pieces this size, bounding how long the
// audio thread can wait on it.
constexpr size_t kCopyChunk = 64 * 1024;
} // namespace

std::shared_ptr<VirtualCard> VirtualCard::Open(const fs::path& folder)
{
    static std::mutex                                       registry_mutex;
    static std::map<std::string, std::weak_ptr<VirtualCard>> registry;

    std::error_code ec;
    fs::create_directories(folder, ec);
    const fs::path    canonical = fs::weakly_canonical(folder, ec);
    const fs::path    resolved  = ec ? folder : canonical;
    const std::string key       = resolved.string();

    std::lock_guard<std::mutex> lock(registry_mutex);
    if(auto existing = registry[key].lock())
        return existing;

    std::shared_ptr<VirtualCard> card(new VirtualCard(resolved));
    registry[key] = card;
    return card;
}

VirtualCard::VirtualCard(fs::path folder) : folder_(std::move(folder))
{
    Load();
    flusher_ = std::thread([this] { FlusherLoop(); });
}

VirtualCard::~VirtualCard()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    flusher_.join();

    std::vector<std::string> keys;
    for(const auto& entry : dirty_)
        keys.push_back(entry.first);
    for(const auto& key : keys)
        FlushKey(key);
}

/** The root and its subfolders (TEMPO keeps samples in folders), one level
 *  deep. */
void VirtualCard::Load()
{
    Load("");
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(folder_, ec))
    {
        const std::string name = entry.path().filename().string();
        if(entry.is_directory(ec) && !name.empty() && name[0] != '.')
        {
            dirs_[KeyFor(name)] = name;
            Load(name);
        }
    }
}

void VirtualCard::Load(const std::string& dir)
{
    std::error_code ec;
    for(const auto& entry : fs::directory_iterator(dir.empty() ? folder_ : folder_ / dir, ec))
    {
        const std::string leaf = entry.path().filename().string();
        const std::string name = dir.empty() ? leaf : dir + "/" + leaf;
        const std::string key  = KeyFor(name);
        if(!entry.is_regular_file(ec) || !IsCardFile(leaf, key))
            continue;

        std::ifstream in(entry.path(), std::ios::binary | std::ios::ate);
        const auto    size = in ? static_cast<std::streamoff>(in.tellg()) : -1;
        if(size < 0 || size > std::numeric_limits<uint32_t>::max()) // FAT32 sizes are 32-bit
            continue;

        auto file  = std::make_unique<File>();
        file->name = name;
        file->key  = key;
        file->data.resize(static_cast<size_t>(size));
        in.seekg(0);
        if(!in.read(reinterpret_cast<char*>(file->data.data()), size))
            continue;

        by_key_[key] = static_cast<int>(files_.size());
        files_.push_back(std::move(file));
    }
}

VirtualCard::File* VirtualCard::Find(int handle)
{
    if(handle <= 0 || handle > static_cast<int>(files_.size()))
        return nullptr;
    return files_[size_t(handle - 1)].get();
}

int VirtualCard::Lookup(const std::string& key) const
{
    auto it = by_key_.find(key);
    return it == by_key_.end() ? -1 : it->second;
}

void VirtualCard::MarkDirty(const std::string& key)
{
    dirty_[key] = NowMs();
}

int VirtualCard::OpenFile(const char* name, bool create, bool truncate, uint32_t* size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string           key = KeyFor(name);
    int                         idx = Lookup(key);

    if(idx < 0)
    {
        // FatFs can't create a file in a folder that isn't there (FR_NO_PATH).
        if(!create || !FolderExists(ParentOf(key)))
            return 0;
        auto file  = std::make_unique<File>();
        file->name = DiskName(name);
        file->key  = key;
        idx        = static_cast<int>(files_.size());
        files_.push_back(std::move(file));
        by_key_[key] = idx;
        MarkDirty(key);
    }

    File& file = *files_[size_t(idx)];
    if(truncate && !file.data.empty())
    {
        file.data.clear();
        MarkDirty(key);
    }

    *size = static_cast<uint32_t>(file.data.size());
    return idx + 1;
}

uint32_t VirtualCard::Read(int handle, uint32_t pos, void* dst, uint32_t n)
{
    std::lock_guard<std::mutex> lock(mutex_);
    File*                       file = Find(handle);
    if(file == nullptr || pos >= file->data.size())
        return 0;
    const uint32_t count = std::min<uint32_t>(n, static_cast<uint32_t>(file->data.size() - pos));
    std::memcpy(dst, file->data.data() + pos, count);
    return count;
}

void VirtualCard::Write(int handle, uint32_t pos, const void* src, uint32_t n)
{
    std::lock_guard<std::mutex> lock(mutex_);
    File*                       file = Find(handle);
    if(file == nullptr || !file->live) // unlinked while open: the data goes nowhere
        return;

    if(file->data.size() < size_t(pos) + n)
        file->data.resize(size_t(pos) + n);
    std::memcpy(file->data.data() + pos, src, n);
    MarkDirty(file->key);
}

void VirtualCard::Resize(int handle, uint32_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    File*                       file = Find(handle);
    if(file == nullptr || !file->live || file->data.size() == size)
        return;
    file->data.resize(size);
    MarkDirty(file->key);
}

bool VirtualCard::Exists(const char* name, uint32_t* size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const int                   idx = Lookup(KeyFor(name));
    if(idx < 0)
        return false;
    if(size != nullptr)
        *size = static_cast<uint32_t>(files_[size_t(idx)]->data.size());
    return true;
}

bool VirtualCard::IsDir(const char* path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    return FolderExists(KeyFor(path));
}

bool VirtualCard::FolderExists(const std::string& key) const
{
    if(key.empty() || dirs_.count(key))
        return true;
    for(const auto& [file, idx] : by_key_)
        if(file.compare(0, key.size() + 1, key + "/") == 0)
            return true;
    return false;
}

/** A firmware path as written to disk: relative to the card folder (a leading
 *  '/' would otherwise make it absolute) and with its folder spelled as on
 *  disk, so "/Chromatic/x.wav" lands in an existing "chromatic" folder on a
 *  case-sensitive volume too. */
std::string VirtualCard::DiskName(const std::string& path) const
{
    const std::string name   = Normalize(path);
    const size_t      slash  = name.rfind('/');
    const auto        folder = slash == std::string::npos ? dirs_.end() : dirs_.find(KeyFor(name.substr(0, slash)));
    return folder == dirs_.end() ? name : folder->second + name.substr(slash);
}

bool VirtualCard::List(const char* path, std::vector<DirEntry>& out)
{
    out.clear();
    if(!IsDir(path))
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string           dir = KeyFor(path);
    std::map<std::string, DirEntry> entries; // by key, so sorted
    auto add_dir = [&](const std::string& key, const std::string& name) {
        if(ParentOf(key) == dir)
            entries.emplace(key, DirEntry{name.substr(name.rfind('/') + 1), 0, true});
    };
    for(const auto& [key, name] : dirs_)
        add_dir(key, name);
    for(const auto& [key, idx] : by_key_)
    {
        const File& file = *files_[size_t(idx)];
        if(ParentOf(key) == dir)
            entries[key] = DirEntry{file.name.substr(file.name.rfind('/') + 1), uint32_t(file.data.size()), false};
        else if(ParentOf(ParentOf(key)) == dir && !ParentOf(key).empty()) // folder created by a write
            add_dir(ParentOf(key), ParentOf(file.name));
    }
    for(auto& [key, entry] : entries)
        out.push_back(std::move(entry));
    return true;
}

bool VirtualCard::Unlink(const char* name)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string           key = KeyFor(name);
    const int                   idx = Lookup(key);
    if(idx < 0)
        return false;

    File& file = *files_[size_t(idx)];
    file.live  = false;
    std::vector<uint8_t>().swap(file.data);
    by_key_.erase(key);
    disk_names_[key] = file.name;
    MarkDirty(key);
    return true;
}

VirtualCard::RenameResult VirtualCard::Rename(const char* from, const char* to)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string           from_key = KeyFor(from);
    const std::string           to_key   = KeyFor(to);
    const int                   idx      = Lookup(from_key);
    if(idx < 0)
        return RenameResult::NoFile;
    if(from_key != to_key && Lookup(to_key) >= 0)
        return RenameResult::Exists;
    if(!FolderExists(ParentOf(to_key)))
        return RenameResult::NoFile;

    File& file             = *files_[size_t(idx)];
    disk_names_[from_key]  = file.name;
    by_key_.erase(from_key);
    file.name       = DiskName(to);
    file.key        = to_key;
    by_key_[to_key] = idx;
    MarkDirty(from_key);
    MarkDirty(to_key);
    return RenameResult::Ok;
}

void VirtualCard::FlusherLoop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while(!quit_)
    {
        wake_.wait_for(lock, kFlushPoll);
        if(quit_)
            break;

        std::vector<std::string> quiet;
        const int64_t            now = NowMs();
        for(const auto& [key, changed] : dirty_)
            if(now - changed >= kQuietMs)
                quiet.push_back(key);

        if(quiet.empty())
            continue;
        lock.unlock();
        for(const auto& key : quiet)
            FlushKey(key);
        lock.lock();
    }
}

/** Mirrors one file to disk. A change that lands mid-copy marks the key dirty
 *  again, so a torn copy is rewritten once the file goes quiet. */
void VirtualCard::FlushKey(const std::string& key)
{
    std::string name;
    size_t      size   = 0;
    bool        remove = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(dirty_.erase(key) == 0)
            return;
        const int idx = Lookup(key);
        remove        = idx < 0;
        name          = remove ? disk_names_[key] : files_[size_t(idx)]->name;
        size          = remove ? 0 : files_[size_t(idx)]->data.size();
    }

    std::error_code ec;
    if(remove)
    {
        fs::remove(folder_ / name, ec);
        return;
    }

    std::vector<uint8_t> data(size);
    for(size_t pos = 0; pos < size; pos += kCopyChunk)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const int                   idx = Lookup(key);
        if(idx < 0 || files_[size_t(idx)]->data.size() < size) // unlinked, renamed or shrunk meanwhile
            return;
        std::memcpy(data.data() + pos, files_[size_t(idx)]->data.data() + pos, std::min(kCopyChunk, size - pos));
    }

    const fs::path target = folder_ / name;
    fs::create_directories(target.parent_path(), ec);
    const fs::path temp = target.parent_path() / ("." + target.filename().string() + ".chimpo-tmp");
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if(!out)
        {
            out.close();
            fs::remove(temp, ec);
            return;
        }
    }
    fs::rename(temp, target, ec);
    if(ec)
        fs::remove(temp, ec);
}

} // namespace emu
