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
	#rm -r server_1/log.txt
	: > server_1/log.txt # cleares logfile
	rm -r server_1/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)

