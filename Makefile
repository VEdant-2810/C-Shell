CC = cc
CFLAGS = -std=c11 -Wall -Wextra -pedantic -g
SRCS = main.c src/shell.c src/scheduler.c
OBJS = $(SRCS:.c=.o)
TARGET = c-shell

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -Iinclude -o $(TARGET) $(SRCS)

clean:
	rm -f $(TARGET) *.o src/*.o
