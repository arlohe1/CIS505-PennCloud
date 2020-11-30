GCC=g++
RPC_DIR= ~/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

test:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	# $(GCC) test_client.cc backend.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o client $(LIBS)
	# $(GCC) master.cc master.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o master $(LIBS)
	
	
# mast:
#	$(GCC) master.cc master.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o master $(LIBS)
	
clean:
	rm -f kvServer
	#rm -f client
	#rm -f Checkpoint*
	#rm -f Serverlog*
	#rm -f master

