#ifndef INTERPRETER_H
#define INTERPRETER_H

#include "core.h"

Arena  arena_create(size_t capacity);
Environment env_create(Arena *pa, Arena *fa, RunStats *run_stats, char *exe_path);
int  launch_repl(Arena *ta, Environment *e, char *script_path, bool quiet);
int  run_program(Arena *ta, Environment *e, FILE *f, char *script_path);
void cleanup(Arena *ta, Arena *a, Arena *fa, char *exe_path);
void print_usage(void);
void print_version(void);
void print_builtins(void);

#endif // !INTERPRETER_H

