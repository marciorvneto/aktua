#include "platform.h"
#include <stddef.h>
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
#include <sys/wait.h>
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

bool platform_open_file(const char *path){
#if defined(_WIN32)
	HINSTANCE hout = ShellExecuteA(NULL, "open", path, NULL, NULL, SW_SHOWNORMAL);
	return (INT_PTR)hout > 32;
#elif defined(__linux__)
	pid_t pid = fork();
	if(pid == -1) return false;
	if(pid == 0){ // Child
		execlp("xdg-open", "xdg-open", path, (char *)NULL);
		_exit(127);
	}
	int status;
	if(waitpid(pid, &status, 0) == -1) return false;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
#else
	#error "platform_open_file: unsupported platform"
#endif
}

FILE *platform_temp_file(const char *suffix, char *path_out, size_t path_size){
#if defined(_WIN32)
	char dir[MAX_PATH + 1];
	char tmp[MAX_PATH + 1];

	DWORD n = GetTempPathA(sizeof(dir), dir);
	if(n == 0 || n >= sizeof(dir)) return NULL;

	// Creates an empty temporary file
	if(!GetTempFileNameA(dir, "akt", 0, tmp)) return NULL;

	// Replace .tmp with the requested suffix
	char *ext = strrchr(tmp, '.');
	if(!ext){
		DeleteFileA(tmp);
		return NULL;
	}

	size_t stem_len = (size_t)(ext - tmp);
	size_t suffix_len = strlen(suffix);

	if(stem_len + suffix_len + 1 > path_size){
		DeleteFileA(tmp);
		return NULL;
	}

	memcpy(path_out, tmp, stem_len);
	memcpy(path_out + stem_len, suffix, suffix_len + 1);

	if(!MoveFileExA(tmp, path_out, MOVEFILE_REPLACE_EXISTING)){
		DeleteFileA(tmp);
		return NULL;
	}

	FILE *f = fopen(path_out, "wb");
	if(!f) DeleteFileA(path_out);
	return f;
#elif defined(__linux__)
	int len = snprintf(path_out, path_size, "/tmp/aktua-XXXXXX%s", suffix);
	if(len < 0 || (size_t) len >= path_size) return NULL;
	int fd = mkstemps(path_out, (int)strlen(suffix));
	if(fd == -1) return NULL;
	FILE *temp = fdopen(fd, "w");
	if(temp == NULL){
		close(fd);
		unlink(path_out);
		return NULL;
	}
	return temp;
#else
	#error "platform_temp_file: unsupported platform"
#endif
}
