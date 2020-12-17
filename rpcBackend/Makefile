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
kvs:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	
serverfiles:
	rm -r server_1/
	rm -r server_2/
	rm -r server_3/
	#mkdir server_1 server_2 server_3
	mkdir server_1/checkpoint server_2/checkpoint server_3/checkpoint
	#touch server_1/log.txt server_2/log.txt server_3/log.txt
	
master:
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	
clean:
	rm -f kvServer
	rm -f masterNode
	rm -f server_1/checkpointNum.txt
	rm -f server_2/checkpointNum.txt
	rm -f server_3/checkpointNum.txt
	: > server_1/log.txt # clear logfile
	: > server_2/log.txt # clear logfile
	: > server_3/log.txt # clear logfile
	rm -r server_1/logArchive
	rm -r server_2/logArchive
	rm -r server_3/logArchive
	mkdir server_1/logArchive server_2/logArchive server_3/logArchive
	rm -r server_1/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	rm -r server_2/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
	rm -r server_3/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)


