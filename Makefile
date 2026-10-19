CC = gcc
CFLAGS = -Wall -Wextra -O2 -g

all: mysh

mysh: mysh.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f mysh
