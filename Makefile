CC ?= cc
BASE_CFLAGS = -O2 -Wall -Wextra -Wpedantic -std=c11
CFLAGS ?= $(BASE_CFLAGS)

ifeq ($(OS),Windows_NT)
NAME = efnfc.exe
LDLIBS = -lwinscard
PREFIX ?= $(if $(MINGW_PREFIX),$(MINGW_PREFIX),/ucrt64)
else
NAME = efnfc
PCSC_CFLAGS := $(shell pkg-config --cflags libpcsclite 2>/dev/null)
PCSC_LIBS := $(shell pkg-config --libs libpcsclite 2>/dev/null)
CFLAGS += $(PCSC_CFLAGS)
LDLIBS = $(if $(PCSC_LIBS),$(PCSC_LIBS),-lpcsclite)
PREFIX ?= /usr/local
endif

BINDIR ?= $(PREFIX)/bin
SRC = efnfc.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean fclean re install

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ)

fclean: clean
	rm -f efnfc efnfc.exe

re: fclean all

install: all
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 755 $(NAME) "$(DESTDIR)$(BINDIR)/$(NAME)"
