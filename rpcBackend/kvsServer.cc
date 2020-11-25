#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include "rpc/server.h"


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

#define QUEUE_LENGTH 100
#define RECV_BUFFER_SIZE 1000

struct thread_info {
	int comm_fd;
	int indx;
};

int debugFlag;
int err = -1;
int detailed = 0;
volatile int fdArr[QUEUE_LENGTH]; //updated when connectons closed
struct thread_info* volatile tinfo_arr[QUEUE_LENGTH]; //note: should be able to make this non-volatile where only main thread accesses it
volatile int shuttingDown = 0;
pthread_t thread_ids[QUEUE_LENGTH]; //NOTE: not updated when threads exit, need to refer to fdArr to see whether index is active
pthread_t dispatcherThreadPid;
int socketFD = -1;
std::map<std::string, std::map<std::string, std::string>> kvMap; // maps server index to ip addr
int serverIndx = 1;

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




/* sets up signal handler given @signum and @handler and prints error if necessary */
void makeSignal(int signum, sighandler_t handler) {
	if (signal(signum, handler) == SIG_ERR) {
		perror("invalid: ");
	}
}

// memset for volatile int array with length len
void volatileMemset(volatile int* start, char c, int len) {
	int i;
	for (i = 0; i < len; i++) {
		start[i] = c;
	}
	
}

// returns 1 if caller is a worker thread and 0 if caller is main function
int isWorkerThread() {
	pthread_t self = pthread_self();
	if (self == dispatcherThreadPid) {
		return 0;
	} else {
		return 1;
	}
}

void freeAndCloseAll() {
	int i;
	for (i = 0; i < QUEUE_LENGTH; i++) {
		return;
	}
	return;
}

void signalHandler(int sig) {
	int i;
	int opts;
	const char* exit_message = "-ERR Server shutting down\n";

	if (sig == SIGUSR1) {
		debugDetailed("%s\n", "worker thread received sigusr1");
		pthread_exit(0);
	}
	if (sig == SIGINT) {
		debugDetailed("%s\n", "entiering sigint case");
		shuttingDown = 1;
		// if sigint caught by worker thread, simply relay to dispatcher thread and returns
		if (isWorkerThread() == 1) {
			debugDetailed("%s\n", "signal caught by worker thread");
			debugDetailed("dispather pid %ld\n", dispatcherThreadPid);
			pthread_kill(dispatcherThreadPid, SIGINT);
			return;
		} 
		debugDetailed("%s\n", "signal caught by main thread");
		for (i = 0; i < QUEUE_LENGTH; i++) {
			if (fdArr[i] != 0) {
				debugDetailed("indx with open fd: %d\n", i);
				//write goodbye message to fd and close fd
				write(fdArr[i], exit_message, strlen(exit_message));
				close(fdArr[i]);
				if (tinfo_arr[i] != NULL) {
					debugDetailed("freeing index %d\n", i);
					free(tinfo_arr[i]);
					tinfo_arr[i] = NULL;
				} else {
					debugDetailed("singal ^C handler - tinfo null indx %d\n", i);
				}
				// send signal SIGUSR1 to all threads and wait on them
				debugDetailed("Killing thread with id %d\n", (int)thread_ids[i]);
				int* retval;
				if (pthread_kill(thread_ids[i], SIGUSR1) != 0) {
					perror("invalid: ");
					exit(-1);
				}
				pthread_join(thread_ids[i], (void**) &retval);		
			}
		}
		debugDetailed("%s\n", "parent exiting");
		//close(socketFD);
		exit(0);
		
	}
	
}

// returns next nonzero indx of array, where array has length arr_len
int nextIndex(volatile int* arr, int arr_len) {
	int i;
	for (i = 0; i < arr_len; i++) {
		if (arr[i] == 0) {
			return i;
		}
	}
	return -1;
}

// // returns -1 if no clrf or offset to first character of \r\n
// int containsCrlf(char* buf, int len) {
// 	int i;
// 	for (i = 0; i < len - 1; i++) {
// 		if (buf[i] == '\r') {
// 			if (buf[i+1] == '\n') {
// 				return i;
// 			}
// 		}
// 	}
// 	return -1;
// }

// returns -1 if no comma and 0 if contains comma
int isCommandHeader(char* buf, int len) {
	int i;
	for (i = 0; i < len; i++) {
		if (buf[i] == ',') {
			return 0;
		}
	}
	return -1;
}

// detaches worker, performs cleanup, and exits thread
void workerCleanupAndExit(int threadIndx) {
	pthread_detach(thread_ids[threadIndx]);
	if (tinfo_arr[threadIndx] != NULL) {
		free(tinfo_arr[threadIndx]);
		tinfo_arr[threadIndx] = NULL;
	}	
	close(fdArr[threadIndx]);
	fdArr[threadIndx] = 0;
	pthread_exit(0);
}

void printKvMap() {
	if (debugFlag == 1) {
		std::cout << "kvmap print: \n";
		for (const auto& x : kvMap) {
			//x.first is row, x.second is column -> value
			std::cout << "\t" << "row: " << x.first << "\n";
			for (const auto& y : x.second) {
				//y.first is column, y.second is value
				std::cout << "\t\t" << "column: " << y.first << ", value: " << y.second << "\n";
			}		
	    }
	    std::cout << std::flush;
	}
	
}


//TODO fix memleak
std::string readFromSocket(int len, int comm_fd) {
	char* buffer = (char*) calloc(len + 1, sizeof(char));
	int bytesToRead = len;
	int ptr = 0;
	int result;

	debugDetailed("------number bytes to read: %d\n", bytesToRead);
	debugDetailed("------buffer is now: %s, ptr: %d\n", buffer, ptr);

	while((bytesToRead > 0) && (result = read(comm_fd, &buffer[ptr], bytesToRead)) > 0) {
		ptr = ptr + result;
		bytesToRead = bytesToRead - result;
		debugDetailed("------number bytes read: %d\n", result);
		debugDetailed("------number bytes to read: %d\n", bytesToRead);
		debugDetailed("------buffer is now: %s, ptr: %d\n", buffer, ptr);
	}

	if (bytesToRead > 0) {
		debugDetailed("%s\n", "error in length arg");
		write(comm_fd, "-ERR 14,bad length arg", strlen("-ERR 14,bad length arg"));
	}

	std::string ret(buffer);
	free(buffer);
	debugDetailed("string of args is %s", ret.c_str());
	return ret;

}

int put(int len, int comm_fd) {
	std::string args = readFromSocket(len, comm_fd);
	char* buf = (char*) args.c_str();
	char* row = strtok(buf, ",");
	char* col = strtok(NULL, ",");
	char* val = strtok(NULL, ",");
	debugDetailed("---put row: %s, column: %s, val: %s\n", row, col, val);
	std::string rowString(row);
	std::string colString(col);
	std::string valString(val);
	kvMap[rowString][colString] = valString;
	printKvMap();
	write(comm_fd, "+OK 0,", strlen("+OK 0,"));
	return 0;
}

int get(int len, int comm_fd) {
	std::string args = readFromSocket(len, comm_fd);
	char* buf = (char*) args.c_str();
	char* row = strtok(buf, ",");
	char* col = strtok(NULL, ",");
	std::string rowString(row);
	std::string colString(col);
	
	//char* err_message = (char*) "-ERR 13,no such value";
	char* err_message = (char*) "-ERR 0,";
	if (kvMap.count(rowString) > 0) {
		if (kvMap[rowString].count(colString) > 0) {
			std::string valString = kvMap[rowString][colString];
			std::string lengthParam = std::to_string(strlen(valString.c_str()));
			debugDetailed("---get row: %s, column: %s, val: %s\n", row, col, valString.c_str());
			write(comm_fd, "+OK ", strlen("+OK "));
			write(comm_fd, lengthParam.c_str(), strlen(lengthParam.c_str()));
			write(comm_fd, ",", strlen(","));
			write(comm_fd, valString.c_str(), strlen(valString.c_str()));
			return 0;
		}
	} 
	debugDetailed("---get row: %s, column: %s, val: not found\n", row, col);
	printKvMap();
	write(comm_fd, err_message, strlen(err_message));
	return 0;
}


int cput(int len, int comm_fd) {
	std::string args = readFromSocket(len, comm_fd);
	char* buf = (char*) args.c_str();
	char* row = strtok(buf, ",");
	char* col = strtok(NULL, ",");
	char* val = strtok(NULL, ",");
	char* newval = strtok(NULL, ",");
	std::string rowString(row);
	std::string colString(col);
	std::string valString(val);
	std::string newvalString(newval);

	
	if (kvMap.count(rowString) > 0) {
		if (kvMap[rowString].count(colString) > 0) {
			//check if row, col is in map
			if (strcmp(val, kvMap[rowString][colString].c_str()) == 0) {
				kvMap[rowString][colString] = newvalString;
				debugDetailed("---cput update row: %s, column: %s, old val: %s, new val: %s\n", row, col, val, newval);
				write(comm_fd, "+OK 7,updated", strlen("+OK 7,updated"));
				return 0;
			} 
			
		}
	} 
	
	write(comm_fd, "+OK 11,not updated", strlen("+OK 11,not updated"));
	return 0;
}

int del(int len, int comm_fd) {
	std::string args = readFromSocket(len, comm_fd);
	char* buf = (char*) args.c_str();
	char* row = strtok(buf, ",");
	char* col = strtok(NULL, ",");
	std::string rowString(row);
	std::string colString(col);

	//char* err_message = (char*) "-ERR 13,no such value";
	char* err_message = (char*) "-ERR 0,";
	if (kvMap.count(rowString) > 0) {
		if (kvMap[rowString].count(colString) > 0) {
			kvMap[rowString].erase(colString);
			debugDetailed("---delete deleted row: %s, column: %s\n", row, col);
			write(comm_fd, "+OK 0,", strlen("+OK 0,"));
			printKvMap();
			return 0;
		}
	} 
	debugDetailed("---delete row: %s, column: %s, val: not found\n", row, col);
	printKvMap();
	write(comm_fd, err_message, strlen(err_message));
	return 0;


	kvMap[rowString].erase(col);
	write(comm_fd, err_message, strlen(err_message));
	printKvMap();
	return 0;
}

int getArgLength(char* buf) {
	char* lenString = strtok(buf, ",");
	int len = atoi(lenString);
	debugDetailed("---getArgLength returns: %d\n", len);
	return len;
}



// parses and executes command in buf and writes output to comm_fd
// updates fdArr if client quits, and exits thread using workerCleanupAndExit
int parseCommand(char* buf, int len, int comm_fd, struct thread_info* tinfo) {
	const char* error_message = "-ERR Unknown command\r\n";
	const char* quit_message = "+OK Goodbye!\r\n";
	const char* echo_message = "+OK ";
	int thread_indx = tinfo->indx;
	int argLength;
	if (len < 4) {
		write(comm_fd, error_message, strlen(error_message));
		return 0;
	} else {
		if (strncasecmp("PUT ", buf, 4) == 0) {
			debugDetailed("%s\n", "put command found");
			argLength = getArgLength(&buf[4]);
			put(argLength, comm_fd);
		} else if (strncasecmp("GET ", buf, 4) == 0) {
			debugDetailed("%s\n", "get command found");
			argLength = getArgLength(&buf[4]);
			get(argLength, comm_fd);
		} else if (strncasecmp("CPUT ", buf, 5) == 0) {
			debugDetailed("%s\n", "cput command found");
			argLength = getArgLength(&buf[5]);
			cput(argLength, comm_fd);
		} else if (strncasecmp("DELETE ", buf, 7) == 0) {
			debugDetailed("%s\n", "delete command found");
			argLength = getArgLength(&buf[7]);
			del(argLength, comm_fd);
		} else if (strncasecmp("QUIT", buf, 4) == 0) {
			write(comm_fd, quit_message, strlen(quit_message));
			debug("[%d] Connection closed\n", comm_fd);	
			workerCleanupAndExit(thread_indx);
		} else {
			write(comm_fd, error_message, strlen(error_message));
		}
		return 0;
	}

}

//assumer command req prefix (eg PUT <length>,) is at most 10000
// worker thread reads from client and executes commands until client closes connection
static void* tExecute(void *arg) {
	struct thread_info* tinfo = (struct thread_info*) arg;
	int comm_fd = tinfo->comm_fd;
	int thread_indx = tinfo->indx;

	int bytesToRead = RECV_BUFFER_SIZE;
	int totalRead = 0;
	int result = 1;
	char buffer[RECV_BUFFER_SIZE];
	char command[RECV_BUFFER_SIZE];
    memset(&buffer, 0, sizeof(char) * RECV_BUFFER_SIZE);
    //char* ptr = &buffer[0];
    int ptr = 0;
    int indx = -1;

    // keep reading up to 1000 total bytes or until fd is closed by client
	while((bytesToRead > 0) && (result = read(comm_fd, &buffer[ptr], 1)) > 0) {
	
		debugDetailed("number bytes read: %d\n", result);
		debugDetailed("buffer is now: %s, ptr: %d\n", buffer, ptr);
		ptr = ptr + result;
		bytesToRead = bytesToRead - result;
		totalRead = totalRead + result;
		if (isCommandHeader(buffer, totalRead) == 0) {
			debugDetailed("%s\n", "header found");
			parseCommand(buffer, totalRead, comm_fd, tinfo);
			memset(&buffer, 0, sizeof(char) * RECV_BUFFER_SIZE);
			//ptr = &buffer[0];
			ptr = 0;
			bytesToRead = RECV_BUFFER_SIZE;
			totalRead = 0;
		}
		
		// while ((indx = isCommandHeader(buffer, totalRead)) != -1) {
		// 	//copy command from buffer to command array and update pointer and count variables
		// 	memset(&command, 0, sizeof(char) * RECV_BUFFER_SIZE);
		// 	strncpy(command, buffer, indx);
		// 	//int numCharsRemainingInBuffer = totalRead - (indx + 2);
		// 	//numCharsRemainingInBuffer = 0;
		// 	//strncpy(buffer, &buffer[indx + 2], numCharsRemainingInBuffer);
		// 	ptr = &buffer[numCharsRemainingInBuffer];
		// 	totalRead = numCharsRemainingInBuffer;
		// 	bytesToRead = RECV_BUFFER_SIZE - totalRead;
		// 	memset(ptr, 0, sizeof(char) * bytesToRead);
		// 	//execute command
		// 	debug("[%d] C: %s\n", comm_fd, command);
		// 	debug("[%d] C buffer: %s\n", comm_fd, buffer);
		// 	command[indx] = '\0';
		// 	parseCommand(command, indx, comm_fd, tinfo);
		// }
	}
	if (result < 0) {
		debugDetailed("%s","thread exiting\n");
		workerCleanupAndExit(thread_indx);
		pthread_exit(&err);
	}

	//close connection and exit
	debugDetailed("[%d] %s\n", comm_fd, "client closed connection");	
	workerCleanupAndExit(thread_indx);
	pthread_exit(&err); // this shouldnt be reached if workerClenaupandExit succeeds
}



int main(int argc, char *argv[]) {	
	int opt;
	int port = 10000;
	debugFlag = 0;

	///int socketfd = -1;
	struct thread_info* tinfo = NULL;
	
	makeSignal(SIGINT, signalHandler);
	makeSignal(SIGUSR1, signalHandler);
	volatileMemset(&fdArr[0], 0, QUEUE_LENGTH);
	memset(thread_ids, 0, sizeof(int) * QUEUE_LENGTH);

	// sets global variable to dispatcher's thread id so it can be identified as such
	dispatcherThreadPid = pthread_self();


	// parse arguments -p <portno>, -a for full name printed, -v for debug output
	while ((opt = getopt(argc, argv, "p:av")) != -1) {
		switch(opt) {
			case 'p':
				port = atoi(optarg);
				break;
			case 'a':
				if (write(STDERR_FILENO, "Liana Patel (lianap)", strlen("Liana Patel (lianap)\n")) < 0) {
		 			perror("invalid: ");
		 			return -1;
		 		}
		 		return 0;
				break;
			case 'v':
				debugFlag = 1;
				debug("%s\n", "DEBUG is on");
				break;
		}
	}

	debugDetailed("dispatcher thread id: %d\n", (int) dispatcherThreadPid);
	


	// create socket
	socketFD = socket(PF_INET, SOCK_STREAM, 0);
	if (socketFD < 0) {
		perror("invalid: ");
    	exit(1);
	}

	// set socket for reuse
	int optionVal = 1;
	if ((setsockopt(socketFD, SOL_SOCKET, SO_REUSEADDR, &optionVal, sizeof(int))) < 0) {
		perror("setsockopt(SO_REUSEADDR) failed");
	}
    

	debugDetailed("%s\n", "created socket");

	// bind
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = htons(INADDR_ANY);
	servaddr.sin_port = htons(port);
	int bindResult = bind(socketFD, (struct sockaddr*)&servaddr, sizeof(servaddr));
	if (bindResult < 0) {
		perror("invalid: ");
		exit(1);
	}

	debugDetailed("%s\n", "binded socket");

	//listen 
	int listenResult = listen(socketFD, QUEUE_LENGTH);
    if (listenResult < 0) {
      fprintf(stderr, "listen error: %s\n", gai_strerror(listenResult));
      exit(1);
    }

    debugDetailed("%s\n", "started listening on socket");

	// acept connections (support up to QUEUE_LENGTH concurrent connections)
	while (true) {
		struct sockaddr_in clientaddr;
		socklen_t clientaddrlen = sizeof(clientaddr);
		int comm_fd = accept(socketFD, (struct sockaddr*)&clientaddr, &clientaddrlen);
		if (comm_fd < 0) {
			perror("invalid: ");
			exit(1);
		}

		//update fdArr data structure
		int indx = nextIndex(fdArr, QUEUE_LENGTH);
		if (indx < 0) {
			debugDetailed("next index in fdArr returned: %d\n", indx);
			//todo error handle
			exit(-1);
		}
		fdArr[indx] = comm_fd;
		//check shutting down flag
		if (shuttingDown == 1) {
			close(fdArr[indx]);
			fdArr[indx] = 0;
			continue;
		}

		//when client connects, sned  asimple greeting message "+OK Server ready (Author: Liana Patel / lianap)"
		debug("[%d] New Connection\n", comm_fd);

		//create new thread for connection and pass comm_fd as argument
		tinfo_arr[indx] = (struct thread_info*) calloc(1, sizeof(struct thread_info));
		tinfo_arr[indx]->comm_fd = comm_fd;
		tinfo_arr[indx]->indx = indx;
		debugDetailed("callocing tinfo index %d\n", indx);
		int thread_res = pthread_create(&thread_ids[indx], NULL, &tExecute, tinfo_arr[indx]);
		if (thread_res != 0) {
			write(STDERR_FILENO, "pthread_create error\n", strlen("pthread_create error\n"));
			if (tinfo_arr[indx] != NULL) {
				free(tinfo_arr[indx]);
				tinfo_arr[indx] = NULL;
			}
			exit(-1);
		}


	}

	// never reached
	return 0;
}
