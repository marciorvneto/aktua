#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"
#include "platform.h"
#include "interpreter.h"

int main(int argc, char **argv){
	platform_init();
	atexit(platform_exit);
	RunStats run_stats = {0};

	char *exe_path_owned = platform_exe_path();
	char *exe_path       = exe_path_owned ? exe_path_owned : argv[0];
	Arena ta             = arena_create(32 * 1024 * 1024); // 32 MB for now
	Arena pa             = arena_create(32 * 1024 * 1024); // 32 MB for now
	Arena fa             = arena_create(1 * 1024 * 1024);  // Frame arena
	Environment e        = env_create(&pa, &fa, &run_stats, exe_path);

	if(argc < 2){
		// REPL
		int code = launch_repl(&ta, &e, argv[0], false);
		cleanup(&ta, &pa, &fa, exe_path_owned);
		return code;
	}else{

		if(strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0){
			print_usage();
			cleanup(&ta, &pa, &fa, exe_path_owned);
			exit(0);
		}

		if(strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0){
			print_version();
			cleanup(&ta, &pa, &fa, exe_path_owned);
			exit(0);
		}

		if(strcmp(argv[1], "-q") == 0 || strcmp(argv[1], "--quiet") == 0){
			int code = launch_repl(&ta, &e, argv[0], true);
			cleanup(&ta, &pa, &fa, exe_path_owned);
			return code;
		}

		if(strcmp(argv[1], "--builtins") == 0){
			print_builtins();
			cleanup(&ta, &pa, &fa, exe_path_owned);
			exit(0);
		}

		if(strcmp(argv[1], "--ast") == 0){
			if(argc < 3){
				print_usage();
				cleanup(&ta, &pa, &fa, exe_path_owned);
				exit(1);
			}
			FILE *f  = fopen(argv[2], "r");
			int code = 0;
			if(f != NULL){
				code = print_program_ast(&e, f);
			}else{
				fprintf(stderr, "Could not load file at %s\n", argv[2]);
				return 1;
			}
			cleanup(&ta, &pa, &fa, exe_path_owned);
			fclose(f);
			return code;
		}



		// Launch a program

		FILE *f = fopen(argv[1], "r");

		if(f != NULL){
			int code = run_program(&ta, &e, f, argv[1]);
			if(run_stats.failed_asserts > 0 || run_stats.runtime_errors > 0){ 
				fprintf(stderr, "%zu assertion(s) failed, %zu runtime error(s)\n",
						run_stats.failed_asserts, run_stats.runtime_errors);
				return 1; 
			}
			fclose(f);
			return code;
		}else{
			fprintf(stderr, "Could not load file at %s\n", argv[1]);
			return 1;
		}

	}

	return 0;
}
