# Detect OS.
UNAME_S := $(shell uname -s)

CC = clang
CFLAGS = -Wall -W -O2
LDFLAGS =

# --- macOS ---
ifeq ($(UNAME_S),Darwin)
	LDFLAGS += -lobjc -framework Foundation -framework AVFoundation -framework CoreMedia -framework CoreVideo
endif

# --- TODO: Linux ---
ifeq ($(UNAME_S),Linux)
	# LDFLAGS += ...
endif

all: picturephone

picturephone: picturephone.c
	$(CC) $(CFLAGS) -o picturephone picturephone.c $(LDFLAGS)

clean:
	rm -f picturephone
