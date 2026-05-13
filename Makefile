CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
LDFLAGS ?= -pthread

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    LDFLAGS += -lrt
endif

BIN := catlady
SRC := catlady.c

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(BIN) $(LDFLAGS)

clean:
	rm -f $(BIN)

run: $(BIN)
	./$(BIN)

run-balanced: $(BIN)
	./$(BIN) 3 4 5 2 3 2 2 30

run-mice-heavy: $(BIN)
	./$(BIN) 2 1 6 1 5 3 1 20

run-cats-heavy: $(BIN)
	./$(BIN) 2 6 1 2 1 2 5 20

run-edge: $(BIN)
	./$(BIN) 1 1 1 2 2 2 2 15

run-stress: $(BIN)
	./$(BIN) 4 8 10 1 2 1 1 30

.PHONY: all clean run run-balanced run-mice-heavy run-cats-heavy run-edge run-stress
