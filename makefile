CC ?= cc
CXX ?= c++
PKG_CONFIG ?= pkg-config

UNAME_S := $(shell uname -s)

CPPFLAGS ?=
CFLAGS ?= -Wall -Wextra -O2 -std=gnu11
CXXFLAGS ?= -Wall -Wextra -O2 -std=c++11
LDLIBS ?= -lpthread -lm

JACK_CFLAGS := $(shell $(PKG_CONFIG) --cflags jack 2>/dev/null)
JACK_LIBS := $(shell $(PKG_CONFIG) --libs jack 2>/dev/null)
PIPEWIRE_CFLAGS := $(shell $(PKG_CONFIG) --cflags libpipewire-0.3 2>/dev/null)
PIPEWIRE_LIBS := $(shell $(PKG_CONFIG) --libs libpipewire-0.3 2>/dev/null)

ifeq ($(strip $(JACK_LIBS)),)
  JACK_LIBS := -ljack
endif

ifeq ($(UNAME_S),Darwin)
  LDLIBS += -lpcap
endif

ifeq ($(UNAME_S),Linux)
  CXXFLAGS += -mtune=native
endif

SRC_DIR := src

CORE_SOURCES := $(SRC_DIR)/reac_decode.c
SOURCES := $(SRC_DIR)/reacjack.c $(CORE_SOURCES)
OBJECTS := $(SOURCES:.c=.o)
EXECUTABLE := reacjack
PIPEWIRE_EXECUTABLE := reacjack-pw
TEST_EXECUTABLE := tests/test_reac_decode
TEST_OBJECTS := tests/test_reac_decode.o $(SRC_DIR)/reac_decode.o
PIPEWIRE_OBJECTS := $(SRC_DIR)/reacjack-pw.o $(SRC_DIR)/reac_decode.o

.PHONY: all clean install install-pipewire test pipewire

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDLIBS) $(JACK_LIBS)

test: $(TEST_EXECUTABLE)
	./$(TEST_EXECUTABLE)

$(TEST_EXECUTABLE): $(TEST_OBJECTS)
	$(CXX) $(TEST_OBJECTS) -o $@ -lm

ifneq ($(strip $(PIPEWIRE_LIBS)),)
pipewire: $(PIPEWIRE_EXECUTABLE)

$(PIPEWIRE_EXECUTABLE): $(PIPEWIRE_OBJECTS)
	$(CXX) $(PIPEWIRE_OBJECTS) -o $@ $(LDLIBS) $(PIPEWIRE_LIBS)

$(SRC_DIR)/reacjack-pw.o: $(SRC_DIR)/reacjack-pw.c $(SRC_DIR)/reac_decode.h
	$(CC) $(CPPFLAGS) $(PIPEWIRE_CFLAGS) $(CFLAGS) -c $< -o $@

install-pipewire: $(PIPEWIRE_EXECUTABLE)
	mkdir -p $(HOME)/bin
	cp $(PIPEWIRE_EXECUTABLE) $(HOME)/bin/
	sudo setcap cap_net_raw,cap_net_admin=eip $(HOME)/bin/$(PIPEWIRE_EXECUTABLE)
else
pipewire:
	@echo "PipeWire development files not found. Install libpipewire-0.3-dev/pipewire-devel."
	@false

install-pipewire: pipewire
endif

%.o: %.c $(SRC_DIR)/reac.h $(SRC_DIR)/reac_decode.h
	$(CXX) $(CPPFLAGS) $(JACK_CFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@

install: $(EXECUTABLE)
	mkdir -p $(HOME)/bin
	cp $(EXECUTABLE) $(HOME)/bin/
ifeq ($(UNAME_S),Linux)
	sudo setcap cap_net_raw,cap_net_admin=eip $(HOME)/bin/$(EXECUTABLE)
endif

clean:
	rm -f $(OBJECTS) $(TEST_OBJECTS) $(PIPEWIRE_OBJECTS) $(EXECUTABLE) \
		$(PIPEWIRE_EXECUTABLE) $(TEST_EXECUTABLE)
