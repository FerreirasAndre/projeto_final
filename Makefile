CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
SRC = src/main.c
BIN = prodcons

all: $(BIN)

$(BIN): $(SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(SRC) -o bin/$(BIN)
	@echo "Gerado bin/$(BIN)"

clean:
	rm -rf bin
