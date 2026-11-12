#include "storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>

namespace mvdb {

namespace {
constexpr uint32_t kFileMagic = 0x4244564D;  // MVDB
constexpr uint32_t kVersion = 1;
constexpr uint32_t kEndMagic = 0x4E45564D;
constexpr size_t kHeaderSize = 8;
constexpr uint32_t kMaxRecord = 64u * 1024u * 1024u;

void write_full(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw DbError("internal_error", "log write failed");
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

void read_full(int fd, uint64_t off, char* p, size_t n) {
    while (n > 0) {
        ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
        if (r < 0) {
            if (errno == EINTR) continue;
            throw DbError("internal_error", "log read failed");
        }
        if (r == 0) throw DbError("internal_error", "unexpected EOF in log");
        p += r;
        off += static_cast<uint64_t>(r);
        n -= static_cast<size_t>(r);
    }
}
}  // namespace

void sync_dir(const std::string& dir) {
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        ::fsync(fd);
        ::close(fd);
    }
}

Log::~Log() { close(); }

void Log::open(const std::string& dir) {
    path_ = dir + "/mvdb.log";
    bool fresh = ::access(path_.c_str(), F_OK) != 0;
    fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0) throw DbError("internal_error", "cannot open log file");
    if (fresh) {
        char header[kHeaderSize];
        std::memcpy(header, &kFileMagic, 4);
        std::memcpy(header + 4, &kVersion, 4);
        write_full(fd_, header, kHeaderSize);
        ::fdatasync(fd_);
        sync_dir(dir);
        size_ = kHeaderSize;
    } else {
        char header[kHeaderSize];
        read_full(fd_, 0, header, kHeaderSize);
        uint32_t magic = 0, ver = 0;
        std::memcpy(&magic, header, 4);
        std::memcpy(&ver, header + 4, 4);
        if (magic != kFileMagic || ver != kVersion)
            throw DbError("internal_error", "bad log header");
        off_t end = ::lseek(fd_, 0, SEEK_END);
        size_ = static_cast<uint64_t>(end);
    }
}

uint64_t Log::append(uint8_t type, uint64_t txn, uint32_t stmt, const std::string& payload) {
    uint32_t body_len = static_cast<uint32_t>(13 + payload.size());
    std::string frame;
    frame.reserve(4 + body_len + 4);
    frame.append(reinterpret_cast<const char*>(&body_len), 4);
    frame.append(reinterpret_cast<const char*>(&txn), 8);
    frame.append(reinterpret_cast<const char*>(&stmt), 4);
    frame.push_back(static_cast<char>(type));
    frame += payload;
    frame.append(reinterpret_cast<const char*>(&kEndMagic), 4);
    uint64_t off = size_;
    write_full(fd_, frame.data(), frame.size());
    size_ += frame.size();
    return off;
}

void Log::sync() {
    if (fd_ >= 0 && ::fdatasync(fd_) != 0)
        throw DbError("internal_error", "fsync failed");
}

void Log::drop_cache() {
    if (fd_ >= 0) ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
}

void Log::scan(const std::function<void(const RecordView&)>& cb) {
    uint64_t off = kHeaderSize;
    while (off < size_) {
        if (size_ - off < 4) break;
        uint32_t body_len = 0;
        read_full(fd_, off, reinterpret_cast<char*>(&body_len), 4);
        if (body_len < 13 || body_len > kMaxRecord || off + 4 + body_len + 4 > size_) break;
        std::string body(body_len, '\0');
        read_full(fd_, off + 4, body.data(), body_len);
        uint32_t end_magic = 0;
        read_full(fd_, off + 4 + body_len, reinterpret_cast<char*>(&end_magic), 4);
        if (end_magic != kEndMagic) break;
        RecordView rv;
        rv.offset = off;
        std::memcpy(&rv.txn, body.data(), 8);
        std::memcpy(&rv.stmt, body.data() + 8, 4);
        rv.type = static_cast<uint8_t>(body[12]);
        rv.payload = body.substr(13);
        cb(rv);
        off += 4 + body_len + 4;
    }
    if (off < size_) {
        if (::ftruncate(fd_, static_cast<off_t>(off)) != 0)
            throw DbError("internal_error", "cannot truncate torn log tail");
        ::fdatasync(fd_);
        size_ = off;
    }
}

RecordView Log::read_at(uint64_t offset) {
    uint32_t body_len = 0;
    read_full(fd_, offset, reinterpret_cast<char*>(&body_len), 4);
    if (body_len < 13 || body_len > kMaxRecord)
        throw DbError("internal_error", "bad record length");
    std::string body(body_len, '\0');
    read_full(fd_, offset + 4, body.data(), body_len);
    uint32_t end_magic = 0;
    read_full(fd_, offset + 4 + body_len, reinterpret_cast<char*>(&end_magic), 4);
    if (end_magic != kEndMagic) throw DbError("internal_error", "bad record magic");
    RecordView rv;
    rv.offset = offset;
    std::memcpy(&rv.txn, body.data(), 8);
    std::memcpy(&rv.stmt, body.data() + 8, 4);
    rv.type = static_cast<uint8_t>(body[12]);
    rv.payload = body.substr(13);
    return rv;
}

void Log::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace mvdb