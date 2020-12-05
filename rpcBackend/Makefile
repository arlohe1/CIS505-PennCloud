GCC=g++
RPC_DIR= ~/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

test:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	# $(GCC) test_client.cc backend.h $(INCLUDES) -L $(RPC_DIR)/output/lib -o client $(LIBS)
	
	
master:
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	
clean:
	rm -f kvServer
	rm -f masterNode
	rm -r server_1/log.txt
	#rm -f client
	#rm -f Checkpoint*
	#rm -f Serverlog*
	#rm -f master

