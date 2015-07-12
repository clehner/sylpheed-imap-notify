NAME = imap-notify
LIB = $(NAME).so
PREFIX ?= /usr/local

CFLAGS += `pkg-config --cflags gtk+-2.0` -fPIC \
		  -I$(PREFIX)/include/sylpheed \
		  -I$(PREFIX)/include/sylpheed/sylph
LDFLAGS += `pkg-config --libs gtk+-2.0` -L$(PREFIX)/lib \
		   -lsylpheed-plugin-0 -lsylph-0

$(LIB): $(NAME).o
	$(CC) $(LDFLAGS) -shared $< -o $@

install: $(LIB)
	cp $(LIB) ~/.sylpheed-2.0/plugins/

clean:
	rm -f *.o $(LIB)

.PHONY: clean install
