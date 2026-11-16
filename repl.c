#include <stdio.h>
#include <stdlib.h>
#include "lineedit.h"

int main(void){

	while(1){
		char *line = line_read("test> ");
		if(line == NULL) break;           // Ctrl-D
		line_history_add(line);
		printf("you typed: [%s]\n", line);
		free(line);
	}

	return 0;
}
