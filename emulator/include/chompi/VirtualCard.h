// The emulated microSD card: a folder on disk loaded into memory, so the
// firmware's file streaming (one request per 1 ms SD tick) never touches the
// disk from the audio thread. Writes land in memory and a background thread
// mirrors them to the folder once a file has been quiet for a moment.
//
// Only .wav and .json files are loaded, from the folder and its subfolders one
// level deep. Paths are FAT-style: case-insensitive, '/'-separated. Instances
// are shared per folder, like one card in one slot; machines on the same
// folder see each other's writes.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace emu
{
class VirtualCard
{
  public:
    /** Loads the card from `folder`, or returns the instance already loaded
     *  from it. Creates the folder if missing. While the last instance on the
     *  folder is still being destroyed, waits for it to write its changes. */
    static std::shared_ptr<VirtualCard> Open(const std::filesystem::path& folder);

    /** Writes any pending changes to the folder. */
    ~VirtualCard();

    // ---- FatFs-level operations (thread-safe). Handles are > 0. The
    // firmwares call them from the audio thread: lookups, reads and listing
    // don't allocate; changes can (a file's first since the last mirroring,
    // growth, creating, renaming, deleting). ----
    int      OpenFile(const char* name, bool create, bool truncate, uint32_t* size);
    uint32_t Read(int handle, uint32_t pos, void* dst, uint32_t n);
    void     Write(int handle, uint32_t pos, const void* src, uint32_t n);
    void     Resize(int handle, uint32_t size);
    bool     Exists(const char* name, uint32_t* size);
    bool     Unlink(const char* name);
    enum class RenameResult
    {
        Ok,
        NoFile,
        Exists
    };
    RenameResult Rename(const char* from, const char* to);

    struct DirEntry
    {
        char     name[256]; // without its folder
        char     key[256];  // the `after` for the entry that follows
        uint32_t size;
        bool     is_dir;
    };
    /** Of the entries directly in `path` ("" is the root), in name order, the
     *  first after key `after` ("": the first). False at the end, or if there
     *  is no such folder. */
    bool NextEntry(const char* path, const char* after, DirEntry& out);
    bool IsDir(const char* path);
    /** A folder in the root (created on disk now); true if it exists after. */
    bool MakeDir(const char* path);

  private:
    VirtualCard(std::filesystem::path folder, std::string registry_key);
    void Load();
    void Load(const std::string& dir);
    void FlusherLoop();
    void FlushKey(const std::string& key);

    struct File
    {
        std::string          name; // as on disk
        std::string          key;  // lowercase name
        std::vector<uint8_t> data;
        bool                 live = true;
    };

    File*       Find(int handle);
    bool        FolderExists(std::string_view key) const;
    std::string DiskName(std::string_view path) const;
    int   Lookup(const std::string& key) const;
    void  MarkDirty(const std::string& key);

    std::filesystem::path folder_;
    std::string           registry_key_;

    // Unlinked files keep their slot: an open FIL may still hold the handle,
    // and it must not come to address a different file.
    mutable std::mutex                           mutex_;
    std::string                                  key_; // the key a lookup builds, its capacity reused
    std::vector<std::unique_ptr<File>>           files_;
    std::unordered_map<std::string, int>         by_key_; // live files by key
    std::unordered_map<std::string, int64_t>     dirty_;  // key -> last change (ms, steady clock)
    std::unordered_map<std::string, std::string> disk_names_; // key -> name to delete on disk
    std::unordered_map<std::string, std::string> dirs_;       // folders found on disk, key -> name

    std::condition_variable wake_;
    bool                    quit_ = false;
    std::thread             flusher_;
};
} // namespace emu
