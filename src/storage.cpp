#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "storage.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

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

// Drop page-aligned file-cache for [off, off+n). Partial edge pages are
// left in place so a later window can still share them.
void evict_file_range(int fd, uint64_t off, size_t n) {
    if (fd < 0 || n == 0) return;
    constexpr uint64_t page = 4096;
    uint64_t start = (off + page - 1) & ~(page - 1);
    uint64_t end = (off + n) & ~(page - 1);
    if (end <= start) return;
    ::posix_fadvise(fd, static_cast<off_t>(start),
                    static_cast<off_t>(end - start), POSIX_FADV_DONTNEED);
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
    // Data is now in rbuf_; evict the file pages so official memory.peak
    // does not hold a WAL window in page cache on top of the heap copy.
    // A packed-snapshot rebuild can then evict WAL as it is scanned.
    evict_file_range(fd_, off, want);
    constexpr uint64_t page = 4096;
    uint64_t evicted_to = (off + want) & ~(page - 1);
    if (evicted_to > drop_pos_) drop_pos_ = evicted_to;
    return true;
}

void Log::flush_buf() {
    if (!wbuf_.empty()) {
        invalidate_read_buf();
        write_full(fd_, wbuf_.data(), wbuf_.size());
        unsynced_ += wbuf_.size();
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
    if (wbuf_.size() >= 1024 * 1024) {
        flush_buf();
        maybe_sync_evict();
    }
    return off;
}

void Log::sync() {
    if (fd_ >= 0) {
        flush_buf();
        if (::fdatasync(fd_) != 0) throw DbError("internal_error", "fsync failed");
        // Official M includes page cache. After a durable COMMIT, drop the
        // whole file (not just the page-aligned prefix) so write-phase
        // cache cannot accumulate the full WAL across many autocommits.
        drop_cache();
        unsynced_ = 0;
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

void Log::advise_noreadahead() {
    if (fd_ >= 0) {
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_RANDOM);
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_NOREUSE);
    }
}

void Log::maybe_sync_evict() {
    // Extra durability + eviction for a long uncommitted (or slowly
    // committed) write burst. COMMIT/autocommit still calls sync().
    if (fd_ < 0 || unsynced_ < kSyncEvictEvery) return;
    flush_buf();
    if (::fdatasync(fd_) != 0) throw DbError("internal_error", "fsync failed");
    ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
    drop_pos_ = size_;
    unsynced_ = 0;
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
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        rbuf_len_ = 0;
        rbuf_off_ = 0;
        rbuf_.clear();
        ::close(fd_);
        fd_ = -1;
    }
}

PackedScan::~PackedScan() { close(); }

void PackedScan::close() {
    if (fd_ >= 0) {
        if (writing_) {
            try { flush_wbuf(); } catch (...) {}
        }
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        rbuf_len_ = 0;
        rbuf_off_ = 0;
        rbuf_.clear();
        wbuf_.clear();
        ::close(fd_);
        fd_ = -1;
    }
    ready_ = false;
    writing_ = false;
}

void PackedScan::write_header(bool complete) {
    if (fd_ < 0) return;
    char hdr[kHeaderSize];
    std::memset(hdr, 0, sizeof(hdr));
    uint32_t magic = kMagic, ver = kVersion, complete_u = complete ? 1u : 0u;
    uint32_t dim_u = static_cast<uint32_t>(dim_);
    std::memcpy(hdr + 0, &magic, 4);
    std::memcpy(hdr + 4, &ver, 4);
    std::memcpy(hdr + 8, &table_id_, 4);
    std::memcpy(hdr + 12, &dim_u, 4);
    std::memcpy(hdr + 16, &n_, 8);
    std::memcpy(hdr + 24, &log_size_, 8);
    std::memcpy(hdr + 32, &committed_max_, 8);
    std::memcpy(hdr + 40, &complete_u, 4);
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(kHeaderSize)) {
        ssize_t w = ::pwrite(fd_, hdr + off, kHeaderSize - static_cast<size_t>(off), off);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw DbError("internal_error", "packed scan header write failed");
        }
        off += w;
    }
}

void PackedScan::invalidate_read_buf() {
    if (rbuf_len_ > 0) {
        drop_upto(rbuf_off_ + rbuf_len_);
        rbuf_len_ = 0;
        rbuf_off_ = 0;
    }
}

void PackedScan::drop_upto(uint64_t off) {
    if (fd_ < 0 || off <= drop_pos_) return;
    constexpr uint64_t page = 4096;
    uint64_t start = (drop_pos_ + page - 1) & ~(page - 1);
    uint64_t end = off & ~(page - 1);
    if (end <= start) return;
    ::posix_fadvise(fd_, static_cast<off_t>(start),
                    static_cast<off_t>(end - start), POSIX_FADV_DONTNEED);
    drop_pos_ = end;
}

void PackedScan::drop_cache() {
    flush_wbuf();
    if (fd_ >= 0) {
        ::posix_fadvise(fd_, 0, 0, POSIX_FADV_DONTNEED);
        drop_pos_ = size_;
    }
}

bool PackedScan::ensure_in_buf(uint64_t off, size_t n) {
    if (n == 0) return true;
    if (off >= rbuf_off_ && off + n <= rbuf_off_ + rbuf_len_) return true;
    if (off + n > size_) return false;
    // Leave a small tail so a 4-wide kernel can still hold pointers into
    // the previous few rows when the window advances.
    constexpr uint64_t kHold = 64ull * 1024ull;
    if (rbuf_len_ > 0) {
        if (off >= rbuf_off_ + kHold) drop_upto(off > kHold ? off - kHold : 0);
        else if (off < rbuf_off_) drop_upto(rbuf_off_ + rbuf_len_);
    }
    uint64_t avail = size_ - off;
    size_t want = static_cast<size_t>(std::min<uint64_t>(
        std::max<uint64_t>(kReadWindow, n), avail));
    if (want < n) return false;
    rbuf_.resize(want);
    read_full(fd_, off, rbuf_.data(), want);
    rbuf_off_ = off;
    rbuf_len_ = want;
    evict_file_range(fd_, off, want);
    constexpr uint64_t page = 4096;
    uint64_t evicted_to = (off + want) & ~(page - 1);
    if (evicted_to > drop_pos_) drop_pos_ = evicted_to;
    return true;
}

void PackedScan::evict_written() {
    // After each 8MiB flush, fdatasync the snapshot and DONTNEED the
    // durable prefix. The build only writes (it does not read back), so
    // there is no need to keep a lagged cached window. fdatasync is used
    // instead of sync_file_range because the latter can report success
    // without actually cleaning pages on some container filesystems,
    // after which DONTNEED would leave the snapshot in official M.
    if (fd_ < 0 || size_ <= kHeaderSize) return;
    constexpr uint64_t page = 4096;
    uint64_t evict_to = size_ & ~(page - 1);
    if (evict_to <= evict_pos_) return;
    if (::fdatasync(fd_) != 0) throw DbError("internal_error", "packed scan fsync failed");
    ::posix_fadvise(fd_, 0, static_cast<off_t>(evict_to), POSIX_FADV_DONTNEED);
    evict_pos_ = evict_to;
    if (evict_to > drop_pos_) drop_pos_ = evict_to;
}

void PackedScan::flush_wbuf() {
    if (wbuf_.empty() || fd_ < 0) return;
    invalidate_read_buf();
    write_full(fd_, wbuf_.data(), wbuf_.size());
    size_ += wbuf_.size();
    wbuf_.clear();
    evict_written();
}

bool PackedScan::try_open_existing(const std::string& path, uint32_t table_id, int dim,
                                   uint64_t log_size, uint64_t committed_max) {
    close();
    path_ = path;
    int fd = ::open(path.c_str(), O_RDWR);
    if (fd < 0) return false;
    fd_ = fd;
    char hdr[kHeaderSize];
    try {
        read_full(fd_, 0, hdr, kHeaderSize);
    } catch (...) {
        close();
        return false;
    }
    uint32_t magic = 0, ver = 0, tid = 0, dim_u = 0, complete = 0;
    uint64_t n = 0, lsz = 0, cmax = 0;
    std::memcpy(&magic, hdr + 0, 4);
    std::memcpy(&ver, hdr + 4, 4);
    std::memcpy(&tid, hdr + 8, 4);
    std::memcpy(&dim_u, hdr + 12, 4);
    std::memcpy(&n, hdr + 16, 8);
    std::memcpy(&lsz, hdr + 24, 8);
    std::memcpy(&cmax, hdr + 32, 8);
    std::memcpy(&complete, hdr + 40, 4);
    off_t end = ::lseek(fd_, 0, SEEK_END);
    if (end < static_cast<off_t>(kHeaderSize) ||
        magic != kMagic || ver != kVersion || complete != 1 ||
        tid != table_id || static_cast<int>(dim_u) != dim ||
        lsz != log_size || cmax != committed_max) {
        close();
        return false;
    }
    table_id_ = tid;
    dim_ = dim;
    n_ = n;
    log_size_ = lsz;
    committed_max_ = cmax;
    size_ = static_cast<uint64_t>(end);
    drop_pos_ = 0;
    evict_pos_ = 0;
    ready_ = true;
    writing_ = false;
    return true;
}

void PackedScan::start_build(const std::string& path, uint32_t table_id, int dim,
                             uint64_t log_size, uint64_t committed_max) {
    close();
    path_ = path;
    ::unlink(path.c_str());
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) throw DbError("internal_error", "cannot open packed scan file");
    table_id_ = table_id;
    dim_ = dim;
    n_ = 0;
    log_size_ = log_size;
    committed_max_ = committed_max;
    size_ = kHeaderSize;
    drop_pos_ = 0;
    evict_pos_ = 0;
    ready_ = false;
    writing_ = true;
    wbuf_.clear();
    wbuf_.reserve(kWriteWindow);
    write_header(false);
    if (::lseek(fd_, static_cast<off_t>(kHeaderSize), SEEK_SET) < 0)
        throw DbError("internal_error", "cannot seek packed scan file");
}

void PackedScan::append_row(int64_t pk, const std::vector<ValType>& mid_types,
                            const int64_t* mid_i, const std::string_view* mid_s,
                            const float* vec) {
    if (!writing_ || fd_ < 0) throw DbError("internal_error", "packed scan not writable");
    size_t mid_bytes = 0;
    size_t si = 0;
    for (ValType t : mid_types) {
        if (t == ValType::BIGINT) mid_bytes += 8;
        else {
            mid_bytes += 4 + mid_s[si].size();
            ++si;
        }
    }
    uint32_t body = static_cast<uint32_t>(8 + mid_bytes + (dim_ > 0 ? static_cast<size_t>(dim_) * 4 : 0));
    wbuf_.reserve(wbuf_.size() + 4 + body);
    wbuf_.append(reinterpret_cast<const char*>(&body), 4);
    wbuf_.append(reinterpret_cast<const char*>(&pk), 8);
    size_t ii = 0, ss = 0;
    for (ValType t : mid_types) {
        if (t == ValType::BIGINT) {
            wbuf_.append(reinterpret_cast<const char*>(&mid_i[ii++]), 8);
        } else {
            uint32_t n = static_cast<uint32_t>(mid_s[ss].size());
            wbuf_.append(reinterpret_cast<const char*>(&n), 4);
            wbuf_.append(mid_s[ss].data(), mid_s[ss].size());
            ++ss;
        }
    }
    if (dim_ > 0) {
        if (!vec) throw DbError("internal_error", "packed scan missing vector");
        wbuf_.append(reinterpret_cast<const char*>(vec), static_cast<size_t>(dim_) * 4);
    }
    ++n_;
    if (wbuf_.size() >= kWriteWindow) flush_wbuf();
}

void PackedScan::set_reuse_key(uint64_t log_size, uint64_t committed_max) {
    log_size_ = log_size;
    committed_max_ = committed_max;
}

void PackedScan::abort_build() {
    std::string p = path_;
    writing_ = false;
    ready_ = false;
    close();
    if (!p.empty()) ::unlink(p.c_str());
}

void PackedScan::finish_build() {
    if (!writing_) return;
    flush_wbuf();
    write_header(true);
    if (::fdatasync(fd_) != 0) throw DbError("internal_error", "packed scan fsync failed");
    evict_pos_ = 0;
    drop_pos_ = 0;
    drop_cache();
    wbuf_.shrink_to_fit();
    writing_ = false;
    ready_ = true;
}

PackedScan::Cursor PackedScan::cursor(const std::vector<ValType>& mid_types, int dim) {
    flush_wbuf();
    if (fd_ >= 0) ::posix_fadvise(fd_, 0, 0, POSIX_FADV_SEQUENTIAL);
    Cursor c;
    c.s_ = this;
    c.mid_types_ = mid_types;
    c.dim_ = dim;
    c.off_ = kHeaderSize;
    c.end_ = size_;
    return c;
}

bool PackedScan::Cursor::next() {
    if (!s_ || off_ >= end_) return false;
    if (end_ - off_ < 4) return false;
    if (!s_->ensure_in_buf(off_, 4)) return false;
    uint32_t body = 0;
    std::memcpy(&body, s_->ptr_at(off_), 4);
    uint64_t rec = 4ull + body;
    if (body < 8 || off_ + rec > end_) return false;
    if (!s_->ensure_in_buf(off_, static_cast<size_t>(rec))) return false;
    const char* p = s_->ptr_at(off_);
    std::memcpy(&row_.pk, p + 4, 8);
    const char* mid = p + 12;
    size_t mid_need = 0;
    for (ValType t : mid_types_) {
        if (t == ValType::BIGINT) mid_need += 8;
        else {
            if (12 + mid_need + 4 > rec) return false;
            uint32_t n = 0;
            std::memcpy(&n, mid + mid_need, 4);
            mid_need += 4 + n;
        }
    }
    size_t vec_bytes = dim_ > 0 ? static_cast<size_t>(dim_) * 4 : 0;
    if (12 + mid_need + vec_bytes > rec) return false;
    row_.mid = mid;
    row_.mid_len = mid_need;
    row_.vec = vec_bytes ? reinterpret_cast<const float*>(mid + mid_need) : nullptr;
    off_ += rec;
    return true;
}

}  // namespace mvdb
