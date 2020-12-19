# to run:
	# Master node (1): ./masterNode -v configFile
	# backend servers (6): ./kvServer -v configFile <server num in 1..6>
	# frontend servers: ./frontend -v -p 5000 -k 127.0.0.1:8000
	# open broswer on 127.0.0.1:5000



GCC=g++
RPC_DIR= ~/rpclib
INCLUDES = -I $(RPC_DIR)/output/include -I $(RPC_DIR)/lib -I $(RPC_DIR)
LIBS= -lrpc -lpthread

test:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)

kvs:
	$(GCC) kvServer.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o kvServer $(LIBS)
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)

testClient: 
	$(GCC) testClient.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o testClient $(LIBS)
	
master:
	$(GCC) masterNode.cc  $(INCLUDES) -L $(RPC_DIR)/output/lib -o masterNode $(LIBS)
	
serverfiles:
	for i in 1 2 3 4 5 6; do \
		rm -r server_$$i; \
		mkdir server_$$i; \
		mkdir server_$$i/checkpoint; \
		mkdir server_$$i/logArchive; \
		touch server_$$i/log.txt; \
	done
	
	
clean:
	rm -f kvServer
	rm -f masterNode
	for i in 1 2 3 4 5 6; do \
		rm -f server_$$i/checkpointNum.txt; \
		: > server_$$i/log.txt; \
		rm -r server_$$i/logArchive; \
		mkdir server_$$i/logArchive; \
		rm -r server_$$i/checkpoint/*; \
	done

# 	rm -f server_1/checkpointNum.txt
# 	rm -f server_2/checkpointNum.txt
# 	rm -f server_3/checkpointNum.txt
# 	: > server_1/log.txt # clear logfile
# 	: > server_2/log.txt
# 	: > server_3/log.txt
# 	rm -r server_1/logArchive
# 	rm -r server_2/logArchive
# 	rm -r server_3/logArchive
# 	mkdir server_1/logArchive server_2/logArchive server_3/logArchive
# 	rm -r server_1/checkpoint/* # empties checkpoint directory (add -r for removing folders in checkpoint)
# 	rm -r server_2/checkpoint/*
# 	rm -r server_3/checkpoint/*


