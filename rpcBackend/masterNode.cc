#include <stdlib.h>
#include <stdio.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <rpc/server.h>
#include <rpc/client.h>
#include <errno.h>
#include <signal.h>
#include <sys/file.h>
#include <iostream>
#include <tuple>
#include <string>
#include <valarray>
#include <map>
#include <deque>
#include <pthread.h>

enum Command {GET, PUT, CPUT, DELETE};
struct thread_info {
    pthread_t thread_id;
    char *serverToNotify;
    char *newLeader;
    int cluster;
};

bool debugFlag;
bool testMode;
int serverIndx = 1;
int numClusters = 1;
std::valarray<int> rowVals (36);
std::map<int, std::deque<std::string>> clusterToServersMap;
std::map<int, std::deque<std::string>> clusterToActiveNodesMap;
// Maps a node's addr:port that it uses for frontend comm to its adrr:port for admin comm
std::map<std::string, std::string> frontendAddrPortToAdminAddrPort;
std::map<std::string, int> serverToClusterMap;
std::map<int, std::string> clusterToLeaderMap;
std::string masterNodeAddr;
// Cluster Number, isClusterLeader, Addr:Port for comm w/ Frontend, Addr:Port for comm w/ Admin
using server_addr_tuple = std::tuple<int, bool, std::string, std::string>;

void stderr_msg(std::string str) {
    std::cerr << str << "\n";
}

void log(std::string str) {
	if (debugFlag)
        stderr_msg(str);
}

// Returns an active server from the cluster that contains row-col-val based on first letter of row
// Returns 0 w/ active server in cluster on success
// Returns 1 w/ "Error" on failure
std::tuple<int, std::string> where(std::string row, std::string session_id) {
    log("Received WHERE: " + row+" for Session "+session_id);
    if(row.length() <= 0 || !isalnum(row.at(0))) {
        // error
         return std::make_tuple(1, "Error");
    }

    if(testMode) {
        return std::make_tuple(0, std::string("127.0.0.1:10000"));
    }

    char firstChar = toupper(row.at(0));
    std::deque<std::string> clusterToChooseFrom;
    if(firstChar >= '0' && firstChar <= '9') {
         clusterToChooseFrom = clusterToActiveNodesMap[0 % numClusters];
    } else if(firstChar >= 'A' && firstChar <= 'I') {
         clusterToChooseFrom = clusterToActiveNodesMap[1 % numClusters];
    } else if(firstChar >= 'J' && firstChar <= 'R') {
         clusterToChooseFrom = clusterToActiveNodesMap[2 % numClusters];
    } else if(firstChar >= 'S' && firstChar <= 'Z') {
         clusterToChooseFrom = clusterToActiveNodesMap[3 % numClusters];
    }
    return std::make_tuple(0, clusterToChooseFrom[rand() % clusterToChooseFrom.size()]);
}

void *notifyOfNewLeaderThreadFunc(void *arg) {
    struct thread_info *tinfo = (thread_info *)arg;
    std::string serverToNotify(tinfo->serverToNotify);
    std::string newLeader(tinfo->newLeader);
    int cluster = tinfo->cluster;
    // notify server of new primary
    int serverToNotifyPortNo = stoi(serverToNotify.substr(serverToNotify.find(":")+1));
	std::string serverToNotifyAddr = serverToNotify.substr(0, serverToNotify.find(":"));
	rpc::client serverToNotifyRPCClient(serverToNotifyAddr, serverToNotifyPortNo);
    int resp = serverToNotifyRPCClient.call("notifyOfNewLeader", newLeader).as<
				int>();
    log("Cluster "+std::to_string(cluster)+": Notifying server "+serverToNotify+" of new leader ("+newLeader+").");
    // freeing and exiting
    free(tinfo->serverToNotify);
    free(tinfo->newLeader);
    pthread_exit(0);
}

/*
Backend kvServers will ask for a new cluster leader if a call to the current leader fails/times out.
Master will remove old leader from list of active nodes, assign a new leader,
and notify all nodes in the cluster of the new leader
*/
std::tuple<int, std::string> getNewClusterLeader(std::string oldLeader) {
    int currCluster = serverToClusterMap[oldLeader];
    log("Node "+oldLeader+" from cluster "+std::to_string(currCluster)+" detected as being down. Assigning new leader.");
    std::deque<std::string> serverList = clusterToActiveNodesMap[currCluster];
    // Removing old leader from list of active nodes for that cluster
    for (auto it = serverList.begin(); it != serverList.end(); it++) {
        std::string currServer = *it;
        if(currServer.compare(oldLeader) == 0) {
            serverList.erase(it);
            break;
        }
    }
    // assigning new leader from list of active nodes for that cluster
    std::string newLeader = serverList.front();
    clusterToLeaderMap[currCluster] = newLeader;
    log("Setting node "+newLeader+" as leader of cluster "+std::to_string(currCluster));
    // notifying all nodes of new leader
    struct thread_info *tinfo = (thread_info *)calloc(serverList.size(), sizeof(struct thread_info));
    int i = 0;
    for(std::string currServer : serverList) {
        tinfo[i].serverToNotify = strdup(currServer.c_str());
        tinfo[i].newLeader = strdup(newLeader.c_str());
        tinfo[i].cluster = currCluster;
        int s = pthread_create(&tinfo[i].thread_id, NULL, &notifyOfNewLeaderThreadFunc, &tinfo[i]);
        i++;
    }
    i = 0;
    // waiting on all threads
    while(i < serverList.size()) {
        void *res;
        int s = pthread_join(tinfo[i].thread_id, &res);
        free(res);
        if (s != 0) {
            errno = s;
            perror("Error with pthread_join");
        }
    }
    free(tinfo);
    return std::make_tuple(0, newLeader);


}

// Backend kvServer node should register themselves with the Master when they first come online
std::tuple<int, std::string> registerWithMaster(std::string serverAddr) {
    log("Registering new node "+serverAddr+" with masterNode");
    if(serverToClusterMap.count(serverAddr) <= 0) {
        return std::make_tuple(-1, "ERROR. Server not present in configFile");
    }
    int cluster = serverToClusterMap[serverAddr];
    clusterToActiveNodesMap[cluster].push_back(serverAddr);
    log("New node "+serverAddr+" registered with masterNode");
    if(clusterToActiveNodesMap[cluster].size() == 1) {
        // First node for that cluster has been registered. Set it as cluster leader.
        clusterToLeaderMap[cluster] = serverAddr;
        log("New node "+serverAddr+" set as leader of cluster "+std::to_string(cluster));
    }
    return std::make_tuple(0, clusterToLeaderMap[cluster]);
}


std::deque<server_addr_tuple> getNodesFromMap(std::map<int, std::deque<std::string>> clusterToServers) {
    std::deque<server_addr_tuple> result;
    for (auto const& entry : clusterToServers) {
        int clusterNum = entry.first;
        for (std::string server : entry.second) {
            bool isClusterLeader = ((clusterToLeaderMap[clusterNum]).compare(server) == 0);
            std::string addrPortForAdmin = frontendAddrPortToAdminAddrPort[server];
            server_addr_tuple serverInfo = std::make_tuple(clusterNum, isClusterLeader, server, addrPortForAdmin);
            result.push_back(serverInfo);
        }
    }
    return result;
}

// Returns a deque of server_addr_tuples for all active backend nodes
std::deque<server_addr_tuple> getActiveNodes() {
    return getNodesFromMap(clusterToActiveNodesMap);
}

// Returns a deque of server_addr_tuples for all backend nodes
std::deque<server_addr_tuple> getAllNodes() {
    return getNodesFromMap(clusterToServersMap);
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
        std::string line = std::string(buffer);
        if(line.at(line.length()-1) == '\n') {
            line = line.substr(0, line.length()-1);
        }
        log("Reading from config file: "+line);
        // first line is address for master node
        if(serverNum != -1) {
            std::string addrPortForAdmin = line.substr(line.find(",") + 1);
            std::string server = line.substr(0, line.find(","));
            int currCluster = serverNum/3;
            if(clusterToServersMap.count(currCluster) <= 0) {
                clusterToServersMap[currCluster] = std::deque<std::string> {};
            }
            clusterToServersMap[currCluster].push_back(server);
            serverToClusterMap[server] = currCluster;
            if(clusterToActiveNodesMap.count(currCluster) <= 0) {
                clusterToActiveNodesMap[currCluster] = std::deque<std::string> {};
            }
            frontendAddrPortToAdminAddrPort[server] = addrPortForAdmin;
        } else {
            masterNodeAddr = line;
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
	srv.bind("getActiveNodes", &getActiveNodes);
	srv.bind("getAllNodes", &getAllNodes);
	srv.bind("registerWithMaster", &registerWithMaster);

	srv.run();

    return 0;
}
