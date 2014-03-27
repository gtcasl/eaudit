BF_CXX = bf-g++-4.7
CXX = g++-4.7

EAFLAGS=-std=gnu++11 -I/home/eric/byfl/lib/include
CXXFLAGS=-O2
LDFLAGS=-L/usr/local/lib -l:libpapi.so.5

ifeq ($(DEBUG),y)
	EAFLAGS += -DDEBUG
	CXXFLAGS = -g -O0
endif

ifeq ($(RECORDALL),y)
	EAFLAGS += -DEAUDIT_RECORD_ALL
endif

TARGET=test

all: eaudit.o test.o
	$(BF_CXX) $(CXXFLAGS) -o $(TARGET) $^ $(LDFLAGS)
	sudo setcap cap_sys_rawio=ep $(TARGET)

eaudit.o: eaudit.cpp eaudit.h
	$(CXX) $(EAFLAGS) $(CXXFLAGS) -c -o $@ $<

test.o: test.cpp
	$(BF_CXX) $(CXXFLAGS) -c -o $@ $<

.PHONY: clean object recordall debug drecordall

object: eaudit.o

clean:
	-rm *.o $(TARGET)

recordall:
	$(MAKE) RECORDALL=y

debug:
	$(MAKE) DEBUG=y

drecordall:
	$(MAKE) RECORDALL=y DEBUG=y
