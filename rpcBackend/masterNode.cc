#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <rpc/server.h>
#include <errno.h>
#include <signal.h>
#include <sys/file.h>
#include <iostream>
#include <tuple>
#include <string>
#include <valarray>
#include <map>

enum Command {GET, PUT, CPUT, DELETE};
bool debugFlag;
bool testMode;
int serverIndx = 1;
int numClusters = 1;
std::valarray<int> rowVals (36);
std::map<int, std::list<std::string>> clusterToServersMap;
std::string masterNodeAddr;

void stderr_msg(std::string str) {
    std::cerr << str << "\n";
}

void log(std::string str) {
	if (debugFlag)
        stderr_msg(str);
}

// Returns cluster that contains row-col-val based on first letter of row
// Returns 0 w/ list of servers in cluster on success
// Returns -1 w/ empty list on failure
std::tuple<int,std::list<std::string>> where(std::string row) {
    log("Received WHERE: " + row);
    std::list<std::string> serverList {};
    if(row.length() <= 0 || !isalnum(row.at(0))) {
        // error
         return std::make_tuple(-1, serverList);
    }

    if(testMode) {
        serverList.push_back("127.0.0.1:10000");
        serverList.push_back("127.0.0.1:10000");
        return std::make_tuple(0, serverList);
    }

    char firstChar = toupper(row.at(0));
    if(firstChar >= '0' && firstChar <= '9') {
         return std::make_tuple(0, clusterToServersMap[0 % numClusters]);
    } else if(firstChar >= 'A' && firstChar <= 'I') {
         return std::make_tuple(0, clusterToServersMap[1 % numClusters]);
    } else if(firstChar >= 'J' && firstChar <= 'R') {
         return std::make_tuple(0, clusterToServersMap[2 % numClusters]);
    } else if(firstChar >= 'S' && firstChar <= 'Z') {
         return std::make_tuple(0, clusterToServersMap[3 % numClusters]);
    }
    serverList.clear();
    return std::make_tuple(-1, serverList);
}

int main(int argc, char *argv[]) {	
	int opt;
	int port = 8000;
	debugFlag = false;
    char* serverListFile = NULL;

	// parse arguments -p <portno>, -a for full name printed, -v for debug output
	while ((opt = getopt(argc, argv, "p:avt")) != -1) {
		switch(opt) {
			case 'p':
				port = atoi(optarg);
				break;
			case 'a':
				stderr_msg("Amit Lohe (alohe)");
		 		return 0;
				break;
			case 't':
				testMode = true;
				stderr_msg("testMode is on - please launch a single backend server on 127.0.0.1:10000");
				break;
			case 'v':
				debugFlag = true;
				stderr_msg("DEBUG is on");
				break;
		}
	}

    if(optind < argc) {
        serverListFile = argv[optind];
    }

    FILE * f = fopen(serverListFile, "r");
    if(f == NULL) {
        stderr_msg("Provide a valid list of backend servers");
        exit(-1);
    }
    int serverNum = -1;
    char buffer[300];
    while(fgets(buffer, 300, f)){
        std::string server = std::string(buffer);
        // first line is address for master node
        if(serverNum != -1) {
            if(server.at(server.length()-1) == '\n') {
                server = server.substr(0, server.length()-1);
            }
            int currCluster = serverNum/3;
            if(clusterToServersMap.count(currCluster) <= 0) {
                clusterToServersMap[currCluster] = std::list<std::string> {};
            }
            clusterToServersMap[currCluster].push_back(server);
        } else {
            masterNodeAddr = server;
            try {
                port = stoi(masterNodeAddr.substr(masterNodeAddr.find(":")+1));
            } catch (const std::invalid_argument &ia) {
                stderr_msg("Master port not found! masterNode port set to 8000");
            }
        }
        serverNum += 1;
    }
    numClusters = serverNum / 3;
    fclose(f);

	rpc::server srv(port);
	srv.bind("where", &where);

	srv.run();

    return 0;
}
