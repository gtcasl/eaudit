CXX = g++

EAFLAGS=-std=gnu++0x
CXXFLAGS=-g -Wl,--export-dynamic -Wall
LDFLAGS=-L/usr/local/lib -l:libpapi.so.5 -lbfd

ifeq ($(DEBUG),y)
	EAFLAGS += -DDEBUG
	CXXFLAGS += -O0
else
	CXXFLAGS += -O3
endif

all: eaudit test

eaudit: eaudit.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	sudo setcap cap_sys_rawio=ep $@

eaudit.o: eaudit.cpp
	$(CXX) $(EAFLAGS) $(CXXFLAGS) -c -o $@ $<

test: test.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) -lpthread

test.o: test.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

.PHONY: clean debug 

clean:
	-rm *.o eaudit test

debug:
	$(MAKE) DEBUG=y

