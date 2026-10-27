/* Results shared by the filesystem drivers (src/fs/). */
#ifndef FS_FS_H
#define FS_FS_H

enum fs_error {
    FS_OK,
    FS_IO,          /* the device read failed */
    FS_NOT_FOUND,   /* a path component does not exist */
    FS_NOT_DIR,     /* a path component before the last is not a directory */
    FS_NOT_FILE,    /* the path names a directory or special file */
    FS_LOOP,        /* too many symlinks, or the path grew too long */
    FS_CORRUPT,     /* a structure is out of range or inconsistent */
    FS_UNSUPPORTED, /* a feature the driver does not read */
    FS_ERROR_COUNT
};

const char *fs_strerror(enum fs_error e);

#endif
