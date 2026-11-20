#ifdef _WIN32
#include <windows.h>
#else
#include <termios.h>
#endif

#include <stddef.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "lineedit.h"

#define MAX_BUF_SIZE 4096
#define HISTORY_MAX  1024
static LineCompletionFn completion_fn = NULL;

#define CTRL_KEY(x) ( ~((1 << 5) | (1 << 6)) & (x) )

// Portability

// Reads a line. The caller is responsible for freeing the buffer
char *read_line(FILE *f){
	size_t capacity = 128;
	size_t len      = 0;
	char *buf       = malloc(capacity);
	if(!buf){
		return NULL;
	}
	while(fgets(buf + len, capacity - len, f) != NULL){
		len += strlen(buf + len);

		if(len > 0 && buf[len - 1] == '\n'){
			// Read a newline. Stop here;
			break;
		}

		capacity *= 2;
		char *new_buf = realloc(buf, capacity);
		if(!new_buf){
			free(buf);
			return NULL;
		}
		buf = new_buf;
	}

	// If nothing was read at all, the while loop
	// will exit immediately and we should clean up
	// and return NULL
	
	if(len == 0){
		free(buf);
		return NULL;
	}

	// Stripping \n and \r\n
	buf[strcspn(buf, "\r\n")] = '\0';

	return buf;
}

// Core
#ifndef _WIN32
struct termios t_original;

static void enable_raw_mode(void){
	tcgetattr(STDIN_FILENO, &t_original);
	struct termios t = t_original;

	t.c_lflag &= ~(ECHO | ICANON);
	t.c_lflag &= ~(ISIG | IEXTEN);

	t.c_iflag &= ~(IXON | ICRNL);
	t.c_oflag &= ~(OPOST);

	t.c_cc[VMIN]  = 0;
	t.c_cc[VTIME] = 1;

	tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
}
void disable_raw_mode(void){
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &t_original);
}

static int read_byte_wait(unsigned char *c){
	ssize_t n = 0;
	while((n = read(STDIN_FILENO, c, 1)) != 1){
		if(n == -1 && errno != EAGAIN && errno != EINTR) return -1;
	}
	return 1;
}

// Reads one byte, giving up after the VTIME timeout.
static int read_byte_timeout(unsigned char *c){
	return read(STDIN_FILENO, c, 1) == 1;
}

#else // ============= Windows ===========

static HANDLE in_handle;
static DWORD  in_mode_original;
static UINT   input_cp_original;

static void enable_raw_mode(void){
	input_cp_original = GetConsoleCP();
	in_handle = GetStdHandle(STD_INPUT_HANDLE);
	GetConsoleMode(in_handle, &in_mode_original);
	SetConsoleCP(CP_UTF8);

	DWORD mode = in_mode_original;
	mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
	mode |=   ENABLE_VIRTUAL_TERMINAL_INPUT;
	
	SetConsoleMode(in_handle, mode);

}

static void disable_raw_mode(void){
	SetConsoleMode(in_handle, in_mode_original);
	SetConsoleCP(input_cp_original);
}

static int read_byte_wait(unsigned char *c){
	DWORD n;
	if(!ReadFile(in_handle, c, 1, &n, NULL) || n != 1) return -1;
	return 1;
}

// Reads one byte, giving up after the VTIME timeout.
static int read_byte_timeout(unsigned char *c){
	if(WaitForSingleObject(in_handle, 100) != WAIT_OBJECT_0) return 0;
	return read_byte_wait(c) == 1;
}

#endif

//==============================
//
//    Keys
//
//==============================

typedef enum {
	// Single-byte keys
	KEY_TAB       = 9,
	KEY_ENTER     = 13,
	KEY_ESC       = 27,
	KEY_BACKSPACE = 127,

	// Multi-byte keys
	KEY_UP = 1000,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_DELETE,
} Key;


// Returns a byte (0-255), a Key above 1000, or -1 on a read error.
static int read_key(void){
	unsigned char c;

	// Wait for the first byte - returns 0 until something comes, 
	// so we need to loop 
  int out = read_byte_wait(&c);
	if(out == -1) return -1;

	if(c != KEY_ESC) return c;

	// An ESC arrived. If nothing follows quickly, it was the Esc key itself.
	unsigned char seq[3];
	if(!read_byte_timeout(&seq[0])) return KEY_ESC;
	if(!read_byte_timeout(&seq[1])) return KEY_ESC;

	if(seq[0] == '['){
		if(isdigit(seq[1])){
			if(!read_byte_timeout(&seq[2]) || seq[2] != '~') return KEY_ESC;
			switch(seq[1]){
				case '1': case '7': return KEY_HOME;
				case '4': case '8': return KEY_END;
				case '3':           return KEY_DELETE;
			}
		}else{
			switch(seq[1]){
				case 'A': return KEY_UP;
				case 'B': return KEY_DOWN;
				case 'C': return KEY_RIGHT;
				case 'D': return KEY_LEFT;
				case 'H': return KEY_HOME;
				case 'F': return KEY_END;
			}
		}
	}else if(seq[0] == 'O'){
		switch(seq[1]){
			case 'H': return KEY_HOME;
			case 'F': return KEY_END;
		}
	}

	return KEY_ESC;
}

typedef struct {
	char   buf[MAX_BUF_SIZE];
	size_t len;
	size_t cursor;
} Line;

static char *history[HISTORY_MAX];
static size_t history_len = 0;

void line_history_add(const char* line){
	if(line[0] == '\0') return;
	if(history_len > 0 && strcmp(history[history_len - 1], line) == 0) return; // Avoid duplicates

	if(history_len == HISTORY_MAX){
		free(history[0]);
		memmove(
				&history[0],
				&history[1],
				(history_len - 1) * sizeof(char *)
			);
		history_len--;
	}
	history[history_len] = strdup(line);
	history_len++;
}

int line_history_load(const char* path){
	FILE *f = fopen(path, "r");
	if(f == NULL) return 1;
	
	char *line = NULL;
	while((line = read_line(f)) != NULL){
		line_history_add(line);
		free(line);
	}

	free(line);
	fclose(f);
	return 0;
}

int line_history_save(const char *path){
	FILE *f = fopen(path, "w");
	if(f == NULL) return 1;

	for(size_t i = 0; i < history_len; i++){
		fprintf(f, "%s\n", history[i]);
	}

	fclose(f);
	return 0;
}

// UTF-8 helpers
static bool utf8_is_continuation(unsigned char c){
	// Explanation: UTF-8 chars may take more than 1 byte, and
	// the top bits of the first byte tell us how many
	//
	// 0xxxxxxx   -> ASCII
	// 110xxxxx   -> 2 bytes
	// 1110xxxx   -> 3 bytes
	// 11110xxx   -> 4 bytes
	// 10xxxxxx   -> Continuation (middle or end)
	//
	// 11000000   -> 0xC0 mask. Anding with this mask tells us if it's a multi-byte char

	return (c & 0xC0) == 0x80;
}

static size_t utf8_char_count(const char *buf, size_t n){
	size_t count = 0;
	for(size_t i = 0; i < n; i++){
		if(!utf8_is_continuation((unsigned char)buf[i])) count++;
	}
	return count;
}



// We redraw the line every single time.
static void refresh_line(const char *prompt, Line *l){
	printf("\r");
	printf("%s", prompt);
	fwrite(l->buf, 1, l->len, stdout);
	printf("\x1b[K"); // Erase whatever is left to the right
	printf("\r");
	size_t col = strlen(prompt) + utf8_char_count(l->buf, l->cursor);
	if(col > 0) printf("\x1b[%zuC", col); // Move right to the cursor
	fflush(stdout);
}

static void line_set(Line *l, const char *text, size_t len){
	memcpy(l->buf, text, len);
	l->len    = len;
	l->cursor = len;
}

static int line_insert(Line *l, const char *text, size_t n){
	if(l->len + n >= MAX_BUF_SIZE) return 0;
	memmove(
		&l->buf[l->cursor + n],
		&l->buf[l->cursor],
		l->len - l->cursor
	);
	memcpy(&l->buf[l->cursor], text, n);
	l->len    += n;
	l->cursor += n;
	return 1;
}

static bool is_word_char(char c){
	return  isalnum((unsigned char)c) || c == '_';
}

static size_t common_prefix_len(LineCompletions *c){
	size_t n = strlen(c->items[0]);
	for(size_t i = 0; i < c->count; i++){
		size_t j = 0;
		while(j < n && c->items[i][j] == c->items[0][j]) j++;
		n = j;
	}
	return n;
}

char *line_read(const char *prompt){
	Line l           = {0};
	size_t hist_pos  = history_len;
	size_t saved_len = 0;
	char saved[MAX_BUF_SIZE];

	printf("%s", prompt);
	fflush(stdout);

	enable_raw_mode();

	while(1){
		int k = read_key();

		if(k == KEY_ENTER) break;
		if(k == KEY_TAB){
			if(completion_fn == NULL) continue;

			// Find word just before the cursor
			size_t start = l.cursor;
			while(start > 0 && is_word_char(l.buf[start - 1])) start--;
			size_t prefix_len = l.cursor - start;

			// Try to find matches
			LineCompletions c = {0};
			completion_fn(&l.buf[start], prefix_len, &c);

			if(c.count >= 1){
				size_t common = common_prefix_len(&c);
				if(common > prefix_len){
					line_insert(&l, c.items[0] + prefix_len, common - prefix_len);
					refresh_line(prompt, &l);
				}else if(c.count > 1){
					printf("\r\n");
					for(size_t i = 0; i < c.count; i++) printf("%s  ", c.items[i]);
					printf("\r\n");
					refresh_line(prompt, &l);
				}
			}
			continue;
		}
		if(k == KEY_LEFT){
			if(l.cursor > 0){
				do {l.cursor--;} while(l.cursor > 0 && utf8_is_continuation(l.buf[l.cursor]));
			}
			refresh_line(prompt, &l);
			continue;
		}
		if(k == KEY_RIGHT){
			if(l.cursor < l.len){
				l.cursor++;
				while(l.cursor < l.len && utf8_is_continuation(l.buf[l.cursor])) l.cursor++;
			}
			refresh_line(prompt, &l);
			continue;
		}
		if(k == KEY_UP || k == CTRL_KEY('p')){
			if(hist_pos > 0){
				if(hist_pos == history_len){ // Leaving the line being typed (save it)
					memcpy(saved, l.buf, l.len);
					saved_len = l.len;
				}
				hist_pos--;
				line_set(&l, history[hist_pos], strlen(history[hist_pos]));
				refresh_line(prompt, &l);
			}
			continue;
		}
		if(k == KEY_DOWN || k == CTRL_KEY('n')){
			if(hist_pos < history_len){
				hist_pos++;
				if(hist_pos == history_len){ // Restore what was saved
					line_set(&l, saved, saved_len);
				}else{
					line_set(&l, history[hist_pos], strlen(history[hist_pos]));
				}
				refresh_line(prompt, &l);
			}
			continue;
		}
		if(k == KEY_HOME || k == CTRL_KEY('a')){
			l.cursor = 0;
			refresh_line(prompt, &l);
			continue;
		}
		if(k == KEY_END || k == CTRL_KEY('e')){
			l.cursor = l.len;
			refresh_line(prompt, &l);
			continue;
		}
		if(k == KEY_DELETE){
			// Backspace at cursor
			//
			// hello    cursor @ 3, len = 5
			//    ^
			// heloo    copy "o" to where the cursor is (5 - 3 - 1 = 1 element copied)
			//    ^
			// helo-    update len, the trailing "o" no longer counts
			//     ^     

			if(l.cursor < l.len){
				size_t n = 1;
				while(l.cursor + n < l.len && utf8_is_continuation(l.buf[l.cursor + n])) n++;
				memmove(
						&l.buf[l.cursor],         // "to"
						&l.buf[l.cursor + n],     // "from"
						l.len - l.cursor - n      // number of elements copied (see above)
						);
				l.len -= n;
				refresh_line(prompt, &l);

			}
			continue;
		}
		if(k == KEY_BACKSPACE){
			// Backspace at cursor
			//
			// hello    cursor @ 3, len = 5
			//    ^
			// heloo    copy "lo" one slot to the left (5 - 3 = 2 elements copied)
			//   ^
			// helo-    update len, the trailing "o" no longer counts
			//     ^     

			if(l.cursor > 0){
				size_t n = 1;
				while(l.cursor - n > 0 && utf8_is_continuation(l.buf[l.cursor - n])) n++;
				memmove(
						&l.buf[l.cursor - n], // "to"
						&l.buf[l.cursor],     // "from"
						l.len - l.cursor      // number of elements copied (see above)
						);
				l.cursor -= n;
				l.len    -= n;
				refresh_line(prompt, &l);

			}
			continue;
		}
  #ifdef _WIN32
			if(k == CTRL_KEY('d') || k == CTRL_KEY('z')){
  #else
			if(k == CTRL_KEY('d')){
  #endif
			if(l.len == 0){
				disable_raw_mode();
				printf("\n");
				return NULL;
			}
			continue;
		}
		if(k == CTRL_KEY('c')){
			// Cancel the line
			l.len = 0;
			l.cursor = 0;
			printf("^C\r\n%s", prompt);
			fflush(stdout);
			continue;
		}

		if(k >= 32 && k != 127 && k < 256 && l.len < MAX_BUF_SIZE - 1){
			// Won't accept control characters (< 32), DEL (127) or special keys (>=256)
			char c = (char)k;
			line_insert(&l, &c, 1);
			refresh_line(prompt, &l);
		}
	}

	disable_raw_mode();
	printf("\n");

	l.buf[l.len] = '\0';
	return strdup(l.buf);

}

//===============================
//
//  Line completion
//
//===============================

void line_completions_add(LineCompletions *c, const char *name){
	if(c->count >= LINE_COMPLETIONS_MAX) return;
	c->items[c->count++] = name;
}

void line_set_completion(LineCompletionFn fn){
	completion_fn = fn;
}

