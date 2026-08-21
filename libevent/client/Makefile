# Makefile - libevent 图像客户端（Linux，C++17）
# 用法：make       编译
#       make clean 清理

CC       = gcc
CXX      = g++
CFLAGS   = -O2 -Wall -I../common
CXXFLAGS = -O2 -Wall -std=c++17 -I../common
LDFLAGS  = -levent -levent_pthreads -lpthread

TARGET = client
SRC    = main.cpp client.cpp preview_window.cpp ../common/logger.cpp
HDR    = client.hpp preview_window.hpp ../common/protocol.hpp ../common/logger.hpp

all: $(TARGET)

$(TARGET): $(SRC) $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(SRC) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
