// G-code file storage for virtual_sdcard: a directory on the host or the
// ESP32's SD card (both through POSIX file I/O), or files kept in memory /
// flash (tests, built-in demo files).
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kl {

class GcodeFile {
public:
    virtual ~GcodeFile() = default;
    virtual size_t read(char *buf, size_t n) = 0;
    virtual bool seek(size_t pos) = 0;
    virtual size_t size() const = 0;
};

// A file being written (upload)
class FileWriter {
public:
    virtual ~FileWriter() = default;
    virtual bool write(const char *data, size_t n) = 0;
    // Finish the file; false = error (the partial file is removed)
    virtual bool close() = 0;
};

struct FileInfo {
    std::string name;      // relative path
    size_t size = 0;
    double modified = 0.;  // seconds since the epoch
    bool is_dir = false;
};

class FileSystem {
public:
    virtual ~FileSystem() = default;
    // (relative path, size) of every file, like virtual_sdcard
    // get_file_list(check_subdirs=True)
    virtual std::vector<std::pair<std::string, size_t>> list() = 0;
    virtual std::unique_ptr<GcodeFile> open(const std::string &name) = 0;

    // File manager (Moonraker).  The defaults describe a read-only store.
    // Every file under dir (recursive) or the entries of dir only
    virtual std::vector<FileInfo> list_info(const std::string &dir,
                                            bool recursive);
    virtual bool stat(const std::string &name, FileInfo *out);
    virtual std::unique_ptr<FileWriter> create(const std::string &name,
                                               std::string *err);
    virtual bool remove(const std::string &name, std::string *err);
    virtual bool rename(const std::string &from, const std::string &to,
                        std::string *err);
    virtual bool make_dir(const std::string &name, std::string *err);
    virtual bool remove_dir(const std::string &name, bool force,
                            std::string *err);
    virtual bool writable() const { return false; }
    // Bytes (total, free); false if unknown
    virtual bool disk_usage(uint64_t *total, uint64_t *free_bytes);
};

// Path checks for names from the API: no "..", no absolute paths
bool safe_relative_path(const std::string &name);

// A directory (host path or an ESP-IDF VFS mount such as /sdcard/gcodes)
class PosixFileSystem : public FileSystem {
public:
    explicit PosixFileSystem(std::string root) : root_(std::move(root)) {}
    std::vector<std::pair<std::string, size_t>> list() override;
    std::unique_ptr<GcodeFile> open(const std::string &name) override;
    std::vector<FileInfo> list_info(const std::string &dir,
                                    bool recursive) override;
    bool stat(const std::string &name, FileInfo *out) override;
    std::unique_ptr<FileWriter> create(const std::string &name,
                                       std::string *err) override;
    bool remove(const std::string &name, std::string *err) override;
    bool rename(const std::string &from, const std::string &to,
                std::string *err) override;
    bool make_dir(const std::string &name, std::string *err) override;
    bool remove_dir(const std::string &name, bool force,
                    std::string *err) override;
    bool writable() const override { return true; }
    bool disk_usage(uint64_t *total, uint64_t *free_bytes) override;
    const std::string &root() const { return root_; }

private:
    std::string root_;
};

// Files that live in memory (not copied: the data must stay valid)
class MemoryFileSystem : public FileSystem {
public:
    void add(const std::string &name, const char *data, size_t len) {
        files_.push_back({name, data, len});
    }
    std::vector<std::pair<std::string, size_t>> list() override;
    std::unique_ptr<GcodeFile> open(const std::string &name) override;

private:
    struct Entry {
        std::string name;
        const char *data;
        size_t len;
    };
    std::vector<Entry> files_;
};

} // namespace kl
