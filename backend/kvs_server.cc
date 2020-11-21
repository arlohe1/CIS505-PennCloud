#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>


#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <signal.h>

#include <pthread.h>
#include <fcntl.h>
#include<signal.h> 
#include <dirent.h>
#include <time.h>
#include <sys/file.h>

#include <map>
#include<iostream>
#include<regex>
#include<algorithm>
#include <chrono>
#include <ctime>


void debugTime() {
	if (debugFlag) {
		std::chrono::time_point<std::chrono::system_clock> now = std::chrono::system_clock::now();
		auto duration = now.time_since_epoch();

		typedef std::chrono::duration<int, std::ratio_multiply<std::chrono::hours::period, std::ratio<8>
		>::type> Days; 

		Days days = std::chrono::duration_cast<Days>(duration);
		    duration -= days;
		auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
		    duration -= hours;
		auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
		    duration -= minutes;
		auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
		    duration -= seconds;
		auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration);
		    duration -= microseconds;

		std::cout << hours.count() << ":"
		          << minutes.count() << ":"
		          << seconds.count() << "."
		          << microseconds.count() << " S"
		          << serverIndx << " " << std::flush;
	}
	
}

//needs atleast 2 args always
#define debugDetailed(fmt, ...) \
	do {if (debugFlag) {debugTime(); fprintf(stdout, "%s:%d:%s(): " fmt, __FILE__, __LINE__, __func__, __VA_ARGS__);} } while (0)

#define debug(fmt, ...) \
	do {if (debugFlag) fprintf(stderr, fmt, __VA_ARGS__); } while (0)



int main(int argc, char *argv[]) {
	// parse config for port and ip
	if (argc < 2) {
		fprintf(stderr, "*** Author: Liana Patel (lianap)\n");
		exit(1);
	}

  	// parse arguments -v for debug output -o for ordering
	int opt;
	while ((opt = getopt(argc, argv, "o:v")) != -1) {
		switch(opt) {
			case 'v':
				debugFlag = 1;
				debug("%s\n", "DEBUG is on");
				//TODO - add time
				break;
			case 'o':
				if (strcmp(optarg, "unordered") == 0) {

				} else if (strcmp(optarg, "fifo") == 0) {
					ordering = FIFO;
				} else if (strcmp(optarg, "total") == 0) {
					ordering = TOTAL;
				} else {
					fprintf(stderr, "invalid ordering, try: unordered, fifo, or total\n");
					exit(1);
				}
				break;
		}
	}

	// parse index and config file arguments
	if (argv[optind] == NULL) {
 		fprintf(stderr, "incorrect usage: try ./chatserver (config file) (server indx)\n");
		exit(1);
 	} else {
 		configFile = argv[optind];
 		optind++;
 	}

 	if (argv[optind] == NULL) {
 		fprintf(stderr, "incorrect usage: try ./chatserver (config file) (server indx)\n");
		exit(1);
 	} else {
 		serverIndx = atoi(argv[optind]);
 	}

 	// get forwarding address and bind address
 	char** res = getAddrs(serverIndx, (char*) configFile, addrs, ips, ports);
 	if (res == NULL) {
 		fprintf(stderr, "invalid config file or server index\n");
		exit(1);
 	}

	// bind to server socket
	int sock = socket(PF_INET, SOCK_DGRAM, 0);
	mySocket = sock; // set global fd for ^C handler use
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	//servaddr.sin_addr.s_addr = htons(INADDR_ANY);
	inet_pton(AF_INET, ipMap.at(serverIndx).c_str(), &(servaddr.sin_addr));
	servaddr.sin_port = htons(portMap.at(serverIndx));
	bind(sock, (struct sockaddr*)&servaddr, sizeof(servaddr));


	while (true) {
		struct sockaddr_in src;
		socklen_t srclen = sizeof(src);
		// receive message and null terminate 
		char buf[MESSAGE_SIZE];
		int rlen = recvfrom(sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &srclen);
		buf[rlen] = 0;
		// check address of message received
		debugDetailed("recieved from client addr: %s:%d\n", inet_ntoa(src.sin_addr), ntohs(src.sin_port));
		//debugDetailed("recieved from client addr: %d\n", ntohs(src.sin_port));
		// check source of message and case accordingly
		std::string str(inet_ntoa(src.sin_addr));
		str.append(":");
		str.append(std::to_string(ntohs(src.sin_port)));
		

		if (serverAddrs.count(str) > 0) {
			if(addrMap[serverIndx].compare(str) == 0) {
				debugDetailed("%s\n", "message came from me");
			} else {
				debugDetailed("%s: %s\n", "message came from server", buf);
			}	
			if (isQuit(buf)) {
				// check if quit command "/"
				sendToAllClients(mySocket, (char*) quitCommand);
				exit(0);
			} else {
				handleServerMessage(sock, buf, rlen);
			}
			
		} else {
			if (clients.count(str) < 1) {
				//new client - update client data structures
				debugDetailed("%s\n", "message came from new client");
				clients[str] = str;
				clientRooms[str] = -1;

			} else {
				// existing client
				debugDetailed("%s\n", "message came from existing client");
			}

			if (isCommand(buf) == 1) {
				debugDetailed("%s\n", "client command");
				handleCommand(sock, buf, strlen(buf) + 1, str);
			} else {
				if (clientRooms[str] == -1) {
					sendToClient(sock, (char*) "-ERR you have not joined a chat room", str);
				} else {
					debugDetailed("client message from client %s\n", str.c_str());
					handleClientMessage(sock, buf, str); 
				}
				
			}
		} 
	}




  return 0;
}  



