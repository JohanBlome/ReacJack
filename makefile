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
DAEMON_EXECUTABLE := reacjackd
CTL_EXECUTABLE := reacjackctl
TEST_EXECUTABLES := tests/test_reac_decode tests/test_shared_audio
PIPEWIRE_OBJECTS := $(SRC_DIR)/reacjack-pw.o $(SRC_DIR)/reac_decode.o
DAEMON_OBJECTS := $(SRC_DIR)/reacjackd.o $(SRC_DIR)/reac_decode.o \
	$(SRC_DIR)/shared_audio.o
CTL_OBJECTS := $(SRC_DIR)/reacjackctl.o $(SRC_DIR)/shared_audio.o

SHM_LIBS :=
ifeq ($(UNAME_S),Linux)
  SHM_LIBS += -lrt
endif
TEST_LIBS := -lm $(SHM_LIBS)

DRIVER_BUNDLE := ReacJack.driver
DRIVER_BINARY := $(DRIVER_BUNDLE)/Contents/MacOS/ReacJack
HAL_INSTALL_DIR := /Library/Audio/Plug-Ins/HAL

.PHONY: all clean install install-pipewire test pipewire driver test-driver \
	install-driver uninstall-driver

all: $(EXECUTABLE) $(CTL_EXECUTABLE)
ifeq ($(UNAME_S),Darwin)
all: $(DAEMON_EXECUTABLE) $(DRIVER_BUNDLE)
endif

ifeq ($(UNAME_S),Darwin)
driver: $(DRIVER_BUNDLE)

$(DRIVER_BUNDLE): $(DRIVER_BINARY) $(DRIVER_BUNDLE)/Contents/Info.plist
	codesign --force --sign - $@
	touch $@

$(DRIVER_BINARY): $(SRC_DIR)/coreaudio/ReacJackDriver.c
	mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -bundle $< -o $@ \
		-framework CoreAudio -framework CoreFoundation

$(DRIVER_BUNDLE)/Contents/Info.plist: $(SRC_DIR)/coreaudio/Info.plist
	mkdir -p $(@D)
	cp $< $@

test: test-driver
test-driver: $(DRIVER_BUNDLE) tests/test_hal_driver
	./tests/test_hal_driver

tests/test_hal_driver: tests/test_hal_driver.o
	$(CXX) $^ -o $@ -framework CoreAudio -framework CoreFoundation

install-driver: $(DRIVER_BUNDLE)
	sudo cp -R $(DRIVER_BUNDLE) $(HAL_INSTALL_DIR)/
	sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod

uninstall-driver:
	sudo rm -rf $(HAL_INSTALL_DIR)/$(DRIVER_BUNDLE)
	sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
endif

$(DAEMON_EXECUTABLE): $(DAEMON_OBJECTS)
	$(CXX) $^ -o $@ $(LDLIBS)

$(CTL_EXECUTABLE): $(CTL_OBJECTS)
	$(CXX) $^ -o $@ -lm $(SHM_LIBS)

$(EXECUTABLE): $(OBJECTS)
	$(CXX) $(OBJECTS) -o $@ $(LDLIBS) $(JACK_LIBS)

test: $(TEST_EXECUTABLES)
	./tests/test_reac_decode
	./tests/test_shared_audio

tests/test_reac_decode: tests/test_reac_decode.o $(SRC_DIR)/reac_decode.o
	$(CXX) $^ -o $@ $(TEST_LIBS)

tests/test_shared_audio: tests/test_shared_audio.o $(SRC_DIR)/shared_audio.o
	$(CXX) $^ -o $@ $(TEST_LIBS)

$(SRC_DIR)/shared_audio.o tests/test_shared_audio.o $(SRC_DIR)/reacjackd.o \
	$(SRC_DIR)/reacjackctl.o: $(SRC_DIR)/shared_audio.h

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
	rm -f $(SRC_DIR)/*.o tests/*.o $(EXECUTABLE) $(PIPEWIRE_EXECUTABLE) \
		$(DAEMON_EXECUTABLE) $(CTL_EXECUTABLE) $(TEST_EXECUTABLES) \
		tests/test_hal_driver
	rm -rf $(DRIVER_BUNDLE)
