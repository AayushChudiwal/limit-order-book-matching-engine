#pragma once

#include <cstddef>
#include <span>
#include <string>

namespace lob::itch {

// A read-only mmap of a file, exposed as a std::span<const std::byte>.
// Zero-copy in the strongest available sense: the OS page cache is the
// only buffer involved -- there's no userspace read() copy at all, and the
// kernel handles readahead/paging for us. That's the right tradeoff for a
// hundreds-of-MB to multi-GB replay file; an ifstream read loop would work
// too, but every block read costs a copy into a userspace buffer that mmap
// skips entirely. POSIX-only (mmap/munmap) -- this project doesn't target
// Windows, so there's no MapViewOfFile fallback to maintain.
//
// Throws std::runtime_error on open/stat/mmap failure. That's exceptions
// on a one-shot CLI startup path, not the matching engine's hot path --
// Phase 5's "-fno-exceptions on the engine target" rule is scoped to the
// optimized engine specifically, not every tool in the repo.
class MappedFile {
  public:
    explicit MappedFile(const std::string& path);
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    [[nodiscard]] std::span<const std::byte> data() const { return {data_, size_}; }

  private:
    const std::byte* data_ = nullptr;
    std::size_t size_ = 0;
    int fd_ = -1;
};

}  // namespace lob::itch
