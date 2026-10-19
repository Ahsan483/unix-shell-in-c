# mysh: a small Unix shell in C

A working shell built on `fork`, `execvp`, `pipe`, `dup2` and `waitpid`.

- Pipelines of any length: `cat file | grep x | sort | head`
- Redirection: `<`, `>`, `>>`
- Background jobs with `&`, plus a `jobs` built-in. Finished children are reaped by a `SIGCHLD` handler, so there are no zombies.
- Quotes (`'...'`, `"..."`) and wildcards (`*.c`, using `glob(3)`)
- Built-ins: `cd`, `pwd`, `jobs`, `exit`
- Ctrl-C stops the running command, not the shell

## Build and run

```sh
make
./mysh
```

## Example session

```
mysh> /home/ahsan/os-projects
mysh> 7
mysh> P3 4 1 4
P5 6 3 5
P2 2 4 1
mysh> mysh> hello from mysh
mysh> [1] 334
mysh> [1] 334  Running  sleep 2 &
mysh> mysh: notacommand: command not found
mysh>
```

## How it works

1. `tokenize()` splits the line into words and operators and handles quotes.
2. `parse()` builds a list of commands with their redirections and expands wildcards.
3. `execute()` forks one child per command, connects neighbours with pipes, applies redirections with `dup2`, then calls `execvp`.
4. The parent waits for foreground jobs. Background jobs are recorded and reaped asynchronously.

## About

Written by Ahsan (Code Projects on YouTube).
I build and debug operating-systems, graphics and desktop projects. Issues and pull requests are welcome.

Licensed under the MIT License.
