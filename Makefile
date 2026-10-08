CXX ?= g++
CXXFLAGS ?= -O3 -std=c++17 -Wall -Wextra -ffast-math -pthread
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin

# Check for libraries via pkg-config, or fallback to standard flags
PKG_CONFIG ?= pkg-config
PW_LIBS := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null || echo "-lpipewire-0.3")
PW_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null || echo "-I/usr/include/pipewire-0.3 -I/usr/include/spa-0.2")

PULSE_LIBS := $(shell $(PKG_CONFIG) --libs libpulse libpulse-simple 2>/dev/null || echo "-lpulse -lpulse-simple")
PULSE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpulse libpulse-simple 2>/dev/null || echo "")

ALSA_LIBS := $(shell $(PKG_CONFIG) --libs alsa 2>/dev/null || echo "-lasound")
ALSA_CFLAGS := $(shell $(PKG_CONFIG) --cflags alsa 2>/dev/null || echo "")

ALL_CFLAGS := $(CXXFLAGS) $(PW_CFLAGS) $(PULSE_CFLAGS) $(ALSA_CFLAGS)
ALL_LIBS := $(PW_LIBS) $(PULSE_LIBS) $(ALSA_LIBS) -pthread

SRCS = src/main.cpp \
       src/dsp.cpp \
       src/config.cpp \
       src/ipc.cpp \
       src/audio_backend.cpp \
       src/pipewire_backend.cpp \
       src/pulse_backend.cpp \
       src/alsa_backend.cpp \
       src/benchmark.cpp

OBJS = $(SRCS:.cpp=.o)
TARGET = bin/crossfeed

.PHONY: all clean install uninstall bench

all: $(TARGET)

$(TARGET): $(OBJS) | bin
	$(CXX) $(OBJS) $(ALL_LIBS) -o $@

%.o: %.cpp
	$(CXX) $(ALL_CFLAGS) -c $< -o $@

bin:
	mkdir -p bin

bench: $(TARGET)
	./$(TARGET) bench

clean:
	rm -f $(OBJS) $(TARGET)
	rm -rf bin

install: $(TARGET)
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/crossfeed

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/crossfeed
