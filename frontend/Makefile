TARGETS = frontend

all: $(TARGETS)

frontend: frontend.cc
	g++ $^ -I/usr/local/opt/openssl/include -L/usr/local/opt/openssl/lib -lcrypto -lpthread -g -o $@

pack:
	rm -f submit-frontend.zip
	zip -r submit-frontend.zip *.cc Makefile

clean::
	rm -fv $(TARGETS) *~

realclean:: clean
	rm -fv cis505-frontend.zip
