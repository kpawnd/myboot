#include "fs/fs.h"

const char *fs_strerror(enum fs_error e)
{
    switch (e) {
    case FS_OK:          return "success";
    case FS_IO:          return "disk read failed";
    case FS_NOT_FOUND:   return "not found";
    case FS_NOT_DIR:     return "a path component is not a directory";
    case FS_NOT_FILE:    return "not a regular file";
    case FS_LOOP:        return "too many symlinks or path too long";
    case FS_CORRUPT:     return "filesystem corrupt";
    case FS_UNSUPPORTED: return "uses a filesystem feature MYBOOT cannot read";
    default:             return "unknown filesystem error";
    }
}
