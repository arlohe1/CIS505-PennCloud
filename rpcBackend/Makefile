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
	: > server_1/log.txt # cleares logfile
	: > server_2/log.txt # cleares logfile
	: > server_3/log.txt # cleares logfile
	rm -r server_1/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	rm -r server_2/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	rm -r server_3/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	#find  . -wholename 'server_*/log.txt' -exec rm -r {} \;


