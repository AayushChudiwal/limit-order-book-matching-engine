#include "lob/itch/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>

namespace lob::itch {

MappedFile::MappedFile(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw std::runtime_error("MappedFile: failed to open " + path);
    }

    struct stat st{};
    if (::fstat(fd_, &st) != 0) {
        ::close(fd_);
        throw std::runtime_error("MappedFile: failed to stat " + path);
    }
    size_ = static_cast<std::size_t>(st.st_size);

    if (size_ == 0) {
        // mmap() of a zero-length file is unspecified/fails on most
        // platforms -- treat it as a valid, empty view rather than a
        // special case callers have to know about.
        ::close(fd_);
        fd_ = -1;
        return;
    }

    void* mapped = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped == MAP_FAILED) {
        ::close(fd_);
        throw std::runtime_error("MappedFile: failed to mmap " + path);
    }
    data_ = static_cast<const std::byte*>(mapped);
}

MappedFile::~MappedFile() {
    if (data_ != nullptr && size_ != 0) {
        ::munmap(const_cast<std::byte*>(data_), size_);
    }
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

}  // namespace lob::itch
