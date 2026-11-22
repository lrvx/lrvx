#pragma once

// The handful of file operations a write-ahead log needs, on the platforms
// lrvx runs on. Not a filesystem abstraction: `std::filesystem` covers
// naming, renaming and removal portably already. What it does not cover is
// durability, and durability is the reason a journal exists.
//
// One operation genuinely differs, and it is not a matter of spelling.
// POSIX makes a rename durable in two steps: rename, then fsync the
// directory holding it. Windows has no directory handle to sync -- its
// rename is ordered by the filesystem itself. So `syncDirectory` does the
// work on POSIX and nothing on Windows, and the difference is written down
// rather than hidden behind a name that suggests both do the same thing.

#include <cstddef>
#include <cstdint>
#include <string>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lrvx::fileio
{

using Fd = int;
inline constexpr Fd kInvalidFd = -1;

enum class OpenMode
{
  Truncate,
  Append,
};

// Opens for writing, creating the file if it is not there. Returns
// kInvalidFd on failure; the caller decides what that means.
inline Fd openForWrite(const std::string& path, OpenMode mode)
{
#if defined(_WIN32)
  const int flags = _O_WRONLY | _O_CREAT | _O_BINARY |
                    (mode == OpenMode::Truncate ? _O_TRUNC : _O_APPEND);
  // _sopen_s, not _open: the CRT marks _open deprecated, and this header is
  // public, so the warning would land in every consumer's build. _SH_DENYNO
  // is the sharing mode _open uses.
  int fd = kInvalidFd;
  if (::_sopen_s(&fd, path.c_str(), flags, _SH_DENYNO, _S_IREAD | _S_IWRITE) != 0)
  {
    return kInvalidFd;
  }
  return fd;
#else
  const int flags = O_WRONLY | O_CREAT | (mode == OpenMode::Truncate ? O_TRUNC : O_APPEND);
  return ::open(path.c_str(), flags, 0644);
#endif
}

inline void closeFd(Fd fd)
{
#if defined(_WIN32)
  ::_close(fd);
#else
  ::close(fd);
#endif
}

// Everything written so far reaches stable storage before this returns.
//
// One caveat that predates this header and still stands: on macOS `fsync`
// does not flush the drive's own write cache (`F_FULLFSYNC` does), so a
// durability figure measured there is an upper bound.
inline bool syncFd(Fd fd)
{
#if defined(_WIN32)
  return ::_commit(fd) == 0;
#else
  return ::fsync(fd) == 0;
#endif
}

// Writes the whole buffer, resuming after a short write. Returns false when
// the file refuses the bytes.
inline bool writeAll(Fd fd, const void* data, size_t n)
{
  const auto* p = static_cast<const unsigned char*>(data);
  size_t off = 0;
  while (off < n)
  {
#if defined(_WIN32)
    const int w = ::_write(fd, p + off, static_cast<unsigned int>(n - off));
#else
    const auto w = ::write(fd, p + off, n - off);
#endif
    if (w <= 0)
    {
      return false;
    }
    off += static_cast<size_t>(w);
  }
  return true;
}

// Gives a freshly created file its first block before anything is waiting on
// the next write.
//
// The first write() to a file that has just been created costs markedly more
// than the ones after it -- the filesystem allocates on it: measured at ~14 us
// against ~1.7 us steady state on APFS, for the same 100-byte record. A
// write-ahead log that rotates onto a new file at a checkpoint hands that cost
// to whatever command arrives first after the pause, which is a stall on the
// matching path that no checkpoint gauge can see. Paying it here, while the
// rotation still has the writer's attention, puts it where it belongs.
//
// The file is left exactly as it was found: one byte written, truncated away,
// the offset rewound. A crash in between leaves a one-byte file, which is
// shorter than any record header and reads back as an empty torn prefix.
// Returns false if the file would be left with the byte still in it.
inline bool reserveFirstBlock(Fd fd)
{
  const char zero = 0;
  if (!writeAll(fd, &zero, 1))
  {
    return false;
  }
#if defined(_WIN32)
  if (::_chsize_s(fd, 0) != 0)
  {
    return false;
  }
  return ::_lseek(fd, 0, SEEK_SET) == 0;
#else
  if (::ftruncate(fd, 0) != 0)
  {
    return false;
  }
  return ::lseek(fd, 0, SEEK_SET) == 0;
#endif
}

// Makes a rename into `dir` durable.
//
// POSIX: a renamed file can be present while the directory entry naming it
// is not, so the directory itself is synced. Windows offers no handle to
// the directory and orders the rename itself, so there is nothing to do and
// this returns true. A caller that treats a `false` here as "the rename may
// be lost" is right on POSIX and is never told so on Windows, because there
// it cannot be.
inline bool syncDirectory(const std::string& dir)
{
#if defined(_WIN32)
  (void)dir;
  return true;
#else
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0)
  {
    return false;
  }
  const bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
#endif
}

}  // namespace lrvx::fileio
