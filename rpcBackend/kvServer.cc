#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <rpc/server.h>

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>

#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/file.h>

#include <map>
#include <iostream>
#include <regex>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <tuple>
#include <string>

#define MAX_LEN_SERVER_DIR 15
#define MAX_LEN_LOG_HEADER 100
#define MAX_COMM_ARGS 4
#define COM_PER_CHECKPOINT 2
enum Command {GET, PUT, CPUT, DELETE};


int debugFlag;
int err = -1;
int detailed = 0;
int replay = 0;
int numCommandsSinceLastCheckpoint = 0;

int serverIdx = 1;
std::map<std::string, std::map<std::string, std::string>> kvMap; // maps server index to ip addr
FILE* logfile = NULL;
FILE* logfileRead = NULL;
std::string currServerAddr;

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
		          << serverIdx << " " << std::flush;
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

// TODO - eviction: global count of values put into mem, map of r, c, 0/1 in mem or disk, 
	// when treshold is reached run checkpoint, then clear map
	// need to add to put,get, cput, delete and in checkpoint a check for map value - Note you would assume that anything not in the cache has remained unchanged

void chdirToCheckpoint() {
	int chdirRet = chdir("checkpoint");
 	if (chdirRet == 0) {
 		debugDetailed("%s\n", "cd into checkpoint dir complete");	
 	} else {
 		debugDetailed("%s\n", "no checkpoint dir to cd into");	
 		if (write(STDERR_FILENO, "please create server's checkpoint directory", strlen("please create server's checkpoint directory")) < 0) {
 			perror("invalid write: ");
 		}
 		exit(-1);
 	}


}

void runCheckpoint() {
	debugDetailed("%s,\n", "--------RUNNING CHECKPOINT--------");
	// cd into checkpoint directory
	chdirToCheckpoint();
	// loop over rows in map and write each to a file
	FILE* rowFilePtr;
	for (const auto& [row, mapping]: kvMap) {
		rowFilePtr = fopen((row).c_str(), "w");
		for (const auto& [col, val]: mapping) {		
			// write all columns to file with format columnLen,ValLen\ncolumnName,ValName\n
			fprintf(rowFilePtr, "%ld,%ld\n", col.length(), val.length());
			fwrite(col.c_str(), sizeof(char), col.length(), rowFilePtr);
			fwrite(val.c_str(), sizeof(char), val.length(), rowFilePtr);
			fwrite("\n", sizeof(char), strlen("\n"), rowFilePtr);
			
		}
		fclose(rowFilePtr);
	}
		

	// cd back out to server directory
	chdir("..");

	// clear logfile
	FILE* logFilePtr;
	logFilePtr = fopen("log.txt", "w");
	fclose(logFilePtr);
	numCommandsSinceLastCheckpoint = 0;
	debugDetailed("%s,\n", "--------cleared log file and return--------");

	// need to add new function - loadKvStore - parses all row files and enters the appropriate column, value into map

}

int loadKvStoreFromDisk() {
	// loop over all files in checkpoint dir and read each row file
	debugDetailed("%s\n", "---------Entered loadKvStoreFromDisk");
	chdirToCheckpoint();
	DIR* dir = opendir(".");
	struct dirent* nextRowFile;
	nextRowFile = readdir(dir); // skip "."
	nextRowFile = readdir(dir); // skip ".."
	
	FILE* rowFilePtr; 
	char headerBuf[MAX_LEN_LOG_HEADER];
	memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
	while((nextRowFile = readdir(dir)) != NULL) {
		printf("reached\n");
		// open file
		if ((rowFilePtr = fopen(nextRowFile->d_name, "r")) == NULL) {
			debugDetailed("fopen failed %d\n", serverIdx);
			perror("invalid fopen of a checkpoint row file: ");
			fclose(rowFilePtr);
			// cd back out to server directory
			chdir("..");
			debugDetailed("%s\n", "---------Finished loadKvStoreFromDisk WITH ERROR");
			return -1;
		}
		int colLen;
		int valLen;
		char* col;
		char* val;
		std::string rowString(nextRowFile->d_name);
		// read formatted file and reconstruct row in kvMap
		while(fgets(headerBuf, MAX_LEN_LOG_HEADER, rowFilePtr) != NULL) {
			headerBuf[strlen(headerBuf)] = '\0'; //set newline to null
			debugDetailed("buf read from checkpoint file (%s) is: %s\n", nextRowFile->d_name, headerBuf);
			// find lengths
			colLen = atoi(strtok(headerBuf, ","));
			valLen = atoi(strtok(NULL, ","));
			col = (char*) calloc(colLen, sizeof(char));
			val = (char*) calloc(valLen, sizeof(char));
			// get col and value
			if (col != NULL) {
				fread(col, sizeof(char), colLen, rowFilePtr);
			}
			if (val != NULL) {
				fread(val, sizeof(char), valLen, rowFilePtr);
			}
			// enter into kvMap
			std::string colString(col, colLen);
			std::string valString(val, valLen);
			kvMap[rowString][colString] = valString;
			// free callocs
			free(col);
			free(val);
			//read newline char before re-looping
			fseek(rowFilePtr, 1, SEEK_CUR);
			memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
		}
		fclose(rowFilePtr);
	}
	printKvMap();
	// cd back out to server directory
	chdir("..");
	
	debugDetailed("%s\n", "---------Finished loadKvStoreFromDisk");

	return 0;	

}

// increments command count since last checkpoint and deals with checkpointing
void checkIfCheckPoint() {
	numCommandsSinceLastCheckpoint = numCommandsSinceLastCheckpoint + 1;
	debugDetailed("NUM completed commands: %d\n", numCommandsSinceLastCheckpoint);
	if (numCommandsSinceLastCheckpoint >= COM_PER_CHECKPOINT) {
		debugDetailed("%s\n", "triggering checkpoint");
		runCheckpoint();
	}

}

// write to log.txt if new command came in and handle checkpointing
int logCommand(enum Command comm, int numArgs, std::string arg1, std::string arg2, std::string arg3, std::string arg4) {
	// check what command is 
	if (replay == 0) {
		debugDetailed("%s\n", "logging command, replay flag off");
		if ((logfile = fopen("log.txt", "a")) == NULL) {
			debugDetailed("could not open log.txt for server %d\n", serverIdx);
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
		logfile = NULL;

		checkIfCheckPoint();
	} else {
		debugDetailed("%s\n", "did not re-log, replay flag is 1");
	}
	

	return 0;
}



std::tuple<int, std::string> put(std::string row, std::string col, std::string val) {
    kvMap[row][col] = val;
    debugDetailed("---PUT row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
    printKvMap();
    logCommand(PUT, 3, row, col, val, row);
    return std::make_tuple(0, "OK");
}

std::tuple<int, std::string> get(std::string row, std::string col) {
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			std::string val = kvMap[row][col];
			debugDetailed("---GET succeeded - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
			printKvMap();
			logCommand(GET, 2, row, col, row, row);
			return std::make_tuple(0, val);
		}
	} 

	debugDetailed("---GET val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	logCommand(GET, 2, row, col, row, row);
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> exists(std::string row, std::string col) {
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			std::string val = kvMap[row][col];
			debugDetailed("---EXISTS succeeded - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
			printKvMap();
			return std::make_tuple(0, "OK");
		}
	} 

	debugDetailed("---EXISTS val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> cput(std::string row, std::string col, std::string expVal, std::string newVal) {
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			if (expVal.compare(kvMap[row][col]) == 0) {
				kvMap[row][col] = newVal;
				debugDetailed("---CPUT updated - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
				printKvMap();
				logCommand(CPUT, 4, row, col, expVal, newVal);
				return std::make_tuple(0, "OK");
			} else {
				debugDetailed("---CPUT did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
				printKvMap();
				logCommand(CPUT, 4, row, col, expVal, newVal);
				return std::make_tuple(2, "Incorrect expVal");
			}
			
		}
	} 

	debugDetailed("---CPUT did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
	printKvMap();
	logCommand(CPUT, 4, row, col, expVal, newVal);
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> del(std::string row, std::string col) {
    if (kvMap.count(row) > 0) {
		if (kvMap[row].count(col) > 0) {
			kvMap[row].erase(col);
			debugDetailed("---DELETE deleted row: %s, column: %s\n", row.c_str(), col.c_str());
			printKvMap();
			logCommand(DELETE, 2, row, col, row, row);
			return std::make_tuple(0, "OK");
		}
	} 

	debugDetailed("---DELETE val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	logCommand(DELETE, 2, row, col, row, row);
	return std::make_tuple(1, "No such row, column pair");
}


void callFunction(char* comm, char* arg1, char* arg2, char* arg3, char* arg4, int len1, int len2, int len3, int len4) {
	if (strncmp(comm, "PUT", 3) == 0) {
		std::string rowString(arg1, len1);
		std::string colString(arg2, len2);
		std::string valString(arg3, len3);
		put(rowString, colString, valString);

	} else if (strncmp(comm, "GET", 3) == 0) {
		std::string rowString(arg1, len1);
		std::string colString(arg2, len2);
		get(rowString, colString);

	} else if (strncmp(comm, "CPUT", 4) == 0) {
		std::string rowString(arg1, len1);
		std::string colString(arg2, len2);
		std::string expValString(arg3, len3);
		std::string newValString(arg4, len4);
		cput(rowString, colString, expValString, newValString);

	} else if (strncmp(comm, "DELETE", 6) == 0) {
		std::string rowString(arg1, len1);
		std::string colString(arg2, len2);
		del(rowString, colString);

	} 
	return;
}

// NOTE: will segfault if bad formatted file eg) if len numbers are wrong - TODO make atoi wrapper/check strtok ret's
int replayLog() {
	replay = 1;
	if ((logfile = fopen("log.txt", "r")) == NULL) {
		debugDetailed("could not open log.txt for server %d\n", serverIdx);
		perror("invalid fopen of log file: ");
		return -1;
	}

	char headerBuf[MAX_LEN_LOG_HEADER];
	memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
	char* comm;
	int i;
	int lens[MAX_COMM_ARGS];
	char* args[MAX_COMM_ARGS];

	while(fgets(headerBuf, MAX_LEN_LOG_HEADER, logfile) != NULL) {
		if (headerBuf[0] == '\0') {
			debugDetailed("%s\n", "nothing in log file");
			break;
		}
		headerBuf[strlen(headerBuf)] = '\0'; //set newline to null
		debugDetailed("buf read from log file is: %s\n", headerBuf);
		comm = strtok(headerBuf, ",");
		for (i = 0; i < MAX_COMM_ARGS; i++) {
			lens[i] = atoi(strtok(NULL, ","));
		}
		for (i = 0; i < MAX_COMM_ARGS; i++) {
			args[i] = (char*) calloc(lens[i], sizeof(char));
			if (args[i] != NULL) {
				fread(args[i], sizeof(char), lens[i], logfile);
			}
		}

		debugDetailed("header args - comm: %s, len1: %d, len2: %d, len3: %d, len4: %d\n", comm, lens[0], lens[1], lens[2], lens[3]);
		debugDetailed("parsed args - arg1: %s, arg2: %s, arg3: %s, arg4: %s\n", args[0], args[1], args[2], args[3]);
		callFunction(comm, args[0], args[1], args[2], args[3], lens[0], lens[1], lens[2], lens[3]);

		for (i = 0; i < MAX_COMM_ARGS; i++) {
			if (args[i] != NULL) {
				free(args[i]);
			}
		}
		// read newline character before re-looping
		fseek(logfile, 1, SEEK_CUR);
		memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
	
	}
	fclose(logfile);
	replay = 0;
	return 0;
}


int main(int argc, char *argv[]) {	
	int opt;
	debugFlag = 0;

	// parse arguments -a for full name printed, -v for debug output
	while ((opt = getopt(argc, argv, "av")) != -1) {
		switch(opt) {
			case 'a':
				if (fprintf(stderr, "Liana Patel (lianap)") < 0) {
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

    char *serverListFile = NULL;
    if(optind + 1 < argc) {
        serverListFile = argv[optind];
        try {
            serverIdx = stoi(std::string(argv[optind+1]));
            if(serverIdx <= 0) {
                fprintf(stderr, "Invalid index provided! Exiting\n");
                exit(-1);
            }
        } catch (const std::invalid_argument &ia) {
            fprintf(stderr, "Invalid index provided! Exiting\n");
            exit(-1);
        }
    } else {
        fprintf(stderr, "Please provide a server config file and server index\n");
        exit(-1);
    }

    FILE * f = fopen(serverListFile, "r");
    if(f == NULL) {
        fprintf(stderr, "Provide a valid list of backend servers\n");
        exit(-1);
    }
    int serverNum = 0;
    char buffer[300];
    int port = 0;
    while(fgets(buffer, 300, f)){
        std::string server = std::string(buffer);
        if(serverNum == serverIdx) {
            if(server.at(server.length()-1) == '\n') {
                server = server.substr(0, server.length()-1);
            }
            currServerAddr = server;
            try {
                port = stoi(currServerAddr.substr(currServerAddr.find(":")+1));
                if(port <= 0) {
                    fprintf(stderr, "Invalid port provided! Exiting\n");
                    exit(-1);
                }
            } catch (const std::invalid_argument &ia) {
                fprintf(stderr, "Server port not found! Exiting\n");
                exit(-1);
            }
        }
        serverNum += 1;
    }
    fclose(f);

	// change dir to server's designated folder with checkpoint and logfile
	char serverDir[MAX_LEN_SERVER_DIR];
	sprintf(serverDir, "server_%d", serverIdx);
	int chdirRet = chdir(serverDir);
 	if (chdirRet == 0) {
 		debug("serverDir is: %s\n", serverDir);	
 	} else {
 		debug("no serverDir: %s\n", serverDir);	
 		if (fprintf(stderr, "Please create server directory\n") < 0) {
 			perror("invalid write: ");
 		}
 		exit(-1);
 	}

 	debug("%s\n", "kvMap before log replay or checkpoint: ");
 	printKvMap();
 	loadKvStoreFromDisk();
 	debug("%s\n", "kvMap before log replay: ");
 	printKvMap();
 	replayLog();
 	debug("%s\n", "kvMap after log replay: ");
 	printKvMap();

    debug("Connecting to port: %d\n", port);
	rpc::server srv(port);
	srv.bind("put", &put);
	srv.bind("get", &get);
	srv.bind("cput", &cput);
	srv.bind("del", &del);

	srv.run();

    return 0;
}

// run as ./kvServer -v -p 10000
