#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdio.h>

// Call once at startup
void platform_init(void);
void platform_exit(void);

bool is_path_sep(char c);
bool is_path_absolute(const char *path);

// Returns NULL if unset. Caller must NOT free the returned pointer
const char *platform_home_dir(void);

// Returns NULL on failure. Caller must free the returned pointer.
char *platform_exe_path(void);

bool platform_open_file(const char *path);

// Caller must close the file
FILE *platform_temp_file(const char *suffix, char *path_out, size_t path_size);

#endif // !PLATFORM_H


