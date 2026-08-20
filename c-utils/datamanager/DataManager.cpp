#include "datamanager/DataManager.h"
#include <algorithm>
#include <dirent.h>
#include "Utils.h"
#include <unordered_map>
#include "libzip/git/lib/zip.h"
#include "platform/Platform.h"
#include "thread/Thread.h"
#include "logger/Logger.h"
#include "string/String.h"

namespace utils {
static std::unordered_map<std::string, struct zip*> zipHandles;
static struct zip* findPak(const char* path);
static Thread lock = {.cond = {}, .mutex = PTHREAD_MUTEX_INITIALIZER, .thread = {}};

void dataManagerInit(void) {
    info("dataManager: initializing");

    String tempString = {};
    stringPrintf(&tempString, "%s%s", platform.cwd, "data");
    std::vector<std::string> foundPaks = {};

    /// FIND PAK FILES
    DIR* directory;
    struct dirent* dir;
    directory = opendir(tempString.data);
    if (directory) {
        while ((dir = readdir(directory)) != nullptr) {
            stringPrintf(&tempString, dir->d_name);
            if (stringEndsWith(&tempString, "pak")) {
                foundPaks.push_back(dir->d_name);
            }
        }
        closedir(directory);
    } else {
        terminate("dataManager: data directory not found");
    }

    if (static_cast<i32>(foundPaks.size()) == 0) {
        warn("dataManager: No paks found!");
        // terminate("could not find any data files (pak)");
    }

    /// SORT PAK FILES DESCENDING
    std::sort(foundPaks.begin(), foundPaks.end(), [](const std::string& a, const std::string& b) { return b < a; });

    /// CREATE ZIP HANDLES FOR PAK FILES
    for (u32 i = 0, s = static_cast<i32>(foundPaks.size()); i < s; i++) {
        debug("dataManager: found pak %s", foundPaks[i].c_str());
        stringPrintf(&tempString, "%s%s%s%s", platform.cwd, "data", platform.seperator, foundPaks[i].c_str());
        int err               = 0;
        struct zip* zipHandle = zip_open(tempString.data, ZIP_RDONLY, &err);
        zip_stat_t st;
        zip_stat_init(&st);
        zip_stat_index(zipHandle, 1, 0, &st);

        if (st.comp_method != ZIP_CM_STORE) {
            warn("pak is compressed; chunk reads will be slow");
        }

        zipHandles[foundPaks[i]] = zipHandle;
        if (err > 0 || !zipHandle) {
            terminate("dataManager: error loading pak => %s", foundPaks[i].c_str());
        }
    }

    stringDestroy(&tempString);
}

u32 dataManagerGetSize(const char* path) {
    struct zip* zipHandle = findPak(path);
    if (!zipHandle) {
        terminate("dataManager: could not find %s ", path);
    }

    struct zip_stat zipStats = {};
    zip_stat_init(&zipStats);
    zip_stat(zipHandle, path, 0, &zipStats);

    return zipStats.size;
}

u32 dataManagerGetCRC(const char* path) {
    struct zip* zipHandle = findPak(path);
    if (!zipHandle) {
        terminate("dataManager: could not find %s ", path);
    }

    struct zip_stat zipStats = {};
    zip_stat_init(&zipStats);
    zip_stat(zipHandle, path, 0, &zipStats);

    return zipStats.crc;
}

bool dataManagerFileExists(const char* path) {
    struct zip* zipHandle = findPak(path);
    if (!zipHandle) {
        return false;
    }
    return true;
}

struct zip* findPak(const char* path) {
    for (const auto& entry : zipHandles) {
        struct zip* zipHandle    = entry.second;
        struct zip_stat zipStats = {};
        zip_stat_init(&zipStats);
        zip_stat(zipHandle, path, 0, &zipStats);
        if (zipStats.size > 0) {
            return zipHandle;
        }
    }
    return nullptr;
}

void dataManagerDestroy(void) {
    for (const auto& entry : zipHandles) {
        zip_close(entry.second);
    }

}

String dataManagerRead(const char* path) {
    threadLock(&lock);
    struct zip* zipHandle = findPak(path);
    if (!zipHandle) {
        terminate("dataManager: could not find %s ", path);
    }

    struct zip_stat zipStats = {};
    zip_stat_init(&zipStats);
    zip_stat(zipHandle, path, 0, &zipStats);

    String string = {};
    stringSetSize(&string, zipStats.size);

    struct zip_file* f = zip_fopen(zipHandle, path, 0);
    zip_fread(f, string.data, zipStats.size);
    zip_fclose(f);

    threadUnlock(&lock);
    return string;
}

void dataManagerReadChunk(const char* path, void* buffer, u32 offset, u32 size) {
    threadLock(&lock);  // Still need this because libzip isn't fully thread-safe on single handle

    struct zip* zipHandle = findPak(path);
    if (!zipHandle) {
        terminate("dataManager: could not find %s ", path);
    }

    struct zip_file* f = zip_fopen(zipHandle, path, 0);
    if (f) {
        zip_fseek(f, offset, SEEK_SET);

        zip_fread(f, buffer, size);
        zip_fclose(f);
    }

    threadUnlock(&lock);
}

std::vector<String> dataManagerListFiles(const char* extension) {
    std::vector<String> result = {};
    std::unordered_map<std::string, char> seen = {};

    threadLock(&lock);
    for (const auto& entry : zipHandles) {
        struct zip* zipHandle    = entry.second;
        zip_int64_t numEntries   = zip_get_num_entries(zipHandle, 0);
        for (zip_int64_t j = 0; j < numEntries; j++) {
            const char* name = zip_get_name(zipHandle, (zip_uint64_t)j, 0);
            if (!name) continue;
            if (!strEndsWithC(name, extension)) continue;
            if (seen.contains(name)) continue;
            seen[name] = 1;
            String s = {};
            stringAppend(&s, name);
            result.push_back(s);
        }
    }
    threadUnlock(&lock);

    return result;
}

void* dmRmlopen(const char* path) {
    threadLock(&lock);
    struct zip* zipHandle = findPak(path);
    if (!zipHandle) {
        terminate("dataManager: could not find %s ", path);
    }

    i64 index = zip_name_locate(zipHandle, path, 0);
    if (index == -1) {
        terminate("dataManager: file not found %s", path);
    }

    struct zip_stat st = {};

    zip_stat_init(&st);
    zip_stat(zipHandle, path, 0, &st);

    ZipFile* zipFile = new ZipFile{};
    zipFile->zipfile = zip_fopen(zipHandle, path, 0);
    zipFile->size    = st.size;
    threadUnlock(&lock);

    return zipFile;
}

void dmRmlclose(void* file) {
    threadLock(&lock);
    ZipFile* zipFile = static_cast<ZipFile*>(file);
    zip_fclose(zipFile->zipfile);
    delete zipFile;
    threadUnlock(&lock);
}

unsigned long long dmRmlread(void* buffer, unsigned long long size, void* file) {
    threadLock(&lock);
    ZipFile* zipFile = static_cast<ZipFile*>(file);
    u64 read         = zip_fread(zipFile->zipfile, buffer, size);
    zipFile->pos     = zipFile->pos + read;
    threadUnlock(&lock);

    return read;
}

int dmRmlseek(void* file, unsigned long long offset, int origin) {
    threadLock(&lock);
    ZipFile* zipFile = static_cast<ZipFile*>(file);

    if (origin == SEEK_CUR) {
        zipFile->pos += offset;
    } else if (origin == SEEK_SET) {
        zipFile->pos = offset;
    } else if (origin == SEEK_END) {
        zipFile->pos = zipFile->size + offset;
    }

    //  if (zipFile->index == 12) logger.info("seek %zu", zipFile->pos);
    // #define SEEK_SET	0	/* Seek from beginning of file.  */
    // #define SEEK_CUR	1	/* Seek from current position.  */
    // #define SEEK_END	2	/* Seek from end of file.  */
    threadUnlock(&lock);

    return 1;
}

unsigned long long dmRmltell(void* file) {
    threadLock(&lock);
    ZipFile* zipFile = static_cast<ZipFile*>(file);
    threadUnlock(&lock);
    return zipFile->pos;
}
}  // namespace utils
