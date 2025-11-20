# # Makefile para cliente FTP 
CC = cc
CFLAGS = -Wall -Wextra -g -O2

SRCS = TCPftp.c connectsock.c connectTCP.c passivesock.c passiveTCP.c errexit.c
OBJS = $(SRCS:.c=.o)
TARGET = TCPftp

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) *~ core

