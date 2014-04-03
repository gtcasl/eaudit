CXX = g++

EAFLAGS=-std=gnu++0x
CXXFLAGS=-g -Wl,--export-dynamic
LDFLAGS=-L/usr/local/lib -l:libpapi.so.5

ifeq ($(DEBUG),y)
	EAFLAGS += -DDEBUG
	CXXFLAGS += -O0
else
	CXXFLAGS += -O3
endif

TARGET=test

all: eaudit.o test.o
	$(CXX) $(CXXFLAGS) -o $(TARGET) $^ $(LDFLAGS)
	sudo setcap cap_sys_rawio=ep $(TARGET)

eaudit.o: eaudit.cpp
	$(CXX) $(EAFLAGS) $(CXXFLAGS) -c -o $@ $<

test.o: test.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

.PHONY: clean object debug 

object: eaudit.o

clean:
	-rm *.o $(TARGET)

debug:
	$(MAKE) DEBUG=y

