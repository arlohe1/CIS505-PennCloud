#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <rpc/server.h>



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
#include <tuple>
#include <string>

#define MAX_LEN_SERVER_DIR 15
enum Command {GET, PUT, CPUT, DELETE};


int debugFlag;
int err = -1;
int detailed = 0;

int serverIndx = 1;
std::map<std::string, std::map<std::string, std::string>> kvMap; // maps server index to ip addr
FILE* logfile = NULL;

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




// memset for volatile int array with length len
void volatileMemset(volatile int* start, char c, int len) {
	int i;
	for (i = 0; i < len; i++) {
		start[i] = c;
	}
	
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

// write to log.txt for
int logCommand(enum Command comm, int numArgs, std::string arg1, std::string arg2, std::string arg3, std::string arg4) {
	// check what command is 
	if ((logfile = fopen("log.txt", "a")) == NULL) {
		debugDetailed("could not open log.txt for server %d\n", serverIndx);
		perror("invalid fopen of log file: ");
		return -1;
	}
	// write header line command arg {GET, PUT, CPUT, DELETE}
	if (comm == GET) {
		//fprintf(logfile, "GET,");
		fwrite("GET,", sizeof(char), strlen("GET,"), logfile);
	} else if (comm == PUT) {
		//fprintf(logfile, "PUT,");
		fwrite("PUT,", sizeof(char), strlen("PUT,"), logfile);
	} else if (comm == CPUT) {
		//fprintf(logfile, "CPUT,");
		fwrite("CPUT,", sizeof(char), strlen("CPUT,"), logfile);
	} else if (comm == DELETE) {
		//fprintf(logfile, "DELETE,");
		fwrite("DELETE,", sizeof(char), strlen("DELETE,"), logfile);
	}

	// write headerline arglens
	if (numArgs == 2) {
		fprintf(logfile, "%ld,%ld,0,0\n", arg1.length(), arg2.length());
		fwrite(arg1.c_str(), sizeof(char), arg1.length(), logfile);
		fwrite(arg2.c_str(), sizeof(char), arg2.length(), logfile);
	} else if (numArgs == 3) {
		fprintf(logfile, "%ld,%ld,%ld,0\n", arg1.length(), arg2.length(), arg3.length());
		fwrite(arg1.c_str(), sizeof(char), arg1.length(), logfile);
		fwrite(arg2.c_str(), sizeof(char), arg2.length(), logfile);
		fwrite(arg3.c_str(), sizeof(char), arg3.length(), logfile);
	} else if (numArgs == 4) {
		fprintf(logfile, "%ld,%ld,%ld,%ld\n", arg1.length(), arg2.length(), arg3.length(), arg4.length());
		fwrite(arg1.c_str(), sizeof(char), arg1.length(), logfile);
		fwrite(arg2.c_str(), sizeof(char), arg2.length(), logfile);
		fwrite(arg3.c_str(), sizeof(char), arg3.length(), logfile);
		fwrite(arg4.c_str(), sizeof(char), arg4.length(), logfile);
	} else {
		debugDetailed("invalid number of args (%d) in logCommand\n", numArgs);
		fprintf(stderr, "invalid paramaters in logCommand\n");
	}

	fwrite("\n", sizeof(char), strlen("\n"), logfile);

	fclose(logfile);

	return 0;
}

std::tuple<int, std::string> put(std::string row, std::string col, std::string val) {
    logCommand(PUT, 3, row, col, val, row);
    kvMap[row][col] = val;
    debugDetailed("---put row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
    printKvMap();
    return std::make_tuple(0, "OK");
}

std::tuple<int, std::string> get(std::string row, std::string col) {
    logCommand(GET, 2, row, col, row, row);
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			std::string val = kvMap[row][col];
			debugDetailed("---get succeeded - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
			printKvMap();
			return std::make_tuple(0, val);
		}
	} 

	debugDetailed("---get val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> exists(std::string row, std::string col) {
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			std::string val = kvMap[row][col];
			debugDetailed("---exists succeeded - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
			printKvMap();
			return std::make_tuple(0, "OK");
		}
	} 

	debugDetailed("---get val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> cput(std::string row, std::string col, std::string expVal, std::string newVal) {
    logCommand(CPUT, 4, row, col, expVal, newVal);
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			if (expVal.compare(kvMap[row][col]) == 0) {
				kvMap[row][col] = newVal;
				debugDetailed("---cput updated - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
				printKvMap();
				return std::make_tuple(0, "OK");
			} else {
				debugDetailed("---cput did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
				printKvMap();
				return std::make_tuple(2, "Incorrect expVal");
			}
			
		}
	} 

	debugDetailed("---cput did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
	printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> del(std::string row, std::string col) {
	logCommand(DELETE, 2, row, col, row, row);
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			kvMap[row].erase(col);
			debugDetailed("---delete deleted row: %s, column: %s\n", row.c_str(), col.c_str());
			printKvMap();
			return std::make_tuple(0, "OK");
		}
	} 

	debugDetailed("---del val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}





int main(int argc, char *argv[]) {	
	int opt;
	int port = 10000;
	debugFlag = 0;

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

	// change dir to server's designated folder with checkpoint and logfile
	char serverDir[MAX_LEN_SERVER_DIR];
	sprintf(serverDir, "server_%d", serverIndx);
	int chdirRet = chdir(serverDir);
 	if (chdirRet == 0) {
 		debug("serverDir is: %s\n", serverDir);	
 	} else {
 		debug("no serverDir: %s\n", serverDir);	
 		if (write(STDERR_FILENO, "please create server directory", strlen("please create server directory")) < 0) {
 			perror("invalid write: ");
 		}
 		exit(-1);
 	}

	rpc::server srv(port);
	srv.bind("put", &put);
	srv.bind("get", &get);
	srv.bind("cput", &cput);
	srv.bind("del", &del);

	srv.run();

    return 0;



}

// run as ./kvServer -v -p 10000
