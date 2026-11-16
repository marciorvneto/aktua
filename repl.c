#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lineedit.h"

static const char *test_names[] = { "print", "poly", "polymod", "polydiv", "linspace", "let" };
void test_complete(const char *prefix, size_t prefix_len, LineCompletions *out){
	for(size_t i = 0; i < sizeof(test_names) / sizeof(test_names[0]); i++){
		if(strncmp(test_names[i], prefix, prefix_len) == 0){
			line_completions_add(out, test_names[i]);
		}
	}       
}       
int main(void){

	line_set_completion(test_complete);
	while(1){
		char *line = line_read("test> ");
		if(line == NULL) break;           // Ctrl-D
		line_history_add(line);
		printf("you typed: [%s]\n", line);
		free(line);
	}

	return 0;
}
