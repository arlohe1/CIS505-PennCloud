#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

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
#include <sys/time.h>

#include <map>
#include <iostream>
#include <regex>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <tuple>
#include <string>
#include <fstream>
#include <streambuf>

#define MAX_LEN_SERVER_DIR 15
#define MAX_LEN_LOG_HEADER 100
#define MAX_COMM_ARGS 4
#define COM_PER_CHECKPOINT 5
#define TIMEOUT_MILLISEC 10000
#define CHECKPOINT_CNT_FILE "checkpointNum.txt"

enum Command {GET, PUT, CPUT, DELETE};
enum EvictCause {MEM, COMM};

int debugFlag;
int err = -1;
int detailed = 0;
int replay = 0;

int serverIdx;
int maxCache;
int startCacheThresh;

// shared read/write data structures for threads
volatile int numCommandsSinceLastCheckpoint = 0;
volatile int cacheSize = 0;
std::map<std::string, std::map<std::string, std::string>> kvMap; // row -> col -> value
std::map<std::string, std::map<std::string, int>> kvLoc; // row -> col -> val (-1 for val deleted, 0 for val on disk, 1 for val in kvMap)

// semphores
pthread_mutex_t checkpointSemaphore;
pthread_mutex_t cacheSizeSemaphore;
pthread_mutex_t primarySemaphore;
pthread_mutex_t numCommandsSinceLastCheckpointSemaphore;
pthread_mutex_t logfileSemaphore;
std::map<std::string, pthread_mutex_t> rwSemaphores; // row -> rw semaphore

std::string myAddrPortForFrontend;
std::string myAddrPortForAdmin;
int kvsPortForFrontend = 0;
int kvsPortForAdmin = 0;
std::string masterNodeAddrPort;
std::string myClusterLeader;
pthread_t kvServerWithFrontendThreadId;

std::deque<std::string> clusterMembers;
std::map<std::string, std::string> serverKVSPortToAdminPortMap;
std::set<std::string> clusterNodesToSkip;

// Cluster Number, isClusterLeader, Addr:Port for comm w/ Frontend, Addr:Port for comm w/ Admin
using server_addr_tuple = std::tuple<int, bool, std::string, std::string>;
using resp_tuple = std::tuple<int, std::string>;



// Returns addr from given string of format addr:port
std::string getIPAddr(std::string addrPort) {
    return addrPort.substr(0, addrPort.find(":"));
}

// Returns port from given string of format addr:port
int getIPPort(std::string addrPort) {
    return stoi(addrPort.substr(addrPort.find(":")+1));
}

// Returns true if this node is the cluster leader (false o/w)
bool isPrimary() {
    return (myAddrPortForFrontend.compare(myClusterLeader) == 0);
}

void debugTime() {
	if (debugFlag) {
        struct timeval tval;
        gettimeofday(&tval, NULL);
        struct tm *tm_info = localtime(&tval.tv_sec);
        char timeBuff[25] = "";
        strftime(timeBuff, 25, "%H:%M:%S", tm_info);
        char timeBuffWithMilli[50] = "";
        sprintf(timeBuffWithMilli, "%s.%06ld ", timeBuff, tval.tv_usec);
        std::string timestamp(timeBuffWithMilli);
		std::cout << timestamp << std::flush;
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

int lockAllRowsExcept(std::string row) {
	// lock map
	// loop over semaphore map and lock row
	for (const auto& x : rwSemaphores) {
		//x.first is row, x.second is semaphore
		if (x.first != row) {
			if (pthread_mutex_lock(&rwSemaphores[x.first]) != 0) {
				debugDetailed("%s\n", "error obtaining mutex lock in lockAllRowsExcept()");
				return -1;
			}
		}
    	
	}
	return 0;
	// unlock map
}

int unlockAllRowsExcept(std::string row) {
	// lock map
	// loop over semaphore map and lock row
	for (const auto& x : rwSemaphores) {
		//x.first is row, x.second is semaphore
		if (x.first != row) {
			if (pthread_mutex_unlock(&rwSemaphores[x.first]) != 0) {
				debugDetailed("%s\n", "error unlocking mutex in lockAllRowsExcept()");
				return -1;
			}
		}
    	
	}
	return 0;
	// unlock map
}

int lockAllRows() {
	// lock map
	// loop over semaphore map and lock row
	for (const auto& x : rwSemaphores) {
		//x.first is row, x.second is semaphore
		if (pthread_mutex_lock(&rwSemaphores[x.first]) != 0) {
			debugDetailed("%s\n", "error obtaining mutex lock in lockAllRowsExcept()");
			return -1;
		}	
	}
	return 0;
	// unlock map
}

int unlockAllRows() {
	// lock map
	// loop over semaphore map and lock row
	for (const auto& x : rwSemaphores) {
		//x.first is row, x.second is semaphore
		if (pthread_mutex_unlock(&rwSemaphores[x.first]) != 0) {
			debugDetailed("%s\n", "error unlocking mutex in lockAllRowsExcept()");
			return -1;
		}
	}
	return 0;
	// unlock map
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
	return 0;
}



void printKvMap() {
    /*
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
    */
	
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
 	if (chdirRet != 0) {
 		debugDetailed("%s\n", "no checkpoint dir to cd into");	
 		if (write(STDERR_FILENO, "please create server's checkpoint directory\n", strlen("please create server's checkpoint directory\n")) < 0) {
 			perror("invalid write: ");
 		}
 		exit(-1);
 	}
}

// makes dirName directory if doesnt already exist, and cds gto the directory //returns 0 if dir exists, else -1
int chdirToRow(const char* dirName) {
	int chdirRet = chdir(dirName);
 	if (chdirRet == 0) {
 		// debugDetailed("cd into row dir (already exists): %s\n", dirName);	
 		return 0;
 	} else {
 		int mkdirRet = mkdir(dirName, 0777);
 		if (mkdirRet < 0) {
            if (write(STDERR_FILENO, "failed to create a checkpointing row directory\n", strlen( "failed to create a checkpointing row directory\n")) < 0) {
	 			perror("invalid write: ");
	 		}
	 		exit (-1); //TODO - handle this better
 		}
 		int chdirRet = chdir(dirName);
	 	if (chdirRet == 0) {
	 		// debugDetailed("cd into row dir (just created): %s\n", dirName);	
	 	} else {
            // debugDetailed("failed to cd into newly created row dir: %s\n", dirName);
            if (write(STDERR_FILENO, "failed to cd into newly created row directory\n", strlen("failed to cd into newly created row directory\n")) < 0) {
	 			perror("invalid write: ");
	 		}
	 		exit(-1);
	 	}
	 	return -1;
 	}

}


int lockPrimary() {
	if (pthread_mutex_lock(&primarySemaphore) != 0) {
		debugDetailed("%s\n", "error obtaining numComm. mutex lock");
		return -1;
	}
	return 0;
}

int unlockPrimary() {
	if (pthread_mutex_unlock(&primarySemaphore) != 0) {
		debug("%s\n", "error unlocking numComm mutex");
		return -1;
	}
	return 0;
}

int lockNumComm() {
	if (pthread_mutex_lock(&numCommandsSinceLastCheckpointSemaphore) != 0) {
		debugDetailed("%s\n", "error obtaining numComm. mutex lock");
		return -1;
	}
	// debugDetailed("LOCK numCommandsSinceLastCheckpointSemaphore: %s\n", "lock obtained, returning");
	return 0;
}

int unlockNumComm() {
	if (pthread_mutex_unlock(&numCommandsSinceLastCheckpointSemaphore) != 0) {
		debug("%s\n", "error unlocking numComm mutex");
		return -1;
	}
	return 0;
}

int lockLogFile() {
	if (pthread_mutex_lock(&logfileSemaphore) != 0) {
		debugDetailed("%s\n", "error obtaining logfile mutex lock");
		return -1;
	}
	// debugDetailed("LOCK LOGFILE: %s\n", "lock obtained, returning");
	return 0;
}

int unlockLogFile() {
	if (pthread_mutex_unlock(&logfileSemaphore) != 0) {
		debug("%s\n", "error unlocking logfile mutex");
		return -1;
	}
	// debugDetailed("UNLOCK LOGFILE: %s\n", "completed unlock, returning");
	return 0;
}


int lockCheckpoint() {
	if (pthread_mutex_lock(&checkpointSemaphore) != 0) {
		debugDetailed("%s\n", "error obtaining checkpoint mutex lock");
		return -1;
	}
	// debugDetailed("LOCK CHECKPOINT: %s\n", "lock obtained, returning");
	return 0;
}

int unlockCheckpoint() {
	if (pthread_mutex_unlock(&checkpointSemaphore) != 0) {
		debug("%s\n", "error unlocking checkpoint mutex");
		return -1;
	}
	// debugDetailed("UNLOCK CHECKPOINT: %s\n", "completed unlock, returning");
	return 0;
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

int getCurrCheckpointCnt() {
	int lastCnt;
	FILE* cntFile;
	if ((cntFile = fopen(CHECKPOINT_CNT_FILE, "r")) == NULL) {
		lastCnt = 0;
	} else {
		lastCnt = getValSize(cntFile);
		fclose(cntFile);
	}
	return lastCnt;
}

void updateCheckpointCount() {
	// try to open file - if failed (numCheckpoint is 1), else read and update
	FILE* cntFile;
	int lastCnt = getCurrCheckpointCnt();	
	if ((cntFile = fopen(CHECKPOINT_CNT_FILE, "w")) == NULL) {
		perror("error crating checkpoint cnt file: ");
	}
	fprintf(cntFile, "%d\n", lastCnt+1);
	fclose(cntFile);
}

std::string readFileToString(char* filePath) {
	std::ifstream logFile(filePath);
	std::string str((std::istreambuf_iterator<char>(logFile)), std::istreambuf_iterator<char>());
	return str;
}

resp_tuple sendUpdates(int lastLogNum) {
	// lock primary semaphore - if this node is a primary, prevent it from performing new requests
	lockPrimary();
	int myLastLogNum = getCurrCheckpointCnt();
	std::string logText;
	if (lastLogNum == myLastLogNum) {
		// send as string log.txt content
		logText = readFileToString((char*) "log.txt");
	} else {
		std::string nextLog("logArchive/log");
		nextLog = nextLog + std::to_string((lastLogNum + 1));
		logText = readFileToString((char*) nextLog.c_str());
		// send log{lastLogNum + 1} from archive folder
	}
	debugDetailed("----sendUpdates: sndr's lastLogNum: %d, myLastLogNum: %d, text response: %s\n", lastLogNum, myLastLogNum, logText.c_str());
	unlockPrimary();
	resp_tuple resp = std::make_tuple(myLastLogNum, logText);
	return resp;
}

// TODO - make sure deleted rows are handled correclty
void runCheckpoint(int evictCause) {
	int valLen;
	debugDetailed("%s\n", "RUNNING CHECKPOINT");
	lockAllRows();
	printKvMap();
	printKvLoc();
	
	// cd into checkpoint directory
	chdirToCheckpoint(); 

	// move updates to disk - loop over rows in create folder for each item in kvMap
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
				//printf("found nullllllllllllllllllllllll\n");
			}
			int loc = (*it).second;
			// debugDetailed("loop for: %s, -> %d\n", col.c_str(), loc);
			// case on kvLoc val -1 (deleted), 0 (on disk), or 1 (in local kvMap)
			if (loc == -1) {
				if(remove( col.c_str() ) != 0 ) {
                    // debugDetailed("File did not exist for col %s. Skipping!\n", col.c_str());
				} else {
                    // debugDetailed("Deleting file for col %s\n", col.c_str());
				}
                // NOTE - del has already been called and removed the val form kvMap and adjusted cache size
                it = kvLoc[row].erase(it);
                printKvLoc();
			} else if (loc == 1) {
				// debugDetailed("checkpoint writes file: %s\n", col.c_str());
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
		}
		chdir("..");
	}
	// cd back out to server directory
	chdir("..");
	
	// handle logging updates if eviction occurs due to nummCommandsSinceLastCheckpoint, 
	//if eviction due to space just move things out of local memory to disk above 
	if (evictCause == COMM) {
		updateCheckpointCount();
		// archive current log, and clear log.txt
		int lastCnt = getCurrCheckpointCnt();
		std::string archiveLog("logArchive/log");
		archiveLog = archiveLog + std::to_string(lastCnt);
		// debugDetailed("----------checkpoint archive log: %s\n", archiveLog.c_str());
		rename("log.txt", archiveLog.c_str());
		FILE* logFilePtr = fopen("log.txt", "w");
		fclose(logFilePtr);
		// debugDetailed("%s,\n", "--------moved old logfile to archive and created new--------");
		numCommandsSinceLastCheckpoint = 0;
	}
	
	// debugDetailed("--------cacheSize: %d, kvMap after checkpoint\n", cacheSize);
	printKvMap();
	printKvLoc();
	debugDetailed("%s,\n", "--------checkpoint finished and return--------");
	unlockAllRows();
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
	if (unlockCheckpoint() < 0) {
		exit(-1);
	}
	return ret;

}


// returns 0 if there was space, and -1 if not
int readAndLoadValIfSpace(FILE* fptr, int valLen, int cacheThresh, char* row, char* col) {
	// check if space
	lockRow(row);
	if (cacheSize + valLen <= cacheThresh) {
		// read in val 
		char* val = (char*) calloc(valLen, sizeof(char));
		if (val != NULL) {
			fread(val, sizeof(char), valLen, fptr);
		} else {
			perror("cannot calloc value in readAndLoadValIfSpace()");
			unlockRow(row);
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
		unlockRow(row);
		return 0;
	} else {
		// update kvLoc
		std::string rowString(row);
		std::string colString(col);
		kvLoc[rowString][colString] = 0;
		unlockRow(row);
		return -1;
	}
}


// call on server start
int loadKvStoreFromDisk() {
	// loop over all files in checkpoint dir and read each row file
	chdirToCheckpoint();
	DIR* dir = opendir(".");
	struct dirent* nextRowDir;
	
	FILE* colFilePtr; 
	char headerBuf[MAX_LEN_LOG_HEADER];
	memset(headerBuf, 0, sizeof(char) * MAX_LEN_LOG_HEADER);
	while((nextRowDir = readdir(dir)) != NULL) {
        if(strcmp(nextRowDir->d_name, ".") == 0 || strcmp(nextRowDir->d_name, "..") == 0) {
            // Skip "." and ".." entries
            continue;
        }
		// cd into next row directory
		if (chdirToRow(nextRowDir->d_name) < 0) {
			debugDetailed("error opening row directory %s\n", nextRowDir->d_name);
			fprintf(stderr, "error opening row directory %s\n", nextRowDir->d_name);
		}
		// loop over column files
		DIR* colDir = opendir(".");
		struct dirent* nextColFile;
		while((nextColFile = readdir(colDir)) != NULL) {
            if(strcmp(nextColFile->d_name, ".") == 0 || strcmp(nextColFile->d_name, "..") == 0) {
                // Skip "." and ".." entries
                continue;
            }
			//open the column file
			if ((colFilePtr = fopen(nextColFile->d_name, "r")) == NULL) {
				debugDetailed("fopen failed when opening %s\n", nextColFile->d_name);
				perror("invalid fopen of a checkpoint col file: ");			
				debugDetailed("%s\n", "---------Finished loadKvStoreFromDisk WITH ERROR");
				// return -1;
                exit(-1);
			}
			// read from column file the formatted length
			if ((fgets(headerBuf, MAX_LEN_LOG_HEADER, colFilePtr)) == NULL) {
				perror("invalid fgets when trying to read col file reader: ");	
				// return -1;
                exit(-1);
			}
			headerBuf[strlen(headerBuf)] = '\0'; // set newlien to null
			int valLen = (atoi(headerBuf));
			
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
	return 0;	
}



// increments command count since last checkpoint and deals with checkpointing
void checkIfCheckPoint() {
	// claim semaphore on numCommandsSinceLastCheckpoint
	if (lockNumComm() < 0) {
		perror("lock failure: ");
		return;
	}
	numCommandsSinceLastCheckpoint = numCommandsSinceLastCheckpoint + 1;
	debugDetailed("NUM completed commands: %d\n", numCommandsSinceLastCheckpoint);
	if (numCommandsSinceLastCheckpoint >= COM_PER_CHECKPOINT) {
		// debugDetailed("%s\n", "triggering checkpoint");
		runCheckpoint(COMM); // Note this function accesses and writes to numCommandsSinceLastCheckpoint
	} 
	// release semaphore on numCommandsSinceLast Checkpoint
	if (unlockNumComm() < 0) {
		perror("lock failure: ");
		return;
	}
	
}

// write to log.txt if new command came in and handle checkpointing, arg1 is row, arg2 is col
int logCommand(enum Command comm, int numArgs, std::string arg1, std::string arg2, std::string arg3, std::string arg4) {
	// check what command is 
	FILE* logfile;
	if (replay == 0) {
		// debugDetailed("%s\n", "logging command, replay flag off");
		if (lockLogFile() < 0) {
			return -1;
		}
		if ((logfile = fopen("log.txt", "a")) == NULL) {
			// debugDetailed("could not open log.txt for server %d\n", serverIdx);
			perror("invalid fopen of log file: ");
			unlockLogFile();
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
			// debugDetailed("invalid number of args (%d) in logCommand\n", numArgs);
			fprintf(stderr, "invalid paramaters in logCommand\n");
		}

		fwrite("\n", sizeof(char), strlen("\n"), logfile);

		fclose(logfile);
		if (unlockLogFile() < 0) {
			return -1;
		}
	}
	return 0;
}


// get val if known to exist
std::string getValDiskorLocal(std::string row, std::string col) {
	std::string val;
	lockRow(row);
	if (kvLoc[row][col] == 0) { // TODO would need to claim semaphore here in this case on checkpoint where eviction happens
		// debugDetailed("%s\n", "row, col, val on disk, retrieiving..");
		// try to load from disk, and run checkpoint if needed
		FILE* colFilePtr = openValFile((char*) row.c_str(), (char*) col.c_str(), "r");
		int valLen = getValSize(colFilePtr);
		unlockRow(row);
		int loadRes = readAndLoadValIfSpace(colFilePtr, valLen, maxCache, (char*) row.c_str(), (char*) col.c_str());
		if (loadRes == -1) {
			runCheckpoint(MEM);
			readAndLoadValIfSpace(colFilePtr, valLen, maxCache, (char*) row.c_str(), (char*) col.c_str());
		}	
	}
	unlockRow(row);
	val = kvMap[row][col];
	return val;
}

// helper to compare responses
resp_tuple combineResps(std::map<std::string, resp_tuple> respSet) {
	// debugDetailed("---combineReps: %s\n", "entered");
	resp_tuple firstVal = std::make_tuple(0, "");
	resp_tuple resp = std::make_tuple(0, "");
	for (const auto& x : respSet) {
		//x.first is ip:port, x.second is resp_tuple
		// debugDetailed("---combineReps: %s\n", "respset iter");
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
	// debugDetailed("---combineReps: %s\n", "completed loop over respSet");
	if (firstVal == std::make_tuple(0, "")) {
		debugDetailed("%s\n", "error in combine resps - empty respSet arg");
		resp = std::make_tuple(-1, "Error: empty resp set");
	}
	// debugDetailed("---combineReps: %s\n", "returned");
	return resp;
}



resp_tuple put(std::string row, std::string col, std::string val) {
    // debugDetailed("---PUT entered - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
    debugDetailed("---PUT entered - row: %s, column: %s, val length: %ld\n", row.c_str(), col.c_str(), val.length());
    int oldLen = kvMap[row][col].length();
    int ranCheckPoint = 0; // need this flag to properly update chacheSize

    // check if can store in local cache
    if (val.length() + cacheSize - oldLen > maxCache) {
    	debugDetailed("------PUT evicting on valLen: %ld, cacheSize: %d, maxCache: %d\n", val.length(), cacheSize, maxCache);
    	runCheckpoint(MEM);
    	ranCheckPoint = 1;
    }
    if (val.length() + cacheSize - oldLen <= maxCache) {
    	// put into kvMap, upate cache size and set kvLoc = 1
    	if (lockRow(row) < 0) {
			return std::make_tuple(1, "ERR");
		}
    	kvMap[row][col] = val;
    	kvLoc[row][col] = 1;
    	
    	cacheSize = cacheSize + val.length();
    	if (ranCheckPoint == 0) {
    		cacheSize = cacheSize - oldLen;
    	}
    	// debugDetailed("------PUT row: %s, column: %s, val: %s, cahceSize: %d\n", row.c_str(), col.c_str(), val.c_str(), cacheSize);
        debugDetailed("---PUT row - row: %s, column: %s, val length: %ld\n", row.c_str(), col.c_str(), val.length());
        printKvMap();
    	logCommand(PUT, 3, row, col, val, row);
    	if (unlockRow(row) < 0) {
			return std::make_tuple(1, "ERR");
		}
		checkIfCheckPoint();
    	return std::make_tuple(0, "OK");
    } else {
    	// evict everything then rerun the function (but dont log the second time)
    	// debugDetailed("------PUT evicting FAILED row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
    	debugDetailed("------PUT evicting FAILED row: %s, column: %s, val length: %ld\n", row.c_str(), col.c_str(), val.length());
    	fprintf(stderr, "memCache overfilled\n");
    	exit(0);
    }
}

std::tuple<int, std::string> cput(std::string row, std::string col, std::string expVal, std::string newVal) {
	// debugDetailed("---CPUT entered - row: %s, column: %s, expVal: %s, newVal: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
	debugDetailed("---CPUT entered - row: %s, column: %s, expVal length: %ld, newVal length: %ld\n", row.c_str(), col.c_str(), expVal.length(), newVal.length());

	lockRow(row);
	if (kvLoc.count(row) > 0) {
		if (kvLoc[row].count(col) > 0) {
			if (kvLoc[row][col] != -1) {
				unlockRow(row);
				std::string val = getValDiskorLocal(row, col);	
				// debugDetailed("------CPUT correct val: %s\n", val.c_str());	
				lockRow(row);
				if (expVal.compare(kvMap[row][col]) == 0) {
					unlockRow(row);
					put(row, col, newVal);
					// debugDetailed("------CPUT called put and updated val - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
                    debugDetailed("---CPUT called put and updated val - row: %s, column: %s, expVal length: %ld, newVal length: %ld\n", row.c_str(), col.c_str(), expVal.length(), newVal.length());
                    printKvMap();
					return std::make_tuple(0, "OK");
				} else {
					unlockRow(row);
					// debugDetailed("------CPUT did not update - row: %s, column: %s, old val: %s, new val: %s\n", row.c_str(), col.c_str(), expVal.c_str(), newVal.c_str());
                    debugDetailed("---CPUT did not update - row: %s, column: %s, expVal length: %ld, newVal length: %ld\n", row.c_str(), col.c_str(), expVal.length(), newVal.length());
                    printKvMap();
					return std::make_tuple(2, "Incorrect expVal");
				}
			} else {
				unlockRow(row);
			}
		} else {
			unlockRow(row);
		}
	} else {
		unlockRow(row);
	}
	
	debugDetailed("------CPUT did not update - row: %s, column: %s, old val length: %ld, new val length: %ld\n", row.c_str(), col.c_str(), expVal.length(), newVal.length());
    printKvMap();
	return std::make_tuple(1, "No such row, column pair");
}

std::tuple<int, std::string> del(std::string row, std::string col) {
	debugDetailed("%s\n", "del entered");
    printKvMap();
	printKvLoc();
	lockRow(row);
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
				unlockRow(row);
				logCommand(DELETE, 2, row, col, row, row);
				return std::make_tuple(0, "OK");
			} else {
				unlockRow(row);
			}
			
		} else {
			unlockRow(row);
		}
	} else {
		unlockRow(row);
	}
	
	debugDetailed("---DELETE val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	//logCommand(DELETE, 2, row, col, row, row);
	return std::make_tuple(1, "No such row, column pair");
}


bool checkIfNodeIsAlive(std::string otherServer) {
    // connect to heartbeat thread of backend server and check if it's alive w/ shorter timeout
    std::string otherServerHeartbeatIP = serverKVSPortToAdminPortMap[otherServer];
    debugDetailed("Checking heartbeat for %s at heartbeat address: %s\n", otherServer.c_str(), otherServerHeartbeatIP.c_str());
    int heartbeatPortNo = getIPPort(otherServerHeartbeatIP);
    std::string heartbeatAddress = getIPAddr(otherServerHeartbeatIP);
    rpc::client kvsHeartbeatRPCClient(heartbeatAddress, heartbeatPortNo);
    kvsHeartbeatRPCClient.set_timeout(2000); // 2000 milliseconds
    try {
        bool isAlive = kvsHeartbeatRPCClient.call("heartbeat").as<bool>();
        debugDetailed("Heartbeat for %s returned true!\n", otherServer.c_str());
        return isAlive;
    } catch(rpc::timeout &t) {
        debugDetailed("Heartbeat for %s failed to return. Node is dead.\n", otherServer.c_str());
        return false;
    }
    return false;
}

// TODO (AMIT): 1) add timeout in primary and nonprimary case for calling put or put approved
			// if timeout - call heartbeat method (which is on admit thread) with shorter timeout
							// if timeout occurs, you know node is dead and can skip it
							// else need to call put / put approved again with timeout

// oldVal used for CPUT
resp_tuple kvsFuncReq(std::string kvsFunc, std::string row, std::string col, std::string val, std::string oldVal) {
    while(true) {
        debugDetailed("Beginning %s Request with (r, c): (%s, %s)\n", kvsFunc.c_str(), row.c_str(), col.c_str());
        std::map<std::string, resp_tuple> respSet; // map from ip:port -> resp tuple
        resp_tuple resp;
        // handle whether of not receiving node is a primary
        if (isPrimary()) {
            debugDetailed("Current Node %s is primary for %s Request\n", myAddrPortForFrontend.c_str(), kvsFunc.c_str());
            // If node is primary, perform local kvs operation and loop over memebers in cluster and call operation (synchronous w timeout) on them
            lockPrimary();
            if(kvsFunc.compare("PUT") == 0) {
                respSet[myAddrPortForFrontend] = put(row, col, val);
            } else if(kvsFunc.compare("CPUT") == 0) {
                respSet[myAddrPortForFrontend] = cput(row, col, oldVal, val);
            } else if(kvsFunc.compare("DEL") == 0) {
                respSet[myAddrPortForFrontend] = del(row, col);
            } else {
                debugDetailed("%s is an invalid function for kvsFuncReq()!\n", kvsFunc.c_str());
                return std::make_tuple(1, "ERR");
            }
            debugDetailed("Local %s completed on primary\n", kvsFunc.c_str());
            for (std::string otherServer : clusterMembers) {
                if(otherServer.compare(myAddrPortForFrontend) != 0 && clusterNodesToSkip.count(otherServer) <= 0) {
                    bool continueTrying = checkIfNodeIsAlive(otherServer);
                    if(!continueTrying) {
                        debugDetailed("Node %s is dead! Adding node to set of dead nodes.\n", otherServer.c_str());
                        // Add node to set of dead nodes (nodes to skip)
                        clusterNodesToSkip.insert(otherServer);
                    }
                    uint64_t timeout = TIMEOUT_MILLISEC;
                    while(continueTrying) {
                        rpc::client client(getIPAddr(otherServer), getIPPort(otherServer));
                        try { // TODO - on timeout, query connection state and retry if connected
                            client.set_timeout(timeout);
                            debugDetailed("Trying remote %s to: %s with timeout %ld\n", kvsFunc.c_str(), otherServer.c_str(), timeout);
                            if(kvsFunc.compare("PUT") == 0) {
                                resp = client.call("putApproved", row, col, val).as<resp_tuple>();
                                debug("putApproved returned: %s\n", std::get<1>(resp).c_str());
                            } else if(kvsFunc.compare("CPUT") == 0) {
                                resp = client.call("cputApproved", row, col, oldVal, val).as<resp_tuple>();
                            } else if(kvsFunc.compare("DEL") == 0) {
                                resp = client.call("delApproved", row, col).as<resp_tuple>();
                            } else {
                                debugDetailed("%s is an invalid function for kvsFuncReq()!\n", kvsFunc.c_str());
                                return std::make_tuple(1, "ERR");
                            }
                            continueTrying = false;
                            respSet[otherServer] = resp;
                        } catch (rpc::timeout &t) {
                            debugDetailed("%s for (%s, %s) with server %s timed out!\n", kvsFunc.c_str(), row.c_str(), col.c_str(), otherServer.c_str());
                            // connect to heartbeat thread of backend server and check if it's alive w/ shorter timeout
                            bool nodeIsAlive = checkIfNodeIsAlive(otherServer);
                            if(nodeIsAlive) {
                                // Double timeout and try again if node is still alive
                                timeout *= 2;
                                // assuming that request is canceled on timeout
                                debugDetailed("Node %s is still alive! Doubling timeout to %ld and trying again.\n", otherServer.c_str(), timeout);
                            } else {
                                debugDetailed("Node %s is dead! Adding node to set of dead nodes.\n", otherServer.c_str());
                                // Add node to set of dead nodes (nodes to skip)
                                clusterNodesToSkip.insert(otherServer);
                                continueTrying = false;
                            }
                        } catch (rpc::rpc_error &e) {
                            printf("SOMETHING BAD HAPPENED\n");
                            std::cout << std::endl << e.what() << std::endl;
                            std::cout << "in function " << e.get_function_name() << ": ";
                            using err_t = std::tuple<int, std::string>;
                            auto err = e.get_error().as<err_t>();
                            std::cout << "[error " << std::get<0>(err) << "]: " << std::get<1>(err)
                                      << std::endl;
                            unlockPrimary();
                            return std::make_tuple(1, "ERR");
                        }
                    }
                }
            }
            // release semaphor on row
            unlockPrimary();
            debugDetailed("%s: Primary calling combineResps\n", kvsFunc.c_str());
            // return resp tuple
            resp = combineResps(respSet);
            printKvLoc();
            return resp;
        } else {
            debugDetailed("Current Node %s is NOT primary for %s Request. Forwarding to primary: %s\n", myAddrPortForFrontend.c_str(), kvsFunc.c_str(), myClusterLeader.c_str());
            // send put Req to primary
            bool continueTrying = checkIfNodeIsAlive(myClusterLeader);
            if(!continueTrying) {
                debugDetailed("Primary Node %s is dead! Adding node to set of dead nodes.\n", myClusterLeader.c_str());
                // Add node to set of dead nodes (nodes to skip)
                clusterNodesToSkip.insert(myClusterLeader);
                // Get new cluster leader and try again
                rpc::client masterNodeRPCClient(getIPAddr(masterNodeAddrPort), getIPPort(masterNodeAddrPort));
                resp_tuple newLeaderResp = masterNodeRPCClient.call("getNewClusterLeader", myClusterLeader).as<resp_tuple>();
                myClusterLeader = std::get<1>(newLeaderResp);
                continueTrying = true;
            }
            uint64_t timeout = TIMEOUT_MILLISEC;
            while(continueTrying) {
                debugDetailed("Starting new rpc::client to primary %s\n", myClusterLeader.c_str());
                rpc::client clusterLeaderClient(getIPAddr(myClusterLeader), getIPPort(myClusterLeader));
                debugDetailed("Trying to forward %s to: primary with timeout %ld\n", kvsFunc.c_str(), timeout);
                try {
                    clusterLeaderClient.set_timeout(timeout);
                    if(kvsFunc.compare("PUT") == 0) {
                        resp = clusterLeaderClient.call("put", row, col, val).as<resp_tuple>(); // TODO - add the time out and possible re-leader election here
                    } else if(kvsFunc.compare("CPUT") == 0) {
                        resp = clusterLeaderClient.call("cput", row, col, oldVal, val).as<resp_tuple>();
                    } else if(kvsFunc.compare("DEL") == 0) {
                        resp = clusterLeaderClient.call("del", row, col).as<resp_tuple>();
                    } else {
                        debugDetailed("%s is an invalid function for kvsFuncReq()!\n", kvsFunc.c_str());
                        return std::make_tuple(1, "ERR");
                    }
                    printKvLoc();
                    return resp;
                } catch (rpc::timeout &t) {
                    debugDetailed("Attempt to forward %s to primary %s timed out!\n", kvsFunc.c_str(), myClusterLeader.c_str());
                    // connect to heartbeat thread of backend server and check if it's alive w/ shorter timeout
                    bool nodeIsAlive = checkIfNodeIsAlive(myClusterLeader);
                    if(nodeIsAlive) {
                        // Double timeout and try again if node is still alive
                        timeout *= 2;
                        debugDetailed("Primary Node %s is still alive! Doubling timeout to %ld and trying again.\n", myClusterLeader.c_str(), timeout);
                    } else {
                        debugDetailed("Primary Node %s is dead! Adding node to set of dead nodes.\n", myClusterLeader.c_str());
                        // Add node to set of dead nodes (nodes to skip)
                        clusterNodesToSkip.insert(myClusterLeader);
                        // Get new cluster leader and try again
                        rpc::client masterNodeRPCClient(getIPAddr(masterNodeAddrPort), getIPPort(masterNodeAddrPort));
                        resp_tuple newLeaderResp = masterNodeRPCClient.call("getNewClusterLeader", myClusterLeader).as<resp_tuple>();
                        myClusterLeader = std::get<1>(newLeaderResp);
                        continueTrying = false;
                    }
                }
            }
        }
    }
    return std::make_tuple(-1, "ERR");
}

resp_tuple putReq(std::string row, std::string col, std::string val) {
    return kvsFuncReq("PUT", row, col, val, "");
}

resp_tuple cputReq(std::string row, std::string col, std::string oldVal, std::string newVal) {
    return kvsFuncReq("CPUT", row, col, newVal, oldVal);
}

resp_tuple delReq(std::string row, std::string col) {
    return kvsFuncReq("DEL", row, col, "", "");
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
				if (unlockRow(row) < 0) {
					return std::make_tuple(1, "ERR");
				}
				std::string val = getValDiskorLocal(row, col);			
				// debugDetailed("---GET succeeded - row: %s, column: %s, val: %s\n", row.c_str(), col.c_str(), val.c_str());
				debugDetailed("---GET succeeded - row: %s, column: %s, val length: %ld\n", row.c_str(), col.c_str(), val.length());
				printKvMap();
				//logCommand(GET, 2, row, col, row, row);
				return std::make_tuple(0, val);
			} else {
				unlockRow(row);
			}
		} else {
			unlockRow(row);
		}
	} else {
		unlockRow(row);
	}
	
	debugDetailed("---GET val not found - row: %s, column: %s\n", row.c_str(), col.c_str());
	printKvMap();
	// release semaphor on row
	
	//logCommand(GET, 2, row, col, row, row);
	debugDetailed("---GET :%s\n", "returning");
	return std::make_tuple(1, "No such row, column pair");	
}

std::tuple<int, int, std::string> getFirstNBytes(std::string row, std::string col, int numBytes) {
    resp_tuple resp = get(row, col);
    std::string val = std::get<1>(resp);
    int origValLength = val.length();
    if(val.length() > numBytes) {
        val = val.substr(0, numBytes);
    }
    if(std::get<0>(resp) == 0) {
        return std::make_tuple(0, origValLength, val);
    }
    return std::make_tuple(std::get<0>(resp), 0, "Error! Row+Col does not exist");
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
	FILE* logfile;
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
		//debugDetailed("buf read from log file is: %s\n", headerBuf);
		comm = strtok(headerBuf, ","); // this should never be null if headerBuf is well formatted
		for (i = 0; i < MAX_COMM_ARGS; i++) {
			lens[i] = atoi(strtok(NULL, ","));
			//debugDetailed("---replayLog: lens[%d] = %d\n", i, lens[i]);
		}
		for (i = 0; i < MAX_COMM_ARGS; i++) {
			if (lens[i] != 0) {
				//debugDetailed("---replayLog: calloc'd args[%d] for lens[%d] = %d\n", i, i, lens[i]);
				args[i] = (char*) calloc(lens[i], sizeof(char));
			} else {
				//debugDetailed("---replayLog: calloc'd args[%d] is NULL\n", i);
				args[i] = NULL;
			}
			if (args[i] != NULL) {
				//debugDetailed("---replayLog: reading into args[%d], lens[%d] = %d\n", i, i, lens[i]);
				fread(args[i], sizeof(char), lens[i], logfile);
			}
		}

		// NOTE: uncomment lines below for debug statements, but this will casue 3 memory erros from one context when run in valgrind
		//debugDetailed("header args - comm: %s, len1: %d, len2: %d, len3: %d, len4: %d\n", comm, lens[0], lens[1], lens[2], lens[3]);
		//debugDetailed("parsed args - arg1: %s, arg2: %s, arg3: %s, arg4: %s\n", args[0], args[1], args[2], args[3]);
		// debugDetailed("---replayLog: %s\n", "calling function");
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

int notifyOfNewLeader(std::string newLeader) {
    debug("Notified of new leader. Cluster leader set to %s\n", newLeader.c_str());
    myClusterLeader = newLeader;
    return 0;
}

int notifyOfNewNode(std::string newNode) {
    debug("New node %s in cluster!\n", newNode.c_str());
    if(clusterNodesToSkip.count(newNode) > 0) {
        debug("Node %s in cluster was previously dead. Removing from set of dead nodes!\n", newNode.c_str());
        clusterNodesToSkip.erase(newNode);
    }
    return 0;
}

void registerWithMasterNode() {
    debug("Registering with masterNode at %s\n", masterNodeAddrPort.c_str());
	rpc::client masterNodeRPCClient(getIPAddr(masterNodeAddrPort), getIPPort(masterNodeAddrPort));
    std::tuple<int, std::string> resp = masterNodeRPCClient.call("registerWithMaster", myAddrPortForFrontend).as<std::tuple<int, std::string>>();
    myClusterLeader = std::get<1>(resp);
    debug("%s\n", "Finished registering with masterNode");
    debug("Cluster leader set to %s\n", myClusterLeader.c_str());
}

void getClusterNodes() {
    debug("getClusterNodes: Requesting cluster nodes from masterNode at %s\n", masterNodeAddrPort.c_str());
	rpc::client masterNodeRPCClient(getIPAddr(masterNodeAddrPort), getIPPort(masterNodeAddrPort));
    std::deque<server_addr_tuple> resp = masterNodeRPCClient.call("getClusterNodes", myAddrPortForFrontend).as<std::deque<server_addr_tuple>>();

    for(server_addr_tuple serverInfo : resp) {
        std::string serverKVSAddrPort = std::get<2>(serverInfo);
        std::string serverAdminAddrPort = std::get<3>(serverInfo);
        debug("Adding cluster member: (%s, %s)\n", serverKVSAddrPort.c_str(), serverAdminAddrPort.c_str());
        clusterMembers.push_back(serverKVSAddrPort);
        serverKVSPortToAdminPortMap[serverKVSAddrPort] = serverAdminAddrPort;
    }
    debug("%s\n", "Finished retrieving all cluster members from masterNode");
}

void sigTermHandler(int signum) {
    if (signum == SIGTERM) {
        pthread_exit(NULL);
    }
}

void getLoggingUpdates() {
	//int myLastLogNum = getCurrCheckpointCnt();
	// TODO need to add primary choosing here
	// debugDetailed("---getLoggingUpdates: %s\n", "entered");
	if (isPrimary()) {
		// debugDetailed("---getLoggingUpdates: %s\n", "returned - primary");
		return;
	}

	rpc::client client(getIPAddr(myClusterLeader), getIPPort(myClusterLeader));
    
    //resp_tuple resp = client.call("sendUpdates", myLastLogNum).as<resp_tuple>(); // TODO - add the time out and possible re-leader election here
    //int primaryLogNum = std::get<0>(resp);
    // while (primaryLogNum != myLastLogNum) {
    // 	// write log to file, replayLog, runCheckpnt
    // 	FILE* nextLogFile = fopen("log.txt", "w");
    // 	fwrite(std::get<1>(resp_tuple).c_str(), sizeof(char), std::get<1>(resp_tuple).length(), nextLogFile);
    // 	fclose(nextLogFile);
    // 	replayLog(); // this will trigger a checkpoint
    // }
    int myLastLogNum = getCurrCheckpointCnt();
   	int primaryLogNum = 0;
    do {
    	myLastLogNum = getCurrCheckpointCnt();
    	resp_tuple resp = client.call("sendUpdates", myLastLogNum).as<resp_tuple>(); 
    	primaryLogNum = std::get<0>(resp);
    	std::string primaryLog = std::get<1>(resp);
    	
    	FILE* logFile = fopen((char*) "log.txt", "w");
    	fwrite(primaryLog.c_str(), sizeof(char), primaryLog.length(), logFile);
    	fclose(logFile);
    	replayLog(); // this will trigger a checkpoint	
    } while (myLastLogNum != primaryLogNum);
    // debugDetailed("---getLoggingUpdates: %s\n", "returned - non-primary");
    return;

}


// Requests latest log files from primary node via getLoggingUpdates(). Should be run on boot to ensure data is
// up-to-date.
void loadLatestKVMapOnBoot() {
	// Hardcoding admin login info on boot
    if (isPrimary()) {
    	resp_tuple resp = get("Admin", "password");
    	if (std::get<0>(resp) == 1) {
    		// if admin not already in backend add it
    		std::string adminRow("Admin");
			std::string adminCol("password");
			std::string adminVal("password");
			put(adminRow, adminCol, adminVal);
    	}
    }
 	// debug("%s\n", "kvMap before log replay or checkpoint: ");
 	printKvMap();
 	loadKvStoreFromDisk();
 	// debug("%s\n", "kvMap before log replay: ");
 	printKvMap();
 	// get updates and most recent logfile from leader -- TODO: need to add function to get leader
 	getLoggingUpdates(); // this function will get logging updates and replay them as needed
 	// debug("%s\n", "kvMap after log replay: ");
 	printKvMap();
 	printKvLoc();
 	// debug("numCommandsSinceLastCheckpoint: %d\n", numCommandsSinceLastCheckpoint);
}

void *kvServerThreadFunc(void *arg) {
    signal(SIGTERM, sigTermHandler);
    pthread_detach(pthread_self());

    registerWithMasterNode();
    getClusterNodes();
    loadLatestKVMapOnBoot();

	


    debug("Server thread %lu started to communicate with frontend servers on port: %d\n", kvServerWithFrontendThreadId, kvsPortForFrontend);
	rpc::server srv(kvsPortForFrontend);

	srv.bind("putApproved", &put); 
	srv.bind("put", &putReq);
	srv.bind("sendUpdates", &sendUpdates);
	srv.bind("cputApproved", &cput); 
	srv.bind("cput", &cputReq);
	srv.bind("delApproved", &del);
	srv.bind("del", &delReq);
	srv.bind("get", &get);
	srv.bind("notifyOfNewLeader", &notifyOfNewLeader);
	srv.bind("notifyOfNewNode", &notifyOfNewNode);

	srv.async_run(30);

	while(1) {
		sleep(10);
	}

    pthread_exit(0);
}

void adminKillServer() {
    if(kvServerWithFrontendThreadId > 0) {
        debug("killServer command received from Admin console. Killing server thread %lu communicating with frontend servers.\n", kvServerWithFrontendThreadId);
        pthread_kill(kvServerWithFrontendThreadId, SIGTERM);
        kvServerWithFrontendThreadId = 0;
        debug("%s\n", "kvsServerWithFrontendThreadId reset to 0");
    } else {
        debug("%s\n", "Server thread for frontend does not exist! Ignoring kill command received from Admin console.");
    }
}

void adminReviveServer() {
    if(kvServerWithFrontendThreadId == 0) {
        debug("%s\n", "reviveServer command received from Admin console. Reviving server thread to communicate with frontend servers.");
        pthread_create(&kvServerWithFrontendThreadId, NULL, &kvServerThreadFunc, NULL);
    } else {
        debug("Server thread %lu for frontend already exists! Ignoring revive command received from Admin console.\n", kvServerWithFrontendThreadId);
    }
}

// Returns true if the kv thread is running. Used to check if server is alive.
bool heartbeat() {
    debug("Heartbeat requested! Returning %d\n", (kvServerWithFrontendThreadId != 0));
    return (kvServerWithFrontendThreadId != 0);
}

void signalHandler(int sig) {
	debugDetailed("%s\n", "signal caught by server");	
	
	// TODO - notify master that I'm dead
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

std::deque<std::string> getAllRows() {
    debug("%s\n", "All rows requested!");
    std::deque<std::string> allRows;
    for(std::map<std::string,std::map<std::string, int>>::iterator it = kvLoc.begin(); it != kvLoc.end(); ++it) {
      allRows.push_back(it->first);
    }
    debug("All rows requested. Returning %ld rows.\n", allRows.size());
    return allRows;
}

std::deque<std::string> getAllColsForRow(std::string row) {
    debug("All columns for row %s requested!\n", row.c_str());
    std::map<std::string, int> cols = kvLoc[row];
    std::deque<std::string> allCols;
    for(std::map<std::string, int>::iterator it = cols.begin(); it != cols.end(); ++it) {
      allCols.push_back(it->first);
    }
    debug("All columns for row %s requested. Returning %ld columns.\n", row.c_str(), allCols.size());
    return allCols;
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
	maxCache = rlim->rlim_cur/2;
	// maxCache = 12;
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
    while(fgets(buffer, 300, f)){
        std::string line = std::string(buffer);
        if(line.at(line.length()-1) == '\n') {
            line = line.substr(0, line.length()-1);
        }
        debug("Reading from config file: %s\n ", line.c_str());
        if(serverNum == serverIdx) {
            myAddrPortForFrontend = line.substr(0, line.find(","));
            myAddrPortForAdmin = line.substr(line.find(",")+1);
            try {
                kvsPortForFrontend = getIPPort(myAddrPortForFrontend);
                kvsPortForAdmin = getIPPort(myAddrPortForAdmin);
                if(kvsPortForFrontend <= 0 || kvsPortForAdmin <= 0) {
                    fprintf(stderr, "Invalid port provided! Exiting\n");
                    exit(-1);
                }
            } catch (const std::invalid_argument &ia) {
                fprintf(stderr, "Server port not found! Exiting\n");
                exit(-1);
            }
        } else if(serverNum == 0) {
            masterNodeAddrPort = line;
        }
        serverNum += 1;
    }
    fclose(f);

    //initialize semaphores
    if (pthread_mutex_init(&checkpointSemaphore, NULL) != 0) {
    	perror("invalid pthread_mutex_init for checkpointSemaphore:");
		return -1;
    }
    if (pthread_mutex_init(&cacheSizeSemaphore, NULL) != 0) {
    	perror("invalid pthread_mutex_init for cacheSizeSemaphore:");
		return -1;
    }
    if (pthread_mutex_init(&numCommandsSinceLastCheckpointSemaphore, NULL) != 0) {
    	perror("invalid pthread_mutex_init for numCommandsSinceLastCheckpointSemaphore:");
		return -1;
    }
    if (pthread_mutex_init(&logfileSemaphore, NULL) != 0) {
    	perror("invalid pthread_mutex_init for logfile:");
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

    pthread_create(&kvServerWithFrontendThreadId, NULL, &kvServerThreadFunc, NULL);

    debug("Main thread starting RPC Server to communicate with admin console on port: %d\n", kvsPortForAdmin);

	rpc::server srv(kvsPortForAdmin);
	srv.bind("killServer", &adminKillServer);
	srv.bind("reviveServer", &adminReviveServer);
	srv.bind("heartbeat", &heartbeat);
	srv.bind("getAllRows", &getAllRows);
	srv.bind("getAllColsForRow", &getAllColsForRow);
	srv.bind("getFirstNBytes", &getFirstNBytes);
	srv.run();

    return 0;
}
