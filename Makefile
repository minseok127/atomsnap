CC		= gcc
AR		= ar
RANLIB	= ranlib


CFLAGS_RELEASE	= -Wall -Wextra -O2 -std=c11 -fPIC
CFLAGS_DEBUG	= -Wall -Wextra -O0 -g -pg -std=c11 -fPIC

BUILD_MODE ?= release

ifeq ($(BUILD_MODE), release)
	CFLAGS = $(CFLAGS_RELEASE)
else ifeq ($(BUILD_MODE), debug)
	CFLAGS = $(CFLAGS_DEBUG)
else
	$(error Unknown BUILD_MODE: $(BUILD_MODE). Use 'release' or 'debug')
endif

STATIC_LIB = libatommv.a
SHARED_LIB = libatommv.so

all: $(STATIC_LIB) $(SHARED_LIB)

$(STATIC_LIB): atommv.o
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SHARED_LIB): atommv.o
	$(CC) -shared -o $@ $^

atommv.o: atommv.c atommv.h
	$(CC) $(CFLAGS) -c atommv.c

clean:
	rm -f *.o $(STATIC_LIB) $(SHARED_LIB)
