#include "storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
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
        // Start drop_pos_ at 0 so recovery/scan can incrementally DONTNEED
        // already-read prefixes. Setting this to size_ disabled drop_upto
        // during restart of a large log and let page cache (counted in
        // official memory.peak) balloon to the full file size.
        drop_pos_ = 0;
    }
}

void Log::invalidate_read_buf() {
    if (rbuf_len_ > 0) {
        drop_upto(rbuf_off_ + rbuf_len_);
        rbuf_len_ = 0;
        rbuf_off_ = 0;
    }
}

bool Log::ensure_in_buf(uint64_t off, size_t n) {
    if (n == 0) return true;
    if (off >= rbuf_off_ && off + n <= rbuf_off_ + rbuf_len_) return true;
    if (off + n > size_) return false;

    // Drop the previous window (or its consumed prefix) from page cache.
    if (rbuf_len_ > 0) {
        if (off >= rbuf_off_) drop_upto(off);
        else drop_upto(rbuf_off_ + rbuf_len_);
    }

    uint64_t avail = size_ - off;
    size_t want = static_cast<size_t>(std::min<uint64_t>(
        std::max<uint64_t>(kReadWindow, n), avail));
    if (want < n) return false;
    rbuf_.resize(want);
    read_full(fd_, off, rbuf_.data(), want);
    rbuf_off_ = off;
    rbuf_len_ = want;
    // Re-reading a previously dropped range brings pages back into the
    // cache; rewind drop_pos_ so later drop_upto() can release them again
    // (important for the 2nd+ over-memory query after drop_cache()).
    constexpr uint64_t page = 4096;
    uint64_t aligned = off & ~(page - 1);
    if (aligned < drop_pos_) drop_pos_ = aligned;
    return true;
}

void Log::flush_buf() {
    if (!wbuf_.empty()) {
        invalidate_read_buf();
        write_full(fd_, wbuf_.data(), wbuf_.size());
        wbuf_.clear();
    }
}

uint64_t Log::append(uint8_t type, uint64_t txn, uint32_t stmt, const std::string& payload) {
    uint32_t body_len = static_cast<uint32_t>(13 + payload.size());
    uint64_t off = size_;
    wbuf_.reserve(wbuf_.size() + 4 + body_len + 4);
    wbuf_.append(reinterpret_cast<const char*>(&body_len), 4);
    wbuf_.append(reinterpret_cast<const char*>(&txn), 8);
    wbuf_.append(reinterpret_cast<const char*>(&stmt), 4);
    wbuf_.push_back(static_cast<char>(type));
    wbuf_ += payload;
    wbuf_.append(reinterpret_cast<const char*>(&kEndMagic), 4);
    size_ += 4 + static_cast<uint64_t>(body_len) + 4;
    if (wbuf_.size() >= 1024 * 1024) flush_buf();
    return off;
}

void Log::sync() {
    if (fd_ >= 0) {
        flush_buf();
        if (::fdatasync(fd_) != 0) throw DbError("internal_error", "fsync failed");
        // Official M includes page cache; drop durable, page-aligned ranges.
        drop_upto(size_);
    }
}

void Log::drop_cache() {
    flush_buf();
    if (fd_ >= 0) {
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        drop_pos_ = size_;
    }
}

void Log::drop_upto(uint64_t off) {
    if (fd_ < 0 || off <= drop_pos_) return;
    constexpr uint64_t page = 4096;
    uint64_t start = (drop_pos_ + page - 1) & ~(page - 1);
    uint64_t end = off & ~(page - 1);
    if (end <= start) return;
    ::posix_fadvise(fd_, static_cast<off_t>(start),
                    static_cast<off_t>(end - start), POSIX_FADV_DONTNEED);
    drop_pos_ = end;
}

void Log::scan(const std::function<void(const RecordView&)>& cb) {
    flush_buf();
    if (fd_ >= 0) ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
    uint64_t off = kHeaderSize;
    while (off < size_) {
        if (size_ - off < 4) break;
        if (!ensure_in_buf(off, 4)) break;
        uint32_t body_len = 0;
        std::memcpy(&body_len, ptr_at(off), 4);
        if (body_len < 13 || body_len > kMaxRecord || off + 4 + body_len + 4 > size_) break;
        uint64_t rec_sz = 4 + static_cast<uint64_t>(body_len) + 4;
        if (!ensure_in_buf(off, static_cast<size_t>(rec_sz))) break;
        const char* rec = ptr_at(off);
        uint32_t end_magic = 0;
        std::memcpy(&end_magic, rec + 4 + body_len, 4);
        if (end_magic != kEndMagic) break;
        RecordView rv;
        rv.offset = off;
        std::memcpy(&rv.txn, rec + 4, 8);
        std::memcpy(&rv.stmt, rec + 12, 4);
        rv.type = static_cast<uint8_t>(rec[16]);
        rv.payload = rec + 17;
        rv.payload_len = body_len - 13;
        cb(rv);
        off += rec_sz;
    }
    drop_upto(off);
    if (off < size_) {
        invalidate_read_buf();
        if (::ftruncate(fd_, static_cast<off_t>(off)) != 0)
            throw DbError("internal_error", "cannot truncate torn log tail");
        ::fdatasync(fd_);
        size_ = off;
    }
}

RecordView Log::read_at(uint64_t offset) {
    flush_buf();
    if (!ensure_in_buf(offset, 4)) throw DbError("internal_error", "bad record length");
    uint32_t body_len = 0;
    std::memcpy(&body_len, ptr_at(offset), 4);
    if (body_len < 13 || body_len > kMaxRecord)
        throw DbError("internal_error", "bad record length");
    uint64_t rec_sz = 4 + static_cast<uint64_t>(body_len) + 4;
    if (!ensure_in_buf(offset, static_cast<size_t>(rec_sz)))
        throw DbError("internal_error", "log record truncated");
    const char* rec = ptr_at(offset);
    uint32_t end_magic = 0;
    std::memcpy(&end_magic, rec + 4 + body_len, 4);
    if (end_magic != kEndMagic) throw DbError("internal_error", "bad record magic");
    RecordView rv;
    rv.offset = offset;
    std::memcpy(&rv.txn, rec + 4, 8);
    std::memcpy(&rv.stmt, rec + 12, 4);
    rv.type = static_cast<uint8_t>(rec[16]);
    rv.payload = rec + 17;
    rv.payload_len = body_len - 13;
    return rv;
}

void Log::close() {
    if (fd_ >= 0) {
        flush_buf();
        rbuf_len_ = 0;
        rbuf_off_ = 0;
        rbuf_.clear();
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace mvdb