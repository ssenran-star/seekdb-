/* Thin portable file-I/O layer (POSIX + Win32) so the same source builds on the
 * judge (Linux) and on a developer workstation for local testing. */
#include "db.h"

#include <errno.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

int mv_os_open(const char *path, int create) {
#ifdef _WIN32
    int flags = _O_RDWR | _O_BINARY;
    if (create) flags |= _O_CREAT;
    return _open(path, flags, _S_IREAD | _S_IWRITE);
#else
    int flags = O_RDWR;
    if (create) flags |= O_CREAT;
    return open(path, flags, 0644);
#endif
}

int mv_os_close(int fd) {
#ifdef _WIN32
    return _close(fd);
#else
    return close(fd);
#endif
}

int64_t mv_os_size(int fd) {
#ifdef _WIN32
    return (int64_t)_filelengthi64(fd);
#else
    struct stat st;
    if (fstat(fd, &st) != 0) return -1;
    return (int64_t)st.st_size;
#endif
}

int64_t mv_os_read(int fd, void *buf, int64_t len, int64_t off) {
#ifdef _WIN32
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int64_t done = 0;
    while (done < len) {
        int chunk = (len - done) > (int64_t)1 << 20 ? (int64_t)1 << 20 : len - done;
        int n = _read(fd, (char *)buf + done, (unsigned)chunk);
        if (n <= 0) break;
        done += n;
    }
    return done;
#else
    int64_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, (char *)buf + done, (size_t)(len - done), (off_t)(off + done));
        if (n <= 0) break;
        done += n;
    }
    return done;
#endif
}

int64_t mv_os_write(int fd, const void *buf, int64_t len, int64_t off) {
#ifdef _WIN32
    if (_lseeki64(fd, off, SEEK_SET) < 0) return -1;
    int64_t done = 0;
    while (done < len) {
        int chunk = (len - done) > (int64_t)1 << 20 ? (int64_t)1 << 20 : len - done;
        int n = _write(fd, (const char *)buf + done, (unsigned)chunk);
        if (n <= 0) return -1;
        done += n;
    }
    return done;
#else
    int64_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, (const char *)buf + done, (size_t)(len - done), (off_t)(off + done));
        if (n <= 0) return -1;
        done += n;
    }
    return done;
#endif
}

int mv_os_fsync(int fd) {
#ifdef _WIN32
    return _commit(fd);
#else
    return fsync(fd);
#endif
}

int mv_os_truncate(int fd, int64_t len) {
#ifdef _WIN32
    return _chsize_s(fd, len);
#else
    return (int)ftruncate(fd, (off_t)len);
#endif
}

int mv_os_rename(const char *from, const char *to) {
#ifdef _WIN32
    _unlink(to);
#endif
    return rename(from, to);
}

int mv_os_unlink(const char *path) { return remove(path); }

int mv_os_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path);
#else
    return mkdir(path, 0755);
#endif
}

void mv_os_sync_dir(const char *path) {
#ifdef _WIN32
    (void)path;
#else
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
#endif
}

int mv_os_exists(const char *path) {
#ifdef _WIN32
    struct _stat64 st;
    return _stat64(path, &st) == 0;
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}
