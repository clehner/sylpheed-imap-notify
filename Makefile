NAME = imap-notify
LIB = $(NAME).so
PREFIX ?= /usr/local

CFLAGS += `pkg-config --cflags gtk+-2.0` -fPIC -g \
		  -I$(PREFIX)/include/sylpheed \
		  -I$(PREFIX)/include/sylpheed/sylph
LDFLAGS += `pkg-config --libs gtk+-2.0` -L$(PREFIX)/lib \
		   -lsylpheed-plugin-0 -lsylph-0

ifdef SYLPHEED_DIR
	CFLAGS += -I$(SYLPHEED_DIR)/libsylph \
			  -I$(SYLPHEED_DIR)/src
	LDFLAGS += -L$(SYLPHEED_DIR)/src/.libs \
			   -L$(SYLPHEED_DIR)/libsylph/.libs
endif

$(LIB): $(NAME).o
	$(CC) $(LDFLAGS) -shared $< -o $@

install: $(LIB)
	cp $(LIB) ~/.sylpheed-2.0/plugins/

clean:
	rm -f *.o $(LIB)

.PHONY: clean install
