#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>


#include <rpc/server.h>
#include "rpc/client.h"
#include "rpc/rpc_error.h"
#include "rpc/this_server.h"

#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <signal.h>

#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/file.h>
#include <sys/stat.h>

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
#define COM_PER_CHECKPOINT 10
#define TIMEOUT_MILLISEC 5000


using resp_tuple = std::tuple<int, std::string>;

enum Command {GET, PUT, CPUT, DELETE};


int debugFlag;
int err = -1;
int detailed = 0;
int replay = 0;


int serverIdx;
int maxCache;
int startCacheThresh;
FILE* logfile = NULL;
std::string currServerAddr;


//TODO read these from the config file instead of hardcoding
std::string masterIP;
std::map<int, std::tuple<std::string, int>> clusterMembers;
std::tuple<std::string, int> myIp; // set after setting serverIdx
std::tuple<std::string, int> primaryIp;

// shared read/write data structures for threads
volatile int numCommandsSinceLastCheckpoint = 0;
volatile int cacheSize = 0;
std::map<std::string, std::map<std::string, std::string>> kvMap; // row -> col -> value
std::map<std::string, std::map<std::string, int>> kvLoc; // row -> col -> val (-1 for val deleted, 0 for val on disk, 1 for val in kvMap)

// semphores
pthread_mutex_t checkpointSemaphore;
std::map<std::string, pthread_mutex_t> rwSemaphores; // row -> rw semaphore
//std::map<std::string, pthread_mutex_t> countSemaphores; // row -> count semaphore
volatile int readcount = 0;




// TODO read from config fiel
void setClusterMembership(int serverIndx) {
	masterIP = "127.0.0.1:8000";
	clusterMembers[1] = std::make_tuple("127.0.0.1", 10000);
	clusterMembers[2] = std::make_tuple("127.0.0.1", 10001);
	clusterMembers[3] = std::make_tuple("127.0.0.1", 10002);
	// clusterMembers[2] = "127.0.0.1:10001";
	// clusterMembers[3] = "127.0.0.1:10002"; 
	myIp = clusterMembers[serverIndx]; // set after setting serverIdx
	primaryIp = std::make_tuple("127.0.0.1", 10000);

}


// checks if this node is the primary - returns 1 for yes else 0
int isPrimary() {
	if (primaryIp == myIp) {
		return 1;
	} 
	return 0;
}



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
		std::cout << "kvmap print (size: " << cacheSize << ") : \n";
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

void printKvLoc() {
	if (debugFlag == 1) {
		std::cout << "kvLoc print (size: " << cacheSize << ") : \n";
		for (const auto& x : kvLoc) {
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



// makes dirName directory if doesnt already exist, and cds gto the directory //returns 0 if dir exists, else -1
int chdirToRow(const char* dirName) {
	int chdirRet = chdir(dirName);
 	if (chdirRet == 0) {
 		debugDetailed("cd into row dir (already exists): %s\n", dirName);	
 		return 0;
 	} else {
 		int mkdirRet = mkdir(dirName, 0777);
 		if (mkdirRet < 0) {
 			debugDetailed("failed to create new row dir: %s", dirName);
 			if (write(STDERR_FILENO, "failed to create a checkpointing row directory", strlen( "failed to create a checkpointing row directory")) < 0) {
	 			perror("invalid write: ");
	 		}
	 		exit (-1); //TODO - handle this better
 		}
 		int chdirRet = chdir(dirName);
	 	if (chdirRet == 0) {
	 		debugDetailed("cd into row dir (just created): %s\n", dirName);	
	 	} else {
	 		debugDetailed("failed to cd into newly created row dir: %s", dirName);
	 		if (write(STDERR_FILENO, "failed to cd into newly created row directory", strlen("failed to cd into newly created row directory")) < 0) {
	 			perror("invalid write: ");
	 		}
	 		exit(-1);
	 	}
	 	return -1;
 	}

}

int lockCheckpoint() {
	if (pthread_mutex_lock(&checkpointSemaphore) != 0) {
		debugDetailed("%s\n", "error obtaining checkpoint mutex lock");
		return -1;
	}
	debugDetailed("-----LOCK CHECKPOINT------: %s\n", "lock obtained, returning");
	return 0;
}

int unlockCheckpoint() {
	if (pthread_mutex_unlock(&checkpointSemaphore) != 0) {
		debug("%s\n", "error unlocking checkpoint mutex");
		return -1;
	}
	debugDetailed("-----UNLOCK CHECKPOINT------: %s\n", "completed unlock, returning");
	return 0;
}

// TODO - make sure deleted rows are handled correclty
void runCheckpoint() {
	int valLen;
	debugDetailed("%s,\n", "--------RUNNING CHECKPOINT--------");
	if (lockCheckpoint() < 0) {
		return;
	}
	printKvMap();
	printKvLoc();
	
	// cd into checkpoint directory
	chdirToCheckpoint(); 

	// loop over rows in create folder for each
	FILE* colFilePtr;
	for (const auto& x: kvLoc) {
		std::string row = x.first;
		chdirToRow(row.c_str());
		//loop over columns, create new or truncate file for each and write value to file
		for (auto it = x.second.cbegin(); it != x.second.cend();) {
		//for (const auto& y: x.second) {	
			//std::string col = y.first;
			std::string col = (*it).first;
			if (col.c_str() == NULL) {
				printf("found nullllllllllllllllllllllll\n");
			}
			//std::string val = y.second;
			//int loc = y.second;
			int loc = (*it).second;
			debugDetailed("loop for: %s, -> %d\n", col.c_str(), loc);
			// case on kvLoc val -1 (deleted), 0 (on disk), or 1 (in local kvMap)
			if (loc == -1) {
				if(remove( col.c_str() ) != 0 ) {
     				perror( "Error deleting file" );
				} else {
				 	debugDetailed("checkpoint deletes file: %s\n", col.c_str());
				 	// NOTE - del has already been called and removed the val form kvMap and adjusted cache size
				// 	//valLen = kvMap[row][col].length();
				// 	kvMap[row].erase(col);
				// 	printf("reached 0\n");
				 	//kvLoc[row].erase(col);
				 	it = kvLoc[row].erase(it);

				// 	//cacheSize = cacheSize - valLen;
				 	printKvMap();
					printKvLoc();
				// 	printf("reached 1\n");
					
				}
			} else if (loc == 1) {
				debugDetailed("checkpoint writes file: %s\n", col.c_str());
				colFilePtr = fopen((col).c_str(), "w");
				// write all value to file with format valLen\nvalue
				fprintf(colFilePtr, "%ld\n", kvMap[row][col].length());
				fwrite(kvMap[row][col].c_str(), sizeof(char), kvMap[row][col].length(), colFilePtr);
				fclose(colFilePtr);	
				valLen = kvMap[row][col].length();
				kvMap[row].erase(col);
				kvLoc[row][col] = 0;
				cacheSize = cacheSize - valLen;		
				printKvMap();
				++it;
			} else {
				++it;
			}
			printf("reached 2\n");		
		}
		printf("reached 3\n");
		
		chdir("..");
		printf("reached 4\n");
	}
	// cd back out to server directory
	chdir("..");
	printf("reached 5\n");

	// clear logfile if not currently replaying log
	if (replay == 0) {
		printf("reached 6\n");
		FILE* logFilePtr;
		logFilePtr = fopen("log.txt", "w");
		fclose(logFilePtr);
		debugDetailed("%s,\n", "--------cleared log file and return--------");
	}
	numCommandsSinceLastCheckpoint = 0;
	debugDetailed("--------cacheSize: %d, kvMap after checkpoint\n", cacheSize);
	printKvMap();
	printKvLoc();
	debugDetailed("%s,\n", "--------checkpoint finished and return--------");
	if (unlockCheckpoint() < 0) {
		return;
	}
	return;
	

	// need to add new function - loadKvStore - parses all row files and enters the appropriate column, value into map

}

FILE * openValFile(char* row, char* col, const char* mode) {
	if (lockCheckpoint() < 0) {
		exit(-1);
	}
	std::string filePath("checkpoint");
	std::string rowString(row);
	std::string colString(col);
	filePath = filePath + "/" + rowString + "/" + colString;
	FILE* ret = fopen(filePath.c_str(), mode);
	if (ret == NULL) {
		perror("error in fopen in openValFile: ");
		debugDetailed("openValFile error in fopen call for file path: %s, mode: %s\n", filePath.c_str(), mode);
	}
	debugDetailed("openValFile finished in fopen call for file path: %s, mode: %s\n", filePath.c_str(), mode);
	if (unlockCheckpoint() < 0) {
		exit(-1);
	}
	return ret;

}

// gets header from a newly opened row file in checkpoint folder
int getValSize(FILE* colFilePtr) {
	char headerBuf[MAX_LEN_LOG_HEADER];
	memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
	// read from column file the formatted length
	if ((fgets(headerBuf, MAX_LEN_LOG_HEADER, colFilePtr)) == NULL) {
		perror("invalid fgets when trying to read col file reader: ");	
		return -1;
	}
	headerBuf[strlen(headerBuf)] = '\0'; // set newlien to null
	int valLen = (atoi(headerBuf));
	debugDetailed("getValSize returns len:%d\n", valLen);
	return valLen;
	
}


// returns 0 if there was space, and -1 if not
int readAndLoadValIfSpace(FILE* fptr, int valLen, int cacheThresh, char* row, char* col) {
	// check if space
	if (cacheSize + valLen <= cacheThresh) {
		// read in val 
		char* val = (char*) calloc(valLen, sizeof(char));
		if (val != NULL) {
			fread(val, sizeof(char), valLen, fptr);
		} else {
			perror("cannot calloc value in readAndLoadValIfSpace()");
			return -1;
		}
		//update kvMap and kvLoc
		std::string rowString(row);
		std::string colString(col);
		std::string valString(val, valLen);
		kvMap[rowString][colString] = valString;
		cacheSize = cacheSize + valLen;
		kvLoc[rowString][colString] = 1;
		free(val);
		return 0;
	} else {
		// update kvLoc
		std::string rowString(row);
		std::string colString(col);
		kvLoc[rowString][colString] = 0;
		return -1;
	}
}


// call on server start
int loadKvStoreFromDisk() {
	// loop over all files in checkpoint dir and read each row file
	debugDetailed("%s\n", "---------Entered loadKvStoreFromDisk");
	chdirToCheckpoint();
	DIR* dir = opendir(".");
	struct dirent* nextRowDir;
	nextRowDir = readdir(dir); // skip "."
	nextRowDir = readdir(dir); // skip ".."
	
	FILE* colFilePtr; 
	char headerBuf[MAX_LEN_LOG_HEADER];
	memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
	while((nextRowDir = readdir(dir)) != NULL) {
		// cd into next row directory
		if (chdirToRow(nextRowDir->d_name) < 0) {
			debugDetailed("error opening row directory %s\n", nextRowDir->d_name);
			fprintf(stderr, "error opening row directory %s\n", nextRowDir->d_name);
		}
		// loop over column files
		DIR* colDir = opendir(".");
		struct dirent* nextColFile;
		nextColFile = readdir(colDir); // skip "."
		nextColFile = readdir(colDir); // skip ".."
		while((nextColFile = readdir(colDir)) != NULL) {
			//open the column file
			if ((colFilePtr = fopen(nextColFile->d_name, "r")) == NULL) {
				debugDetailed("fopen failed when opening %s\n", nextColFile->d_name);
				perror("invalid fopen of a checkpoint col file: ");			
				debugDetailed("%s\n", "---------Finished loadKvStoreFromDisk WITH ERROR");
				return -1;
			}
			// read from column file the formatted length
			if ((fgets(headerBuf, MAX_LEN_LOG_HEADER, colFilePtr)) == NULL) {
				perror("invalid fgets when trying to read col file reader: ");	
				return -1;
			}
			headerBuf[strlen(headerBuf)] = '\0'; // set newlien to null
			int valLen = (atoi(headerBuf));
			debugDetailed("buf read from checkpoint file (%s) is: %s, valLen:%d\n", nextColFile->d_name, headerBuf, valLen);
			
			// enter into kvMap - if space
			readAndLoadValIfSpace(colFilePtr, valLen, startCacheThresh, nextRowDir->d_name, nextColFile->d_name);

			memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
			fclose(colFilePtr);
		}
		closedir(colDir);
		chdir("..");
	}
	chdir("..");
	closedir(dir);
	
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



// helper to compare responses
resp_tuple combineResps(std::map<std::tuple<std::string, int>, std::tuple<int, std::string>> respSet) {
	std::tuple<int, std::string> firstVal = std::make_tuple(0, "");
	std::tuple<int, std::string> resp = std::make_tuple(0, "");
	for (const auto& x : respSet) {
		//x.first is ip:port, x.second is resp_tuple
    	if (firstVal == std::make_tuple(0, "")) {
    		firstVal = x.second;
    		resp = x.second;
    	} else {
    		if (x.second != firstVal) {
    			resp = std::make_tuple(3, "Error with consistency");
    			return (resp_tuple) resp;
    		}
    	}	
	}
	if (firstVal == std::make_tuple(0, "")) {
		debugDetailed("%s\n", "error in combine resps - empty respSet arg");
		resp = std::make_tuple(-1, "Error: empty resp set");
	}
	return (resp_tuple) resp;
	//return respSet[myIp];
}

int lockRow(std::string row) {
	if (rwSemaphores.count(row) == 0) {
		// initalize semaphore for this row
		if (pthread_mutex_init(&rwSemaphores[row], NULL) != 0) {
			perror("invalid pthread_mutex_init:");
			return -1;
		}
		debugDetailed("-----LOCK ROW------: %s, row: %s\n", "new lock created", row.c_str());
	}
	if (pthread_mutex_lock(&rwSemaphores[row]) != 0) {
		debugDetailed("%s\n", "error obtaining mutex lock");
		return -1;
	}
	debugDetailed("-----LOCK ROW------: %s, row:%s\n", "lock obtained, returning", row.c_str());
	return 0;
}

int unlockRow(std::string row) {
	if (rwSemaphores.count(row) == 0) {
		// initalize semaphore for this row
		debugDetailed("ERROR IN UNLOCK ROW: %s", "unlock called on nonexistent mutex");
		exit(-1);
	}
	if (pthread_mutex_unlock(&rwSemaphores[row]) != 0) {
		debug("%s\n", "error unlocking mutex");
		return -1;
	}
	debugDetailed("-----UNLOCK ROW------: %s, row:%s\n", "completed unlock, returning", row.c_str());
	return 0;
}




std::tuple<int, std::string> put(std::string row, std::string col, std::string val) {
    debugDetailed("---PUT entered - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
    int oldLen = kvMap[row][col].length();
    int ranCheckPoint = 0; // need this flag to properly update chacheSize

    // check if can store in local cache
    if (val.length() + cacheSize - oldLen > maxCache) {
    	debugDetailed("------PUT evicting on valLen: %ld, cacheSize: %d, maxCache: %d\n", val.length(), cacheSize, maxCache);
    	runCheckpoint();
    	ranCheckPoint = 1;
    }
    if (val.length() + cacheSize - oldLen <= maxCache) {
    	// put into kvMap, upate cache size and set kvLoc = 1
    	
    	kvMap[row][col] = val;
    	kvLoc[row][col] = 1;
    	cacheSize = cacheSize + val.length();
    	if (ranCheckPoint == 0) {
    		cacheSize = cacheSize - oldLen;
    	}
    	debugDetailed("------PUT row: %s, column: %s, val: %s, cahceSize: %d\n", row.c_str(), col.c_str(), val.c_str(), cacheSize);
    	printKvMap();
    	logCommand(PUT, 3, row, col, val, row);
    	return std::make_tuple(0, "OK");
    } else {
    	// evict everything then rerun the function (but dont log the second time)
    	debugDetailed("------PUT evicting FAILED row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
    	fprintf(stderr, "memCache overfilled\n");
    	exit(0);
    }
}

std::tuple<int, std::string> putApproved(std::string row, std::string col, std::string val) {
	// claim semaphore on row
	if (lockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}	

	resp_tuple resp =  put(row, col, val);

	// release semaphor on row
	if (unlockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}
	return resp;
}


std::tuple<int, std::string> putReq(std::string row, std::string col, std::string val) {
	debugDetailed("---PUTREQ entered, primary is: %s:%d\n", std::get<0>(primaryIp).c_str(), std::get<1>(primaryIp));
	std::map<std::tuple<std::string, int>, resp_tuple> respSet; // map from ip:port -> resp tuple 
	resp_tuple resp;
	// handle whether of not receiving node is a primary
    if (isPrimary() == 1) {
    	// if node is primary, perform local put and loop over memebers in cluster and call put (synchronous w timeout) on them
    	if (lockRow(row) < 0) {
    		return std::make_tuple(1, "ERR");
    	}	
    	respSet[myIp] = put(row, col, val);
    	debugDetailed("---PUTREQ entered - I am primary: %s\n", "local put completed");
    	for (const auto& server : clusterMembers) {
    		// server.first is serverIndx, server.second is tuple (ip string, port int)
    		if (server.second != myIp) {
    			rpc::client client(std::get<0>(server.second), std::get<1>(server.second));
				try { // TODO - on timeout, query connection state and retry if connected
			        // default timeout is 5000 milliseconds TODO: adjust timeout as needed
			        const uint64_t short_timeout = TIMEOUT_MILLISEC;
			        client.set_timeout(short_timeout);
			        debugDetailed("---PUTREQ entered: %s: %s:%d\n", "trying remote put to ", std::get<0>(server.second).c_str(), std::get<1>(server.second));
			        //resp = client.call("put", short_timeout + 10).as<resp_tuple>();
			        resp = client.call("putApproved", row, col, val).as<resp_tuple>();
			        respSet[server.second] = resp;
			    } catch (rpc::timeout &t) {
			        // will display a message like
			        // rpc::timeout: Timeout of 50ms while calling RPC function 'put'
			        std::cout << t.what() << std::endl;
			    } catch (rpc::rpc_error &e) {
				    std::cout << std::endl << e.what() << std::endl;
				    std::cout << "in function " << e.get_function_name() << ": ";

				    using err_t = std::tuple<int, std::string>;
				    auto err = e.get_error().as<err_t>();
				    std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
				              << std::endl;
				    return std::make_tuple(1, "ERR");
			    }
    		}
		}
		// release semaphor on row
		if (unlockRow(row) < 0) {
    		return std::make_tuple(1, "ERR");
    	}
		debugDetailed("---PUTREQ entered - primary: %s\n", "calling combineResps");
		// return resp tuple
		resp = combineResps(respSet);
		return resp;
		//return respSet[myIp];
    } else {
    	debugDetailed("---PUTREQ entered - I am NOT primary: %s\n", "sending to primary");
    	// send put Req to primary //TODO need to handle case where primary has failed
    	//std::cout << "received a put request and reached putReq fct " << std::endl;
    	rpc::client client(std::get<0>(primaryIp), std::get<1>(primaryIp));
    	resp = client.call("put", row, col, val).as<resp_tuple>(); // TODO - add the time out and possible re-leader election here
    	//debugDetailed("---PUTREQ entered - I am NOT primary: %s\n", "local put completed");
    	return resp;
    }
    return std::make_tuple(-1, "ERR");

}

// get val if known to exist
std::string getValDiskorLocal(std::string row, std::string col) {
	std::string val;
	if (kvLoc[row][col] == 0) { // TODO would need to claim semaphore here in this case on checkpoint where eviction happens
		debugDetailed("%s\n", "row, col, val on disk, retrieiving..");
		// try to load from disk, and run checkpoint if needed
		FILE* colFilePtr = openValFile((char*) row.c_str(), (char*) col.c_str(), "r");
		int valLen = getValSize(colFilePtr);
		int loadRes = readAndLoadValIfSpace(colFilePtr, valLen, maxCache, (char*) row.c_str(), (char*) col.c_str());
		if (loadRes == -1) {
			runCheckpoint();
			readAndLoadValIfSpace(colFilePtr, valLen, maxCache, (char*) row.c_str(), (char*) col.c_str());
		}	
	}
	val = kvMap[row][col];
	return val;
}


std::tuple<int, std::string> get(std::string row, std::string col) {
	// check that row, col exists in tablet, and not deleted
	debugDetailed("---GET entered - row: %s, column: %s\n", row.c_str(), col.c_str());
	if (lockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}
	if (kvLoc.count(row) > 0) {
		if (kvLoc[row].count(col) > 0) {
			if (kvLoc[row][col] != -1) {
				std::string val = getValDiskorLocal(row, col);			
				debugDetailed("---GET succeeded - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
				printKvMap();
				if (unlockRow(row) < 0) {
					return std::make_tuple(1, "ERR");
				}
				//logCommand(GET, 2, row, col, row, row);
				return std::make_tuple(0, val);
			}
			
		}
	} 
	
	debugDetailed("---GET val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	// release semaphor on row
	if (unlockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}
	//logCommand(GET, 2, row, col, row, row);
	return std::make_tuple(1, "No such row, column pair");	
}

std::tuple<int, std::string> exists(std::string row, std::string col) {
    if (kvLoc.count(row) > 0) {
		if (kvLoc[row].count(col) > 0) {
			debugDetailed("---EXISTS succeeded - row: %s, column: %s\n", row.c_str(), col.c_str());
			printKvMap();
			return std::make_tuple(0, "OK");
		}
	} 

	debugDetailed("---EXISTS val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> cput(std::string row, std::string col, std::string expVal, std::string newVal) {
	debugDetailed("---CPUT entered - row: %s, column: %s, expVal: %s, newVal: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());

	if (kvLoc.count(row) > 0) {
		if (kvLoc[row].count(col) > 0) {
			if (kvLoc[row][col] != -1) {
				std::string val = getValDiskorLocal(row, col);	
				debugDetailed("------CPUT correct val: %s\n", val.c_str());	
				if (expVal.compare(kvMap[row][col]) == 0) {
					put(row, col, newVal);
					debugDetailed("------CPUT called put and updated val - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
					//debugDetailed("------CPUT updated - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
					printKvMap();
					//logCommand(CPUT, 4, row, col, expVal, newVal);
					return std::make_tuple(0, "OK");
				} else {
					debugDetailed("------CPUT did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
					printKvMap();
					//logCommand(CPUT, 4, row, col, expVal, newVal);
					return std::make_tuple(2, "Incorrect expVal");
				}
			}
			
		}
	} 

	debugDetailed("------CPUT did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
	printKvMap();
	//logCommand(CPUT, 4, row, col, expVal, newVal);
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> cputApproved(std::string row, std::string col, std::string val, std::string newVal) {
	// claim semaphore on row
	if (lockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}	

	resp_tuple resp =  cput(row, col, val, newVal);

	// release semaphor on row
	if (unlockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}
	return resp;
}


std::tuple<int, std::string> cputReq(std::string row, std::string col, std::string val, std::string newVal) {
	debugDetailed("---CPUTREQ entered, primary is: %s:%d\n", std::get<0>(primaryIp).c_str(), std::get<1>(primaryIp));
	std::map<std::tuple<std::string, int>, resp_tuple> respSet; // map from ip:port -> resp tuple 
	resp_tuple resp;
	// handle whether of not receiving node is a primary
    if (isPrimary() == 1) {
    	// if node is primary, perform local put and loop over memebers in cluster and call put (synchronous w timeout) on them
    	if (lockRow(row) < 0) {
    		return std::make_tuple(1, "ERR");
    	}
    	respSet[myIp] = cput(row, col, val, newVal);
    	debugDetailed("---CPUTREQ entered - I am primary: %s\n", "local cput completed");
    	for (const auto& server : clusterMembers) {
    		// server.first is serverIndx, server.second is tuple (ip string, port int)
    		if (server.second != myIp) {
    			rpc::client client(std::get<0>(server.second), std::get<1>(server.second));
				try { // TODO - on timeout, query connection state and retry if connected
			        // default timeout is 5000 milliseconds TODO: adjust timeout as needed
			        const uint64_t short_timeout = 5000;
			        client.set_timeout(short_timeout);
			        debugDetailed("---CPUTREQ entered: %s: %s:%d\n", "trying remote cput to ", std::get<0>(server.second).c_str(), std::get<1>(server.second));
			        //resp = client.call("put", short_timeout + 10).as<resp_tuple>();
			        resp = client.call("cputApproved", row, col, val, newVal).as<resp_tuple>();
			        respSet[server.second] = resp;
			    } catch (rpc::timeout &t) {
			        // will display a message like
			        // rpc::timeout: Timeout of 50ms while calling RPC function 'put'
			        std::cout << t.what() << std::endl;
			    } catch (rpc::rpc_error &e) {
				    std::cout << std::endl << e.what() << std::endl;
				    std::cout << "in function " << e.get_function_name() << ": ";

				    using err_t = std::tuple<int, std::string>;
				    auto err = e.get_error().as<err_t>();
				    std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
				              << std::endl;
				    return std::make_tuple(1, "ERR");
			    }
    		}
		}
		// release semaphor on row
		if (unlockRow(row) < 0) {
    		return std::make_tuple(1, "ERR");
    	}
		debugDetailed("---CPUTREQ entered - primary: %s\n", "calling combineResps");
		// return resp tuple
		resp = combineResps(respSet);
		return resp;
		//return respSet[myIp];
    } else {
    	debugDetailed("---CPUTREQ entered - I am NOT primary: %s\n", "sending to primary");
    	// send put Req to primary //TODO need to handle case where primary has failed
    	//std::cout << "received a put request and reached putReq fct " << std::endl;
    	rpc::client client(std::get<0>(primaryIp), std::get<1>(primaryIp));
    	resp = client.call("cput", row, col, val, newVal).as<resp_tuple>(); // TODO - add the time out and possible re-leader election here
    	//debugDetailed("---PUTREQ entered - I am NOT primary: %s\n", "local put completed");
    	return resp;
    }
    return std::make_tuple(-1, "ERR");

}

std::tuple<int, std::string> del(std::string row, std::string col) {
	debugDetailed("%s\n", "del entered");
	printKvMap();
	printKvLoc();
	if (kvLoc.count(row) > 0) {
		if (kvLoc[row].count(col) > 0) {
			if (kvLoc[row][col] != -1) {
				if (kvLoc[row][col] == 1) {
					cacheSize = cacheSize - kvMap[row][col].length();
					
				}
				kvMap[row].erase(col);
				kvLoc[row][col] = -1;	
				debugDetailed("---DELETE deleted row: %s, column: %s\n", row.c_str(), col.c_str());	
				printKvMap();	
				logCommand(DELETE, 2, row, col, row, row);
				return std::make_tuple(0, "OK");
			}
			
		}
	}
	debugDetailed("---DELETE val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	//logCommand(DELETE, 2, row, col, row, row);
	return std::make_tuple(1, "No such row, column pair");
}


std::tuple<int, std::string> delApproved(std::string row, std::string col) {
	// claim semaphore on row
	if (lockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}	

	resp_tuple resp =  del(row, col);

	// release semaphor on row
	if (unlockRow(row) < 0) {
		return std::make_tuple(1, "ERR");
	}
	return resp;
}


std::tuple<int, std::string> delReq(std::string row, std::string col) {
	debugDetailed("---delREQ entered, primary is: %s:%d\n", std::get<0>(primaryIp).c_str(), std::get<1>(primaryIp));
	std::map<std::tuple<std::string, int>, resp_tuple> respSet; // map from ip:port -> resp tuple 
	resp_tuple resp;
	// handle whether of not receiving node is a primary
    if (isPrimary() == 1) {
    	// if node is primary, perform local put and loop over memebers in cluster and call put (synchronous w timeout) on them
    	if (lockRow(row) < 0) {
    		return std::make_tuple(1, "ERR");
    	}	
    	respSet[myIp] = del(row, col);
    	debugDetailed("---delREQ entered - I am primary: %s\n", "local del completed");
    	for (const auto& server : clusterMembers) {
    		// server.first is serverIndx, server.second is tuple (ip string, port int)
    		if (server.second != myIp) {
    			rpc::client client(std::get<0>(server.second), std::get<1>(server.second));
				try { // TODO - on timeout, query connection state and retry if connected
			        // default timeout is 5000 milliseconds TODO: adjust timeout as needed
			        const uint64_t short_timeout = 5000;
			        client.set_timeout(short_timeout);
			        debugDetailed("---delREQ entered: %s: %s:%d\n", "trying remote put to ", std::get<0>(server.second).c_str(), std::get<1>(server.second));
			        //resp = client.call("put", short_timeout + 10).as<resp_tuple>();
			        resp = client.call("delApproved", row, col).as<resp_tuple>();
			        respSet[server.second] = resp;
			    } catch (rpc::timeout &t) {
			        // will display a message like
			        // rpc::timeout: Timeout of 50ms while calling RPC function 'put'
			        std::cout << t.what() << std::endl;
			    } catch (rpc::rpc_error &e) {
				    std::cout << std::endl << e.what() << std::endl;
				    std::cout << "in function " << e.get_function_name() << ": ";

				    using err_t = std::tuple<int, std::string>;
				    auto err = e.get_error().as<err_t>();
				    std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
				              << std::endl;
				    return std::make_tuple(1, "ERR");
			    }
    		}
		}
		// release semaphor on row
		if (unlockRow(row) < 0) {
			return std::make_tuple(1, "ERR");
		}
		debugDetailed("---delREQ entered - primary: %s\n", "calling combineResps");
		// return resp tuple
		resp = combineResps(respSet);
		return resp;
		//return respSet[myIp];
    } else {
    	debugDetailed("---delREQ entered - I am NOT primary: %s\n", "sending to primary");
    	// send put Req to primary //TODO need to handle case where primary has failed
    	//std::cout << "received a put request and reached putReq fct " << std::endl;
    	rpc::client client(std::get<0>(primaryIp), std::get<1>(primaryIp));
    	resp = client.call("del", row, col).as<resp_tuple>(); // TODO - add the time out and possible re-leader election here
    	//debugDetailed("---PUTREQ entered - I am NOT primary: %s\n", "local put completed");
    	return resp;
    }
    return std::make_tuple(-1, "ERR");

}


void callFunction(char* comm, char* arg1, char* arg2, char* arg3, char* arg4, int len1, int len2, int len3, int len4) {
	numCommandsSinceLastCheckpoint = numCommandsSinceLastCheckpoint + 1;
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
		comm = strtok(headerBuf, ","); // this should never be null if headerBuf is well formatted
		for (i = 0; i < MAX_COMM_ARGS; i++) {
			lens[i] = atoi(strtok(NULL, ","));
			debugDetailed("---replayLog: lens[%d] = %d\n", i, lens[i]);
		}
		for (i = 0; i < MAX_COMM_ARGS; i++) {
			if (lens[i] != 0) {
				debugDetailed("---replayLog: calloc'd args[%d] for lens[%d] = %d\n", i, i, lens[i]);
				args[i] = (char*) calloc(lens[i], sizeof(char));
			} else {
				debugDetailed("---replayLog: calloc'd args[%d] is NULL\n", i);
				args[i] = NULL;
			}
			if (args[i] != NULL) {
				debugDetailed("---replayLog: reading into args[%d], lens[%d] = %d\n", i, i, lens[i]);
				fread(args[i], sizeof(char), lens[i], logfile);
			}
		}

		// NOTE: uncomment lines below for debug statements, but this will casue 3 memory erros from one context when run in valgrind
		//debugDetailed("header args - comm: %s, len1: %d, len2: %d, len3: %d, len4: %d\n", comm, lens[0], lens[1], lens[2], lens[3]);
		//debugDetailed("parsed args - arg1: %s, arg2: %s, arg3: %s, arg4: %s\n", args[0], args[1], args[2], args[3]);
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



void signalHandler(int sig) {
	debugDetailed("%s\n", "signal caught by server");	
	
	if (sig == SIGINT) {
		// quit server
		rpc::this_server().stop();
		exit(0);
		
	}
	
}

/* sets up signal handler given @signum and @handler and prints error if necessary */
void makeSignal(int signum, sighandler_t handler) {
	if (signal(signum, handler) == SIG_ERR) {
		perror("invalid: ");
	}
}


int main(int argc, char *argv[]) {	
	int opt;
	debugFlag = 0;
	makeSignal(SIGINT, signalHandler);

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

	// set memory cache size
	struct rlimit *rlim = (struct rlimit*) calloc(1, sizeof(struct rlimit));
	getrlimit(RLIMIT_MEMLOCK, rlim);
	maxCache = rlim->rlim_cur/3;
	fprintf(stderr, "total mem limit: %ld\n", rlim->rlim_cur);
	fprintf(stderr, "cache mem limit: %d\n", maxCache);
	startCacheThresh = maxCache / 2;
	free(rlim);

	// open config file
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


    // parse config file
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

    setClusterMembership(serverIdx); // TODO need to fix this funciton
    

    //initialize checkpoint semaphore
    if (pthread_mutex_init(&checkpointSemaphore, NULL) != 0) {
    	perror("invalid pthread_mutex_init for checkpointSemaphore:");
		return -1;
    }

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
 	printKvLoc();
 	debug("numCommandsSinceLastCheckpoint: %d\n", numCommandsSinceLastCheckpoint);

    debug("Connecting to port: %d\n", port);
	rpc::server srv(port);
	srv.bind("putApproved", &putApproved); 
	srv.bind("put", &putReq);
	srv.bind("cputApproved", &cputApproved); 
	srv.bind("cput", &cputReq);
	srv.bind("delApproved", &delApproved); 
	srv.bind("del", &delReq);
	srv.bind("get", &get);
	//srv.bind("cput", &cput);
	//srv.bind("del", &del);

	srv.async_run(10);

	while(1) {
		sleep(10);
	}

    return 0;
}

// run as ./kvServer -v -p 10000
