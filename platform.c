#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>


#if defined(_WIN32)
#include <windows.h>

static HANDLE original_std_handle;
static UINT original_output_cp;
static DWORD original_console_mode;

#elif defined(__linux__)
#include <unistd.h>
#else
#endif

void platform_init(void){
#if defined(_WIN32)
	original_output_cp = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);

	original_std_handle = GetStdHandle(STD_OUTPUT_HANDLE);
	if(GetConsoleMode(original_std_handle, &original_console_mode)){
		SetConsoleMode(original_std_handle, original_console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
	}
#endif
}
void platform_exit(void){
#if defined(_WIN32)
	SetConsoleMode(original_std_handle, original_console_mode);
	SetConsoleOutputCP(original_output_cp);
#endif
}

bool is_path_sep(char c){
	if(c == '/') return true;
	if(c == '\\') return true;
	return false;
}

bool is_path_absolute(const char *path){
	if(!path || path[0] == '\0') return false;

	if(path[0] == '/' || path[0] == '\\') return true;

	if(
			isalpha((unsigned char)path[0]) &&
			path[1] == ':'                  &&
			is_path_sep(path[2]) // C:/ is valid too...
		) return true;

	return false;
}

const char *platform_home_dir(void){
#if defined(_WIN32)
	const char *home = getenv("USERPROFILE");
#elif defined(__linux__)
	const char *home = getenv("HOME");
#else
	#error "platform_home_dir: unsupported platform"
#endif
	return home;
}

char *platform_exe_path(void){
char exe_path[4096];
#if defined(_WIN32)
	DWORD len = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
	if(len == 0) return NULL;
	if(len == sizeof(exe_path)) return NULL;
#elif defined(__linux__)
	ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1); // Last bit for \0
	if(len < 0) return NULL;
	if(len == sizeof(exe_path) - 1) return NULL;
	exe_path[len] = '\0';
#else
	#error "platform_exe_path: unsupported platform"
#endif
	return strdup(exe_path);
}

