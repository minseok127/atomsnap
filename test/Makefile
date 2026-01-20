CC		?= gcc
CFLAGS		?= -std=c11 -O2 -Wall -Wextra -Werror
CFLAGS		+= -pthread -I..
LDFLAGS		?=
LDLIBS		?=

TARGET		:= wraparound_test
SRCS		:= wraparound_test.c
OBJS		:= $(SRCS:.c=.o)

# Set to 1 to ignore "double finalize" duplicates (debug convenience).
DISABLE_FINALIZE_CHECK ?= 0

ifeq ($(DISABLE_FINALIZE_CHECK),1)
CFLAGS		+= -DATOMSNAP_DISABLE_FINALIZE_CHECK
endif

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET) $(OBJS)

