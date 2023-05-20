CC = gcc
CFLAGS = -Wall -Wextra
LIBS = 
TARGET = tcphup
SRCS = tcphup.c
all: $(TARGET)
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)
clean:
	rm -f $(TARGET)
