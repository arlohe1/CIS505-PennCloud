GCC=g++
RPC_DIR= ~/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

test:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	mkdir server_1 server_2 server_3
	mkdir server_1/checkpoint server_2/checkpoint server_3/checkpoint
	touch server_1/log.txt server_2/log.txt server_3/log.txt
	
serverfiles:
	mkdir server_1 server_2 server_3
	mkdir server_1/checkpoint server_2/checkpoint server_3/checkpoint
	touch server_1/log.txt server_2/log.txt server_3/log.txt
	
master:
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	
clean:
	rm -f kvServer
	rm -f masterNode
	#rm -r server_1/log.txt
	rm -r server_1/
	rm -r server_2/
	rm -r server_3/


