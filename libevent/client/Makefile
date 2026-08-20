# Makefile - libevent 图像客户端（Linux，C++11）
# 用法：make       编译
#       make clean 清理

CC       = gcc
CXX      = g++
CFLAGS   = -O2 -Wall -I../common
CXXFLAGS = -O2 -Wall -std=c++11 -I../common
LDFLAGS  = -levent -levent_pthreads -lpthread

TARGET = client
SRC    = main.cpp client.cpp preview_window.cpp ../common/logger.cpp
HDR    = client.h preview_window.h ../common/protocol.h ../common/logger.h

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
