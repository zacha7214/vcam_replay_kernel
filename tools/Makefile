CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += -D_GNU_SOURCE

all: vcam_feed

vcam_feed: vcam_feed.c ../driver/vcam_uapi.h
	$(CC) $(CFLAGS) -o $@ vcam_feed.c

clean:
	rm -f vcam_feed
