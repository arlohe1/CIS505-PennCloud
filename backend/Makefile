TARGETS = kvServer

all: $(TARGETS)

kvServer: kvServer.cc 
	g++ $^ -lpthread -g -o $@
pack:
	rm -f submit-hw4.zip
	zip -r submit-hw4.zip *.cc README Makefile

clean::
	rm -fv $(TARGETS) *~

realclean:: clean
	rm -fv cis505-hw4.zip
