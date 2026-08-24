#include "platform/Platform.h" // IWYU pragma: keep

#ifdef _WIN32
#include <windows.h>

namespace utils {
MappedFile* map_file(const char* path, u64 size, bool create_and_resize) {
    MappedFile* out_map = new MappedFile{};

    // --- WINDOWS IMPLEMENTATION ---
    DWORD access     = GENERIC_READ;
    DWORD share      = FILE_SHARE_READ;
    DWORD creation   = OPEN_EXISTING;
    DWORD protect    = PAGE_READONLY;
    DWORD map_access = FILE_MAP_READ;

    if (create_and_resize) {
        access |= GENERIC_WRITE;
        creation   = CREATE_ALWAYS;  // Overwrite if exists
        protect    = PAGE_READWRITE;
        map_access = FILE_MAP_WRITE;
    }

    HANDLE hFile = CreateFileA(path, access, share, nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return 0;

    // On Windows, specifying the size in CreateFileMapping automatically resizes the file
    // if we are in Read/Write mode.
    DWORD size_high = static_cast<DWORD>((size >> 32) & 0xFFFFFFFF);
    DWORD size_low  = static_cast<DWORD>(size & 0xFFFFFFFF);

    if (!create_and_resize) {
        // If reading, get actual file size
        LARGE_INTEGER fileSize;
        if (!GetFileSizeEx(hFile, &fileSize)) {
            CloseHandle(hFile);
            return 0;
        }
        size = static_cast<size_t>(fileSize.QuadPart);
        // Set these to 0 for CreateFileMapping to map the whole existing file
        size_high = 0;
        size_low  = 0;
    }

    HANDLE hMap = CreateFileMappingA(hFile, nullptr, protect, size_high, size_low, nullptr);
    if (hMap == nullptr) {
        CloseHandle(hFile);
        return 0;
    }

    void* ptr = MapViewOfFile(hMap, map_access, 0, 0, size);
    if (ptr == nullptr) {
        CloseHandle(hMap);
        CloseHandle(hFile);
        return 0;
    }

    out_map->data              = ptr;
    out_map->size              = size;
    out_map->internal_handle_1 = hFile;
    out_map->internal_handle_2 = hMap;
    return out_map;
}

void unmap_file(MappedFile* map) {

    if (!map || !map->data) return;

    UnmapViewOfFile(map->data);
    CloseHandle(static_cast<HANDLE>(map->internal_handle_2));  // Map handle
    CloseHandle(static_cast<HANDLE>(map->internal_handle_1));  // File handle

    map->data = nullptr;
    map->size = 0;
    delete map;
}
}  // namespace utils
#endif
