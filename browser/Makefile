CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic -std=c11
DESTDIR ?=

ifeq ($(OS),Windows_NT)
NAME = efrits-nfc-bridge.exe
LDLIBS = -lws2_32
PREFIX ?= $(if $(MINGW_PREFIX),$(MINGW_PREFIX),/ucrt64)
else
NAME = efrits-nfc-bridge
LDLIBS =
PREFIX ?= $(HOME)/.local
endif

BINDIR ?= $(PREFIX)/bin

SRC = efrits-nfc-bridge.c
OBJ = $(SRC:.c=.o)

.PHONY: all clean fclean re install

all: $(NAME)

$(NAME): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ)

fclean: clean
	rm -f efrits-nfc-bridge efrits-nfc-bridge.exe

re: fclean all

install: $(NAME)
	install -d "$(DESTDIR)$(BINDIR)"
	install -m 0755 $(NAME) "$(DESTDIR)$(BINDIR)/$(NAME)"
