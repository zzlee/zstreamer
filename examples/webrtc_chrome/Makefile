#=============================================================================
#   Makefile for independent build of the WebRTC Chrome example
#=============================================================================
CC = gcc
CFLAGS = -Wall -Wextra -O2 -I../../include -pthread
LDFLAGS = -L../../build -L../../build/src -lzstreamer -lzstreamer-elements -ldatachannel -lpthread

TARGET = webrtc_chrome_server
SRC = server.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
