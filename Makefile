CXX = g++

CXXFLAGS=-g -std=gnu++0x -Wl,--export-dynamic
LDFLAGS=-L/usr/local/lib -l:libpapi.so.5

ifeq ($(DEBUG),y)
	CXXFLAGS += -O0
else
	CXXFLAGS += -O3
endif

libeaudit.so: eaudit.o
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS)

test: test.cpp libeaudit.so
	$(CXX) $(CXXFLAGS) -o $@ test.cpp -L. -leaudit
	sudo setcap cap_sys_rawio=ep $@

eaudit.o: eaudit.cpp
	$(CXX) $(CXXFLAGS) -fPIC -c -o $@ $<

.PHONY: clean debug 

clean:
	-rm *.o test libeaudit.so

debug:
	$(MAKE) test DEBUG=y

