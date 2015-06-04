CXXFLAGS=-g -std=gnu++0x -Wall -Wextra -pthread -Isupereasyjson -I/home/eric/.local/include
LDFLAGS=-lpapi -lpthread -L/home/eric/.local/lib

ifeq ($(RELEASE),y)
	CXXFLAGS += -O3
else
	CXXFLAGS += -O0 -DDEBUG
endif

all: eaudit test eaudit-wrapper

eaudit: eaudit.o supereasyjson/json.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)
	sudo setcap cap_sys_rawio=ep $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: test.o
	$(CXX) -g -O0 -o $@ $^ -lpthread

test.o: test.cpp
	$(CXX) -g -O0 -c -o $@ $<

eaudit-wrapper: wrapper.o
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

.PHONY: clean release all

clean:
	-rm *.o supereasyjson/*.o eaudit test eaudit-wrapper

release:
	$(MAKE) RELEASE=y

