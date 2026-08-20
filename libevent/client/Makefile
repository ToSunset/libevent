# Makefile —— libevent 图像客户端（Linux）
# 用法：make       编译
#       make clean 清理

CC      = gcc
CFLAGS  = -O2 -Wall -I../common
LDFLAGS = -levent -levent_pthreads -lpthread

TARGET = client
SRC    = client.c
HDR    = ../common/protocol.h

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean