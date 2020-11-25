# TARGETS = kvServer

# all: $(TARGETS)

# kvServer: kvServer.cc 
# 	g++ $^ -lpthread -g -o $@
# pack:
# 	rm -f submit-hw4.zip
# 	zip -r submit-hw4.zip *.cc README Makefile

# clean::
# 	rm -fv $(TARGETS) *~

# realclean:: clean
# 	rm -fv cis505-hw4.zip
GCC=g++
RPC_DIR= ~/git/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

test:
	$(GCC) kvServer.cc $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	#$(GCC) backend.cc backend.h kvstore.cc kvstore.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o server $(LIBS)
	#$(GCC) test_client.cc backend.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o client $(LIBS)
	#$(GCC) master.cc master.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o master $(LIBS)
	
	
#mast:
#	$(GCC) master.cc master.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o master $(LIBS)
	
clean:
	rm -f kvServer
	#rm -f server
	#rm -f client
	#rm -f Checkpoint*
	#rm -f Serverlog*
	#rm -f master
