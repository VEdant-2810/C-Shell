# C-SHELL Implementation Checklist

This file maps the project requirements to the implementation in this repo and explains where each feature is implemented.

Project files
- `main.c` — program entry; calls `shell_loop()`.
- `Makefile` — build targets: `make`, `make clean`.
- `README.md` — quick usage.
- `include/shell.h` — public `shell_loop()` declaration.
- `src/shell.c` — full shell implementation. Key functions and line references are listed below.

Implemented features (and where)
- Prompt and input loop: `shell_loop()` (src/shell.c lines ~438–525).
- Reading input: `read_line()` (src/shell.c lines ~129–142).
- Tokenization and splitting:
  - `split_unquoted()` — splits on `;` and `|` while respecting quotes (src/shell.c near its definition).
  - `tokenize()` — quote-aware tokenizer that recognizes operators (`<`, `>`, `>>`, `|`, `&`) and backslash escaping.
- Builtins:
  - `cd`, `exit` in `do_builtin()` (src/shell.c lines ~256–276).
  - `jobs`, `fg`, `bg` in `do_builtin()` (src/shell.c lines ~277–322). Job list helpers: `add_job()`, `remove_job()`, `print_jobs()` (src/shell.c lines ~36–84).
- I/O redirection: parsed in `parse_command()` and applied in `launch_pipeline()` (src/shell.c lines ~324–436).
- Pipes: implemented in `launch_pipeline()` — sets up pipes between stages and forks processes (src/shell.c lines ~324–436).
- Sequential `;` and background `&`: handled in `shell_loop()` and `launch_pipeline()` (src/shell.c lines ~462–509 and ~413–436).
- Job control and signals: `sigchld_handler()` handles child state changes and updates job list; `init_shell()` configures shell PGID and signal handling (src/shell.c lines ~87–127).
- Environment variable expansion: `$VAR` and `${VAR}` supported via `expand_env_vars()` used in `parse_command()`.
- Quoted strings and escapes: `tokenize()` supports single quotes (no expansion), double quotes (allowing escapes), and backslash escaping.
- Scheduler: `src/scheduler.c` implements a simple MLFQ scheduler for delayed commands. Scheduler readiness is checked before each prompt and ready jobs are executed automatically.

- Quoted-string handling: `tokenize()` recognizes single (`'...'`) and double (`"..."`) quotes and treats enclosed text as a single token (see `tokenize()` in `src/shell.c`).
- Escape sequences: backslash (`\\`) escaping is supported in `tokenize()` to include literal special characters.
- Environment-variable expansion: `expand_env_vars()` implements `$VAR` and `${VAR}` expansion and is invoked from `parse_command()` so `argv` entries are expanded before execution.
- MLFQ scheduler: `scheduler_init()`, `scheduler_add_job()`, `scheduler_check_pending()`, and `scheduler_print_queue()` in `src/scheduler.c`.

These additions address common parsing edge-cases and bring the shell closer to POSIX-like behavior for basic scripts.


