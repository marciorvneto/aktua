#ifndef LINEEDIT_H
#define LINEEDIT_H

char *line_read(const char *prompt);
void line_history_add(const char* line);
int line_history_load(const char *path);
int line_history_save(const char *path);

#endif // !LINEEDIT_H
