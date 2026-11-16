#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <stdio.h>

char *line_read(const char *prompt);
void line_history_add(const char* line);
int line_history_load(const char *path);
int line_history_save(const char *path);

#define LINE_COMPLETIONS_MAX 256
typedef struct {
	const char *items[LINE_COMPLETIONS_MAX];
	size_t      count;
} LineCompletions;

typedef void (*LineCompletionFn)(const char *prefix, size_t prefix_len, LineCompletions *out);
void line_set_completion(LineCompletionFn fn);
void line_completions_add(LineCompletions *c, const char *name);

#endif // !LINEEDIT_H
