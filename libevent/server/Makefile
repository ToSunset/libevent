# Makefile - libevent 图像服务器（Linux，C++17，含浏览器界面）
# 用法：make        编译
#       make clean 清理

CC       = gcc
CXX      = g++
CFLAGS   = -O2 -Wall -I../common
CXXFLAGS = -O2 -Wall -std=c++17 -I../common
LDFLAGS  = -levent -levent_pthreads -lpthread

TARGET = server
CXXSRC = main.cpp server.cpp image_source.cpp client_session.cpp client_manager.cpp web_server.cpp png_encoder.cpp ../common/logger.cpp
CSRC   = mongoose.c
HDR    = server.hpp image_source.hpp client_session.hpp client_manager.hpp web_server.hpp png_encoder.hpp ../common/protocol.hpp ../common/logger.hpp mongoose.h

all: $(TARGET)

$(TARGET): $(CXXSRC) mongoose.o $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(CXXSRC) mongoose.o $(LDFLAGS)

mongoose.o: $(CSRC) mongoose.h
	$(CC) $(CFLAGS) -c -o $@ $(CSRC)

clean:
	rm -f $(TARGET) mongoose.o

.PHONY: all clean
