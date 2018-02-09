CC = gcc
CXX = g++
BASE_FLAGS = -O3 -g0 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable -Wno-char-subscripts
CFLAGS = $(BASE_FLAGS) -std=c99
CXXFLAGS = $(BASE_FLAGS) -std=c++11
LDFLAGS = $(CFLAGS) -pthread -lcrypto

dnsseed: dns.o bitcoin.o netbase.o protocol.o db.o main.o util.o
	$(CXX) -o dnsseed dns.o bitcoin.o netbase.o protocol.o db.o main.o util.o $(LDFLAGS)

%.o: %.cpp bitcoin.h netbase.h protocol.h db.h serialize.h uint256.h util.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

dns.o: dns.c
	$(CC) -std=c99 $(CFLAGS) dns.c -c -o dns.o

%.o: %.cpp
