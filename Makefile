CXXFLAGS=-g -std=gnu++0x -Wall -Wextra -pthread
LDFLAGS=-lpapi -lpthread

ifeq ($(RELEASE),y)
	CXXFLAGS += -O3
else
	CXXFLAGS += -O0 -DDEBUG
endif

all: eaudit test

eaudit: eaudit.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	sudo setcap cap_sys_rawio=ep $@

eaudit.o: eaudit.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: test.o
	$(CXX) -g -O0 -o $@ $^ -lpthread

test.o: test.cpp
	$(CXX) -g -O0 -c -o $@ $<

.PHONY: clean debug 

clean:
	-rm *.o eaudit test

release:
	$(MAKE) RELEASE=y

