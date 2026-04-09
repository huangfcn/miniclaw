CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -O3 -Wall
ARFLAGS  = rcs

LIB     = lib/libfiber.a
OBJS    = fiber.o context.o

.PHONY: all clean

all: $(LIB)

# --- static library ---
$(LIB): $(OBJS)
	$(AR) $(ARFLAGS) $@ $^

fiber.o: fiber.c
	$(CC) $(CFLAGS) -c $< -o $@

context.o: context.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(LIB) $(TEST) $(BENCHMARK)
