#include "platform/Platform.h"

#ifndef _WIN32
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>  // for remove/error printing



namespace utils {
MappedFile* map_file(const char* path, u64 size, bool create_and_resize) {
    MappedFile* out_map = new MappedFile{};

    out_map->data = nullptr;
    out_map->size = 0;

    // --- LINUX / POSIX IMPLEMENTATION ---
    int flags = O_RDONLY;
    if (create_and_resize) flags = O_RDWR | O_CREAT | O_TRUNC;

    int fd = open(path, flags, 0644);
    if (fd == -1) return nullptr;

    if (create_and_resize) {
        // CRITICAL: On Linux, you must explicitly expand the file size
        // before mmapping, otherwise you get a bus error when writing.
        if (ftruncate(fd, size) == -1) {
            close(fd);
            return nullptr;
        }
    } else {
        // Get actual size
        struct stat sb;
        if (fstat(fd, &sb) == -1) {
            close(fd);
            return nullptr;
        }
        size = sb.st_size;
    }

    int prot = PROT_READ;
    if (create_and_resize) prot |= PROT_WRITE;

    void* ptr = mmap(NULL, size, prot, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    out_map->data = ptr;
    out_map->size = size;
    // Store fd in handle_1 (cast to void*), handle_2 unused
    out_map->internal_handle_1 = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
    out_map->internal_handle_2 = nullptr;
    return out_map;
}

void unmap_file(MappedFile* map) {

    if (!map || !map->data) return;

    munmap(map->data, map->size);
    close(static_cast<int>(reinterpret_cast<intptr_t>(map->internal_handle_1)));

    map->data = nullptr;
    map->size = 0;
    delete map;
}
}  // namespace utils
#endif
