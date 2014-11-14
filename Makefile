CXXFLAGS=-g -std=gnu++0x -Wall -Wextra -pthread
LDFLAGS=-lpapi -lpthread

ifeq ($(DEBUG),y)
	CXXFLAGS += -O0 -DDEBUG
else
	CXXFLAGS += -O3
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

debug:
	$(MAKE) DEBUG=y

