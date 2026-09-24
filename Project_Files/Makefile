# Compiler
CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -O2

# Targets
all: mkfs validator journal

mkfs: mkfs.c journal.h
	$(CC) $(CFLAGS) -o mkfs mkfs.c

validator: validator.c journal.h
	$(CC) $(CFLAGS) -o validator validator.c

journal: journal.c journal.h
	$(CC) $(CFLAGS) -o journal journal.c

clean:
	rm -f mkfs validator journal vsfs.img

test: all
	@echo "Creating filesystem..."
	./mkfs
	@echo "\nValidating initial state..."
	./validator
	@echo "\nCreating a file via journal (metadata staged, not yet installed)..."
	./journal create hello.txt
	@echo "\nValidating after create (journal has a pending transaction)..."
	./validator
	@echo "\nInstalling the journaled transaction..."
	./journal install
	@echo "\nFinal validation..."
	./validator
	@echo "\n=== All tests completed ==="

.PHONY: all clean test
