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
#include <thread>
#include <unordered_map>
#include <vector>

namespace emu
{
class VirtualCard
{
  public:
    /** Loads the card from `folder`, or returns the instance already loaded
     *  from it. Creates the folder if missing. */
    static std::shared_ptr<VirtualCard> Open(const std::filesystem::path& folder);

    /** Writes any pending changes to the folder. */
    ~VirtualCard();

    // ---- FatFs-level operations (thread-safe). Handles are > 0. ----
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
        std::string name; // without its folder
        uint32_t    size;
        bool        is_dir;
    };
    /** Entries directly in `path` ("" is the root), sorted by name. False if
     *  there is no such folder. */
    bool List(const char* path, std::vector<DirEntry>& out);
    bool IsDir(const char* path);

  private:
    explicit VirtualCard(std::filesystem::path folder);
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
    bool        FolderExists(const std::string& key) const;
    std::string DiskName(const std::string& path) const;
    int   Lookup(const std::string& key) const;
    void  MarkDirty(const std::string& key);

    std::filesystem::path folder_;

    // Unlinked files keep their slot: an open FIL may still hold the handle,
    // and it must not come to address a different file.
    mutable std::mutex                           mutex_;
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
