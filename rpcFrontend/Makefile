GCC=g++
RPC_DIR= ~/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

TARGETS = frontend smtp

all: $(TARGETS)

frontend: frontend.cc
	g++ $^ -I/usr/local/opt/openssl/include -L/usr/local/opt/openssl/lib -lcrypto -lpthread $(INCLUDES) -L $(RPC_DIR)/output/lib $(LIBS) -g -o $@

smtp: smtp.cc
	g++ $< -lpthread -g -o $@

pack:
	rm -f submit-frontend.zip
	zip -r submit-frontend.zip *.cc Makefile

clean::
	rm -fv $(TARGETS) *~

realclean:: clean
	rm -fv cis505-frontend.zip
