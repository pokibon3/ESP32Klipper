// G-code file storage (see files.hpp).
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "klippy/files.hpp"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef ESP_PLATFORM
#include <sys/statvfs.h>
#endif

namespace kl {

namespace {

class PosixFile : public GcodeFile {
public:
    PosixFile(FILE *f, size_t size) : f_(f), size_(size) {}
    ~PosixFile() override { fclose(f_); }
    size_t read(char *buf, size_t n) override { return fread(buf, 1, n, f_); }
    bool seek(size_t pos) override { return fseek(f_, (long)pos, SEEK_SET) == 0; }
    size_t size() const override { return size_; }

private:
    FILE *f_;
    size_t size_;
};

class MemoryFile : public GcodeFile {
public:
    MemoryFile(const char *data, size_t len) : data_(data), len_(len) {}
    size_t read(char *buf, size_t n) override {
        size_t k = std::min(n, len_ - pos_);
        memcpy(buf, data_ + pos_, k);
        pos_ += k;
        return k;
    }
    bool seek(size_t pos) override {
        if (pos > len_)
            return false;
        pos_ = pos;
        return true;
    }
    size_t size() const override { return len_; }

private:
    const char *data_;
    size_t len_, pos_ = 0;
};

void
walk(const std::string &root, const std::string &rel,
     std::vector<std::pair<std::string, size_t>> *out, int depth)
{
    if (depth > 8)
        return;
    std::string dir = rel.empty() ? root : root + "/" + rel;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        std::string r = rel.empty() ? e->d_name : rel + "/" + e->d_name;
        struct stat st;
        if (::stat((root + "/" + r).c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            walk(root, r, out, depth + 1);
        else
            out->emplace_back(r, (size_t)st.st_size);
    }
    closedir(d);
}

void
walk_info(const std::string &root, const std::string &rel, bool recursive,
          std::vector<FileInfo> *out, int depth)
{
    if (depth > 8)
        return;
    std::string dir = rel.empty() ? root : root + "/" + rel;
    DIR *d = opendir(dir.c_str());
    if (!d)
        return;
    while (struct dirent *e = readdir(d)) {
        if (e->d_name[0] == '.')
            continue;
        std::string r = rel.empty() ? e->d_name : rel + "/" + e->d_name;
        struct stat st;
        if (::stat((root + "/" + r).c_str(), &st) != 0)
            continue;
        FileInfo fi;
        fi.name = r;
        fi.size = (size_t)st.st_size;
        fi.modified = (double)st.st_mtime;
        fi.is_dir = S_ISDIR(st.st_mode);
        if (fi.is_dir && recursive)
            walk_info(root, r, true, out, depth + 1);
        else
            out->push_back(fi);
    }
    closedir(d);
}

class PosixWriter : public FileWriter {
public:
    PosixWriter(FILE *f, std::string tmp, std::string path)
        : f_(f), tmp_(std::move(tmp)), path_(std::move(path)) {}
    ~PosixWriter() override {
        if (f_) {
            fclose(f_);
            ::remove(tmp_.c_str());
        }
    }
    bool write(const char *data, size_t n) override {
        return f_ && fwrite(data, 1, n, f_) == n;
    }
    bool close() override {
        if (!f_)
            return false;
        bool ok = fclose(f_) == 0;
        f_ = nullptr;
        // Replace the old file only once the new one is complete
        if (ok)
            ok = ::rename(tmp_.c_str(), path_.c_str()) == 0;
        if (!ok)
            ::remove(tmp_.c_str());
        return ok;
    }

private:
    FILE *f_;
    std::string tmp_, path_;
};

bool
remove_tree(const std::string &path)
{
    DIR *d = opendir(path.c_str());
    if (d) {
        while (struct dirent *e = readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..")
                continue;
            std::string p = path + "/" + n;
            struct stat st;
            if (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                remove_tree(p);
            else
                ::remove(p.c_str());
        }
        closedir(d);
    }
    return ::rmdir(path.c_str()) == 0;
}

} // namespace

bool
safe_relative_path(const std::string &name)
{
    if (name.empty() || name[0] == '/')
        return false;
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t e = name.find('/', pos);
        if (e == std::string::npos)
            e = name.size();
        std::string part = name.substr(pos, e - pos);
        if (part == ".." || part == ".")
            return false;
        pos = e + 1;
    }
    return true;
}

/****************************************************************
 * FileSystem defaults (read-only)
 ****************************************************************/

std::vector<FileInfo>
FileSystem::list_info(const std::string &dir, bool)
{
    std::vector<FileInfo> out;
    for (const auto &f : list()) {
        if (!dir.empty() && f.first.compare(0, dir.size() + 1, dir + "/") != 0)
            continue;
        FileInfo fi;
        fi.name = f.first;
        fi.size = f.second;
        out.push_back(fi);
    }
    return out;
}

bool
FileSystem::stat(const std::string &name, FileInfo *out)
{
    for (const auto &f : list())
        if (f.first == name) {
            out->name = name;
            out->size = f.second;
            return true;
        }
    return false;
}

std::unique_ptr<FileWriter>
FileSystem::create(const std::string &, std::string *err)
{
    *err = "Storage is read-only";
    return nullptr;
}

bool
FileSystem::remove(const std::string &, std::string *err)
{
    *err = "Storage is read-only";
    return false;
}

bool
FileSystem::rename(const std::string &, const std::string &, std::string *err)
{
    *err = "Storage is read-only";
    return false;
}

bool
FileSystem::make_dir(const std::string &, std::string *err)
{
    *err = "Storage is read-only";
    return false;
}

bool
FileSystem::remove_dir(const std::string &, bool, std::string *err)
{
    *err = "Storage is read-only";
    return false;
}

bool
FileSystem::disk_usage(uint64_t *, uint64_t *)
{
    return false;
}

/****************************************************************
 * PosixFileSystem
 ****************************************************************/

std::vector<FileInfo>
PosixFileSystem::list_info(const std::string &dir, bool recursive)
{
    std::vector<FileInfo> out;
    walk_info(root_, dir, recursive, &out, 0);
    std::sort(out.begin(), out.end(),
              [](const FileInfo &a, const FileInfo &b) { return a.name < b.name; });
    return out;
}

bool
PosixFileSystem::stat(const std::string &name, FileInfo *out)
{
    struct stat st;
    if (::stat((root_ + "/" + name).c_str(), &st) != 0)
        return false;
    out->name = name;
    out->size = (size_t)st.st_size;
    out->modified = (double)st.st_mtime;
    out->is_dir = S_ISDIR(st.st_mode);
    return true;
}

std::unique_ptr<FileWriter>
PosixFileSystem::create(const std::string &name, std::string *err)
{
    std::string path = root_ + "/" + name;
    // Parent directories
    for (size_t p = name.find('/'); p != std::string::npos;
         p = name.find('/', p + 1))
        mkdir((root_ + "/" + name.substr(0, p)).c_str(), 0775);
    std::string tmp = path + ".part";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) {
        *err = "Unable to create " + name;
        return nullptr;
    }
    return std::unique_ptr<FileWriter>(new PosixWriter(f, tmp, path));
}

bool
PosixFileSystem::remove(const std::string &name, std::string *err)
{
    if (::remove((root_ + "/" + name).c_str()) != 0) {
        *err = "Unable to delete " + name;
        return false;
    }
    return true;
}

bool
PosixFileSystem::rename(const std::string &from, const std::string &to,
                        std::string *err)
{
    for (size_t p = to.find('/'); p != std::string::npos; p = to.find('/', p + 1))
        mkdir((root_ + "/" + to.substr(0, p)).c_str(), 0775);
    if (::rename((root_ + "/" + from).c_str(), (root_ + "/" + to).c_str())
        != 0) {
        *err = "Unable to move " + from + " to " + to;
        return false;
    }
    return true;
}

bool
PosixFileSystem::make_dir(const std::string &name, std::string *err)
{
    if (mkdir((root_ + "/" + name).c_str(), 0775) != 0) {
        *err = "Unable to create directory " + name;
        return false;
    }
    return true;
}

bool
PosixFileSystem::remove_dir(const std::string &name, bool force,
                            std::string *err)
{
    std::string path = root_ + "/" + name;
    bool ok = force ? remove_tree(path) : ::rmdir(path.c_str()) == 0;
    if (!ok)
        *err = "Unable to delete directory " + name
               + (force ? "" : " (not empty?)");
    return ok;
}

bool
PosixFileSystem::disk_usage(uint64_t *total, uint64_t *free_bytes)
{
#ifndef ESP_PLATFORM
    struct statvfs vs;
    if (statvfs(root_.c_str(), &vs) != 0)
        return false;
    *total = (uint64_t)vs.f_blocks * vs.f_frsize;
    *free_bytes = (uint64_t)vs.f_bavail * vs.f_frsize;
    return true;
#else
    (void)total;
    (void)free_bytes;
    return false;
#endif
}

std::vector<std::pair<std::string, size_t>>
PosixFileSystem::list()
{
    std::vector<std::pair<std::string, size_t>> out;
    walk(root_, "", &out, 0);
    std::sort(out.begin(), out.end());
    return out;
}

std::unique_ptr<GcodeFile>
PosixFileSystem::open(const std::string &name)
{
    std::string path = root_ + "/" + name;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return nullptr;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    return std::unique_ptr<GcodeFile>(new PosixFile(f, size < 0 ? 0 : size));
}

std::vector<std::pair<std::string, size_t>>
MemoryFileSystem::list()
{
    std::vector<std::pair<std::string, size_t>> out;
    for (const auto &e : files_)
        out.emplace_back(e.name, e.len);
    return out;
}

std::unique_ptr<GcodeFile>
MemoryFileSystem::open(const std::string &name)
{
    for (const auto &e : files_)
        if (e.name == name)
            return std::unique_ptr<GcodeFile>(new MemoryFile(e.data, e.len));
    return nullptr;
}

} // namespace kl
