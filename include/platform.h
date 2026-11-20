#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>

bool is_path_sep(char c);
bool is_path_absolute(const char *path);

// Returns NULL if unset. Caller must NOT free the returned pointer
const char *platform_home_dir(void);

// Returns NULL on failure. Caller must free the returned pointer.
char *platform_exe_path(void);

#endif // !PLATFORM_H


