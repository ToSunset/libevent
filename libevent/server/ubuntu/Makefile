# Makefile - libevent 图像服务器（Linux，C++11，含浏览器界面）
# 用法：make        编译
#       make clean 清理

CC       = gcc
CXX      = g++
CFLAGS   = -O2 -Wall -I../common
CXXFLAGS = -O2 -Wall -std=c++11 -I../common
LDFLAGS  = -levent -levent_pthreads -lpthread

TARGET = server
CXXSRC = ../main.cpp ../server.cpp ../image_source.cpp ../client_session.cpp ../client_manager.cpp ../web_server.cpp ../png_encoder.cpp ../common/logger.cpp
CSRC   = ../mongoose.c
HDR    = ../server.h ../image_source.h ../client_session.h ../client_manager.h ../web_server.h ../png_encoder.h ../common/protocol.h ../common/logger.h ../mongoose.h

all: $(TARGET) index.html

$(TARGET): $(CXXSRC) mongoose.o $(HDR)
	$(CXX) $(CXXFLAGS) -o $@ $(CXXSRC) mongoose.o $(LDFLAGS)

mongoose.o: $(CSRC) ../mongoose.h
	$(CC) $(CFLAGS) -c -o $@ $(CSRC)

index.html: ../index.html
	cp -f $< $@

clean:
	rm -f $(TARGET) mongoose.o index.html

.PHONY: all clean
