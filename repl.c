#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#define CTRL_KEY(x) ( ~((1 << 5) | (1 << 6)) & (x) )


struct termios t_original;

void enable_raw_mode(void){
	struct termios t;

	tcgetattr(STDIN_FILENO, &t);
	t.c_lflag &= ~(ECHO | ICANON);
	t.c_lflag &= ~(ISIG | IEXTEN);

	t.c_iflag &= ~(IXON | ICRNL);
	t.c_oflag &= ~(OPOST);

	// read() returns after at most 100 ms, with 0 bytes if nothing arrived.
	// That timeout is what lets us tell a lone ESC from an escape sequence.
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
	// Single-byte keys, named for readability
	KEY_TAB       = 9,
	KEY_ENTER     = 13,
	KEY_ESC       = 27,
	KEY_BACKSPACE = 127,

	// Multi-byte keys, above any byte value
	KEY_UP = 1000,
	KEY_DOWN,
	KEY_LEFT,
	KEY_RIGHT,
	KEY_HOME,
	KEY_END,
	KEY_DELETE,
} Key;

// Reads one byte, giving up after the VTIME timeout. Returns 1 if a byte arrived.
static int read_byte(unsigned char *c){
	return read(STDIN_FILENO, c, 1) == 1;
}

// Returns a byte (0-255), a Key above 1000, or -1 on a read error.
int read_key(void){
	unsigned char c;
	ssize_t n;

	// Wait for the first byte. read() returns 0 every 100 ms while idle, so loop.
	while((n = read(STDIN_FILENO, &c, 1)) != 1){
		if(n == -1 && errno != EAGAIN && errno != EINTR) return -1;
	}

	if(c != KEY_ESC) return c;

	// An ESC arrived. If nothing follows quickly, it was the Esc key itself.
	// Sequences come in a few shapes:
	//   ESC [ A..D        arrows
	//   ESC [ H / ESC [ F Home / End (xterm style)
	//   ESC O H / ESC O F Home / End ("application mode")
	//   ESC [ n ~         Home (1, 7), Delete (3), End (4, 8) - tmux sends these
	// Anything we don't recognize (Alt+key, F-keys, ...) collapses to KEY_ESC.
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

const char *key_name(int k){
	switch(k){
		case KEY_TAB:       return "TAB";
		case KEY_ENTER:     return "ENTER";
		case KEY_ESC:       return "ESC";
		case KEY_BACKSPACE: return "BACKSPACE";
		case KEY_UP:        return "UP";
		case KEY_DOWN:      return "DOWN";
		case KEY_LEFT:      return "LEFT";
		case KEY_RIGHT:     return "RIGHT";
		case KEY_HOME:      return "HOME";
		case KEY_END:       return "END";
		case KEY_DELETE:    return "DELETE";
	}
	return NULL;
}

int main(void){
	tcgetattr(STDIN_FILENO, &t_original);
	if(atexit(disable_raw_mode) != 0){
		printf("Failed to register cleanup callback\n");
		exit(1);
	}
	enable_raw_mode();

	while(1){
		int k = read_key();
		if(k == -1 || k == CTRL_KEY('d')) break;

		const char *name = key_name(k);
		if(name)            printf("%s\r\n", name);
		else if(k < 32)     printf("Ctrl-%c\r\n", k + 'A' - 1);
		else                printf("'%c' (%d)\r\n", k, k);
	}

	return 0;
}
