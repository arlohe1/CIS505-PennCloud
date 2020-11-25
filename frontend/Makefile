TARGETS = frontend smtp

all: $(TARGETS)

frontend: frontend.cc
	g++ $^ -I/usr/local/opt/openssl/include -L/usr/local/opt/openssl/lib -lcrypto -lpthread -g -o $@
	
smtp: smtp.cc
	g++ $< -lpthread -g -o $@

pack:
	rm -f submit-frontend.zip
	zip -r submit-frontend.zip *.cc Makefile

clean::
	rm -fv $(TARGETS) *~

realclean:: clean
	rm -fv cis505-frontend.zip
