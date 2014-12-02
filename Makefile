.PHONY: all instrumented tracing

all: instrumented tracing

instrumented:
	$(MAKE) -C byfl-instrumented

tracing:
	$(MAKE) -C tracing
