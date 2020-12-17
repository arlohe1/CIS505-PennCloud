GCC=g++
RPC_DIR= ~/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

test:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	#mkdir server_1 server_2 server_3
	mkdir server_1/checkpoint server_2/checkpoint server_3/checkpoint
	touch server_1/log.txt server_2/log.txt server_3/log.txt
kvs:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)

testClient: 
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	
serverfiles:
	#rm -r server_1/
	#rm -r server_2/
	#rm -r server_3/
	#mkdir server_1 server_2 server_3
	mkdir server_1/checkpoint server_2/checkpoint server_3/checkpoint
	touch server_1/log.txt server_2/log.txt server_3/log.txt
	
master:
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	
clean:
	rm -f kvServer
	rm -f masterNode
	: > server_1/log.txt # clear logfile
	: > server_2/log.txt # clear logfile
	: > server_3/log.txt # clear logfile
	rm -r server_1/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	rm -r server_2/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	rm -r server_3/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)


