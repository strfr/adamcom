PREFIX   ?= /usr/local
BINDIR    = $(PREFIX)/bin
CC        = g++
CFLAGS    = -std=c++11 -Wall
SRC       = adamcom.cpp
TARGET    = adamcom

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
