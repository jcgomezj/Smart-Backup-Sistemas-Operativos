# Makefile – Smart Backup Kernel-Space Utility
# Universidad EAFIT – Sistemas Operativos

CC      = gcc
CFLAGS  = -Wall -Wextra -Wpedantic -std=c11 -O2
TARGET  = backup
SRCS    = main.c backup_engine.c
OBJS    = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c smart_copy.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)

bench: $(TARGET)
	./$(TARGET) --bench

.PHONY: all clean bench
