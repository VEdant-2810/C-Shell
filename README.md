# C-SHELL 

Minimal POSIX-style shell written in C.

Features implemented
- Prompt showing current working directory
- Read input with `getline()`
- Quote-aware tokenization and escaping (`'...'`, `"..."`, `\\`)
- Environment-variable expansion: `$VAR` and `${VAR}`
- I/O redirection: `<`, `>`, `>>`
- Pipes (`|`) and pipelines
- Background execution with `&`
- Job control and signals: `jobs`, `fg`, `bg`, `SIGCHLD` handling
- Scheduler built-in: `schedule <seconds> <command>`
- Multi-Level Feedback Queue (MLFQ) scheduling for delayed jobs
- Builtins: `cd`, `exit`, `jobs`, `fg`, `bg`, `schedule`

Files
- `main.c` — program entry; starts the shell loop.
- `include/shell.h` — shell API.
- `src/shell.c` — core implementation (parsing, execution, job control, scheduler integration).
- `src/scheduler.c` — scheduler module implementing an MLFQ for delayed commands.
- `include/scheduler.h` — scheduler public API.
- `CHECKLIST.md` — mapping of project requirements to code locations.

Build

```sh
make
```

Run

```sh
./c-shell
```

Quick examples
- Run a pipeline: `ls -l | grep ".c"`
- Redirect output: `echo hi > out.txt`
- Background job: `sleep 10 &` then `jobs`
- Schedule a delayed command: `schedule 10 echo hello`
- Prevent expansion: `echo '\$HOME'` (single quotes)

Known limitations
- Token single-quote no-expansion semantics are approximated in some edge cases.
- Parsing assumes simple whitespace separation for some operator-edge cases (e.g., `cmd>file`).

See [CHECKLIST.md](CHECKLIST.md) for a detailed mapping of the assignment requirements to the implementation (file/function locations and notes).

