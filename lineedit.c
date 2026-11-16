#include <stddef.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <termios.h>
#include <string.h>
#include <unistd.h>

#include "lineedit.h"

#define MAX_BUF_SIZE 4096
#define HISTORY_MAX  1024


#define CTRL_KEY(x) ( ~((1 << 5) | (1 << 6)) & (x) )


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

// Reads one byte, giving up after the VTIME timeout.
static int read_byte(unsigned char *c){
	return read(STDIN_FILENO, c, 1) == 1;
}

// Returns a byte (0-255), a Key above 1000, or -1 on a read error.
static int read_key(void){
	unsigned char c;
	ssize_t n;

	// Wait for the first byte - returns 0 until something comes, 
	// so we need to loop 
	while((n = read(STDIN_FILENO, &c, 1)) != 1){
		if(n == -1 && errno != EAGAIN && errno != EINTR) return -1;
	}

	if(c != KEY_ESC) return c;

	// An ESC arrived. If nothing follows quickly, it was the Esc key itself.
	unsigned char seq[3];
	if(!read_byte(&seq[0])) return KEY_ESC;
	if(!read_byte(&seq[1])) return KEY_ESC;

	if(seq[0] == '['){
		if(isdigit(seq[1])){
			if(!read_byte(&seq[2]) || seq[2] != '~') return KEY_ESC;
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


// We redraw the line every single time.
static void refresh_line(const char *prompt, Line *l){
	printf("\r");
	printf("%s", prompt);
	fwrite(l->buf, 1, l->len, stdout);
	printf("\x1b[K"); // Erase whatever is left to the right
	printf("\r");
	size_t col = strlen(prompt) + l->cursor;
	if(col > 0) printf("\x1b[%zuC", col); // Move right to the cursor
	fflush(stdout);
}

static void line_set(Line *l, const char *text, size_t len){
	memcpy(l->buf, text, len);
	l->len    = len;
	l->cursor = len;
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
		if(k == KEY_LEFT){
			if(l.cursor > 0) l.cursor--;
			refresh_line(prompt, &l);
			continue;
		}
		if(k == KEY_RIGHT){
			if(l.cursor < l.len) l.cursor++;
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
				memmove(
						&l.buf[l.cursor],         // "to"
						&l.buf[l.cursor + 1],     // "from"
						l.len - l.cursor - 1      // number of elements copied (see above)
						);
				l.len--;
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
				memmove(
						&l.buf[l.cursor - 1], // "to"
						&l.buf[l.cursor],     // "from"
						l.len - l.cursor      // number of elements copied (see above)
						);
				l.cursor--;
				l.len--;
				refresh_line(prompt, &l);

			}
			continue;
		}
		if(k == CTRL_KEY('d')){
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

		if(k >= 32 && k < 127 && l.len < MAX_BUF_SIZE - 1){
			// Inserting at cursor
			//
			// hello_    cursor @ 3, len = 5
			//    ^
			// helllo    copy "lo" one slot to the right (5 - 3 = 2 elements copied)
			//    ^
			// helXlo    buf[cursor] = X and then cursor++, len++
			//     ^     

			memmove(
					    &l.buf[l.cursor + 1], // "to"
					    &l.buf[l.cursor],     // "from"
							l.len - l.cursor      // number of elements copied (see above)
					);

			l.buf[l.cursor++] = (char)k;
			l.len++;
			refresh_line(prompt, &l);
		}
	}

	disable_raw_mode();
	printf("\n");

	l.buf[l.len] = '\0';
	return strdup(l.buf);

}
