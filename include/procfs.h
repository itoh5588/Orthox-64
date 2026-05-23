#ifndef PROCFS_H
#define PROCFS_H

#include <stdint.h>
#include <stddef.h>
#include "fs.h"

fs_file_t* procfs_open(const char* subpath, int flags);

#endif
