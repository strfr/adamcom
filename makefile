# ADAMCOM - Serial/CAN Terminal
# Makefile

PREFIX    ?= /usr/local
BINDIR     = $(PREFIX)/bin

CXX        = g++
CXXFLAGS   = -std=c++17 -Wall -Wextra -Wpedantic -O2 -Iinclude
LDLIBS     = -lreadline

# Source files
SRCDIR     = src
SRCS       = $(SRCDIR)/main.cpp \
             $(SRCDIR)/config.cpp \
             $(SRCDIR)/io.cpp \
             $(SRCDIR)/menu.cpp

OBJS       = $(SRCS:.cpp=.o)
TARGET     = adamcom

# Pattern rule for compiling
$(SRCDIR)/%.o: $(SRCDIR)/%.cpp include/adamcom.hpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

.PHONY: all install uninstall clean debug

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

# Debug build with symbols and no optimization
debug: CXXFLAGS = -std=c++17 -Wall -Wextra -Wpedantic -g -O0 -Iinclude -DDEBUG
debug: clean $(TARGET)

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(TARGET) $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

# Show help
help:
	@echo "Targets:"
	@echo "  all       - Build adamcom (default)"
	@echo "  debug     - Build with debug symbols"
	@echo "  install   - Install to $(BINDIR)"
	@echo "  uninstall - Remove from $(BINDIR)"
	@echo "  clean     - Remove build artifacts"
	@echo "  help      - Show this message"
