CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -I../common
SRC     = src/server.c
BIN     = server

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(BIN)

.PHONY: all clean
