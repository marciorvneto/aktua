#ifndef LINEEDIT_H
#define LINEEDIT_H

char *line_read(const char *prompt);
void line_history_add(const char* line);

#endif // !LINEEDIT_H
