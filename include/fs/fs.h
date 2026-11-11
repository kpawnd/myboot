/* Results shared by the filesystem drivers (src/fs/). */
#ifndef FS_FS_H
#define FS_FS_H

enum fs_error {
    FS_OK,
    FS_IO,
    FS_NOT_FOUND,
    FS_NOT_DIR,
    FS_NOT_FILE,
    FS_LOOP,
    FS_CORRUPT,
    FS_UNSUPPORTED,
    FS_ERROR_COUNT
};

const char *fs_strerror(enum fs_error e);

#endif
