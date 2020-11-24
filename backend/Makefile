TARGETS = kvs_server

all: $(TARGETS)

kvs_server: kvs_server.cc 
	g++ $^ -lpthread -g -o $@
pack:
	rm -f submit-hw4.zip
	zip -r submit-hw4.zip *.cc README Makefile

clean::
	rm -fv $(TARGETS) *~

realclean:: clean
	rm -fv cis505-hw4.zip
