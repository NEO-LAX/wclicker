CC      = gcc
CFLAGS  = $(shell pkg-config --cflags gtk4) -Wall -Wextra -O2
LIBS    = $(shell pkg-config --libs gtk4) -lpthread
TARGET  = wclicker

all: $(TARGET)

$(TARGET): wclicker.c
	$(CC) $(CFLAGS) wclicker.c -o $(TARGET) $(LIBS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)

.PHONY: all clean install
