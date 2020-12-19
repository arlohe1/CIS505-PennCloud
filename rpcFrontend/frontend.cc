#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/md5.h>
#include <queue>
#include <set>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <deque>
#include <rpc/client.h>
#include <rpc/rpc_error.h>
#include <regex>
#include <resolv.h>

std::string error_msg = "-ERR Server shutting down\r\n";
std::string kvMaster_addr = "";
std::string mail_addr = "";
std::string storage_addr = "";
pthread_mutex_t fd_mutex;
std::set<int*> fd;
volatile bool verbose = false;
int vflag = 0, internal_socket_fd;
volatile bool shut_down = false;
volatile bool load_balancer = false;
volatile int l_balancer_index = 0, server_index = 1;
volatile int session_id_counter = rand();
sockaddr_in load_balancer_addr;

/********************** Internal message stuff ******************/
std::string INTERNAL_THREAD = "i", SMTP_THREAD = "s", HTTP_THREAD = "h";
pthread_mutex_t modify_server_state, crashing, access_state_map;
struct server_state {
	time_t last_modified = time(NULL);
	std::string http_address = "";
	int http_connections = 0, smtp_connections = 0, internal_connections = 0,
			num_threads = 1;
} server_state;
struct server_state this_server_state;
std::vector<std::string> frontend_server_list;
std::map<std::string, struct server_state> frontend_state_map;

enum message_type {
	INFO_REQ = 0, INFO_RESP = 1, STOP = 2, RESUME = 3, ACK = 4
};
struct internal_message {
	message_type type;
	struct server_state state;
} internal_message;

std::vector<sockaddr_in> frontend_internal_list;
// maps session IDs to a specific backend server as specified by whereKVS
using server_tuple = std::tuple<std::string, std::string>;
std::map<std::string, server_tuple> sessionToServerMap;
std::map<std::string, int> sessionToServerIdx;
std::map<std::string, int> rowToClusterNum;
std::map<int, std::deque<server_tuple>> clusterToServerListMap;

using resp_tuple = std::tuple<int, std::string>;
using server_addr_tuple = std::tuple<int, bool, std::string, std::string>;

/********************** HTTP data structures *********************/

struct http_session {
	int id;
	std::string username;
} http_session;

struct http_request {
	bool valid = true;
	std::string type, filepath, version, content;
	std::map<std::string, std::string> headers;
	std::map<std::string, std::string> cookies;
	std::map<std::string, std::string> formData;
} http_request;

struct http_response {
	int status_code;
	std::string status, content;
	std::map<std::string, std::string> headers;
	std::map<std::string, std::string> cookies;
} http_response;
std::map<int, struct http_session> id_to_session;

/******************************* End http data structures ******************************/

/******************************* Start Util functions     ******************************/

// From HW2
void computeDigest(char *data, int dataLengthBytes,
		unsigned char *digestBuffer) {
	/* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */
	MD5_CTX c;
	MD5_Init(&c);
	MD5_Update(&c, data, dataLengthBytes);
	MD5_Final(digestBuffer, &c);
}

std::string generateStringHash(std::string strToHash) {
	unsigned char *digestBuff = (unsigned char*) malloc(
			MD5_DIGEST_LENGTH * sizeof(unsigned char) + 1);
	char *strToHashCStr = strdup(strToHash.c_str());
	computeDigest(strToHashCStr, strToHash.length() + 1, digestBuff);
	free(strToHashCStr);
	digestBuff[MD5_DIGEST_LENGTH] = '\0';
	char *stringHash = (char*) malloc((32 + 1) * sizeof(char));
	for (int i = 0; i < 16; i++) {
		stringHash[2 * i] = "0123456789ABCDEF"[digestBuff[i] / 16];
		stringHash[2 * i + 1] = "0123456789ABCDEF"[digestBuff[i] % 16];
	}
	free(digestBuff);
	stringHash[32] = '\0';
	return std::string(stringHash);

}

void log(std::string str) {
	if (!verbose)
		return;
	if (str.size() <= 1000) {
		std::cerr << str << "\n";
	} else {
		std::cerr << "<LOG STRING TOO LONG! LOGGING LENGTH INSTEAD> "
				<< str.size() << "\n";
		std::cerr << "<AND THE STARTING 500 BYTES> " << str.substr(0, 500)
				<< "\n";
	}
}

bool do_write(int fd, char *buf, int len) {
	int sent = 0;
	while (sent < len) {
		int n = write(fd, &buf[sent], len - sent);
		if (n < 0)
			return false;
		sent += n;
	}
	return true;
}

int readNBytes(int *client_fd, int n, char *buffer) {
	if (n == 0)
		return 0;
	int message_read = 0;
	while (message_read < n) {
		int rlen = read(*client_fd, &buffer[message_read], n - message_read);
		if (rlen > 0)
			message_read += rlen;
		if (rlen <= 0)
			return message_read;
	}
	return message_read;
}

void writeNBytes(int *client_fd, int n, const char *buffer) {
	if (n == 0)
		return;
	int message_wrote = 0;
	while (message_wrote < n) {
		int rlen = write(*client_fd, &buffer[message_wrote], n - message_wrote);
		message_wrote += rlen;
	}
}

void sigint_handler(int sig) {
	shut_down = true;
	for (int *f : fd) {
		int flags = fcntl(*f, F_GETFL, 0);
		fcntl(*f, F_SETFL, flags | O_NONBLOCK);
	}
}

std::deque<std::string> split(std::string str, std::string delimiter) {
	std::deque < std::string > ret;

	size_t pos = 0;

	while ((pos = str.find(delimiter)) != std::string::npos) {
		ret.push_back(str.substr(0, pos));
		str.erase(0, pos + delimiter.length());
	}
	ret.push_back(str);
	return ret;
}

std::string trim(std::string str) {
	str.erase(str.begin(),
			std::find_if(str.begin(), str.end(),
					std::not1(std::ptr_fun<int, int>(std::isspace))));
	str.erase(
			std::find_if(str.rbegin(), str.rend(),
					std::not1(std::ptr_fun<int, int>(std::isspace))).base(),
			str.end());
	return str;
}

std::string lower(std::string str) {
	std::string lower = str;
	std::transform(lower.begin(), lower.end(), lower.begin(),
			[](unsigned char c) {
				return std::tolower(c);
			});
	return lower;
}

std::string getLineAndDelete(std::string &str) {
	std::string delimiter = "\n";
	size_t pos = str.find(delimiter);
	std::string ret =
			pos == std::string::npos ?
					std::string(str) : std::string(str.substr(0, pos));
	if (pos != std::string::npos) {
		str.erase(0, pos + delimiter.length());
	} else {
		str.clear();
	}
	return ret;
}

void removeQuotes(std::string &str) {
	str.erase(remove(str.begin(), str.end(), '\"'), str.end());
}

int getPortNoFromString(std::string fullServAddr) {
	int port = 0;
	try {
		port = stoi(split(trim(fullServAddr), ":")[1]);
	} catch (const std::invalid_argument &ia) {
		log("Port not found! returning 0");
	}
	return port;
}

std::string getAddrFromString(std::string fullServAddr) {
	return trim(split(fullServAddr, ":")[0]);
}

/*********************** Internal Messaging Util function ***********************************/

void incrementThreadCounter(std::string type) {
	pthread_mutex_lock(&modify_server_state);
	if (type.compare(INTERNAL_THREAD) == 0) {
		this_server_state.internal_connections += 1;
	} else if (type.compare(SMTP_THREAD) == 0) {
		this_server_state.smtp_connections += 1;
	} else if (type.compare(HTTP_THREAD) == 0) {
		this_server_state.http_connections += 1;
	}
	this_server_state.num_threads += 1;
	pthread_mutex_unlock(&modify_server_state);
}

void decrementThreadCounter(std::string type) {
	pthread_mutex_lock(&modify_server_state);
	if (type.compare(INTERNAL_THREAD) == 0) {
		this_server_state.internal_connections -= 1;
	} else if (type.compare(SMTP_THREAD) == 0) {
		this_server_state.smtp_connections -= 1;
	} else if (type.compare(HTTP_THREAD) == 0) {
		this_server_state.http_connections -= 1;
	}
	this_server_state.num_threads -= 1;
	pthread_mutex_unlock(&modify_server_state);
}

std::string getAddressFromSockaddr(sockaddr_in &addr) {
	return std::string(inet_ntoa(addr.sin_addr)) + ":"
			+ std::to_string(ntohs(addr.sin_port));
}

std::string getMessageFromInternalSocket(struct sockaddr_in &src) {
	char buf[1001];
	socklen_t srcSize = sizeof(src);
	int rlen = recvfrom(internal_socket_fd, buf, sizeof(buf), 0,
			(struct sockaddr*) &src, &srcSize);
	buf[rlen] = 0;
	std::string raw_message(buf);
	if (!load_balancer)
		log("Received " + raw_message + " from " + getAddressFromSockaddr(src));

	return raw_message;
}

int send_message(std::string ret, sockaddr_in &dest, bool silent) {
	int rlen = sendto(internal_socket_fd, ret.data(), ret.length(), 0,
			(struct sockaddr*) &dest, sizeof(dest));
	if (!silent)
		log(
				"Sent '" + ret + "' to " + getAddressFromSockaddr(dest)
						+ " total bytes sent " + std::to_string(rlen));
	return rlen;
}

std::string internalMessageToString(struct internal_message &message) {
	std::string ret = std::to_string(message.type) + ",";
	if (message.type == INFO_RESP) {
		ret += message.state.http_address + ",";
		ret += std::to_string(message.state.http_connections) + ",";
		ret += std::to_string(message.state.smtp_connections) + ",";
		ret += std::to_string(message.state.internal_connections) + ",";
		ret += std::to_string(message.state.num_threads) + ",";
	}
	ret.pop_back();
	return ret;
}

void log_server_state(struct server_state &state) {
	std::string ret;
	ret += "Http address: " + state.http_address + ",";
	ret += "Last modified: " + std::to_string(state.last_modified) + ",";
	ret += "Http conn num: " + std::to_string(state.http_connections) + ",";
	ret += "Smtp conn num: " + std::to_string(state.smtp_connections) + ",";
	ret += "Internal conn num: " + std::to_string(state.internal_connections)
			+ ",";
	ret += "Num threads: " + std::to_string(state.num_threads);
	log(ret);
}

struct internal_message parseRawMessage(std::string &message) {
	struct internal_message ret;
	ret.type = message_type(stoi(message.substr(0, message.find(","))));
	message.erase(0, message.find(",") + 1);
	if (ret.type == INFO_RESP) {
		ret.state.http_address = message.substr(0, message.find(","));
		message.erase(0, message.find(",") + 1);
		ret.state.http_connections = message_type(
				stoi(message.substr(0, message.find(","))));
		message.erase(0, message.find(",") + 1);
		ret.state.smtp_connections = message_type(
				stoi(message.substr(0, message.find(","))));
		message.erase(0, message.find(",") + 1);
		ret.state.internal_connections = message_type(
				stoi(message.substr(0, message.find(","))));
		message.erase(0, message.find(",") + 1);
		ret.state.num_threads = message_type(
				stoi(message.substr(0, message.find(","))));
		message.erase(0, message.find(",") + 1);
	}
	return ret;
}

std::string prepareInternalMessage(message_type type) {
	struct internal_message ret;
	ret.type = type;
	if (type == INFO_RESP) {
		ret.state = this_server_state;
	}
	return internalMessageToString(ret);
}

bool amICrashing() {
	pthread_mutex_lock(&crashing);
	pthread_mutex_unlock(&crashing);
	return false;
}

int sendStateTo(sockaddr_in &dest) {
	std::string message = prepareInternalMessage(INFO_RESP);
	int rlen = sendto(internal_socket_fd, message.data(), message.length(), 0,
			(struct sockaddr*) &dest, sizeof(dest));
	return rlen;
}

void storeServerState(struct server_state &state) {
	pthread_mutex_lock(&access_state_map);
	state.last_modified = time(NULL);
	std::string http_addr = trim(state.http_address);
	frontend_state_map[http_addr] = state;
	pthread_mutex_unlock(&access_state_map);
}

void requstStateFromAllServers() {
	std::string message = prepareInternalMessage(INFO_REQ);
	for (sockaddr_in dest : frontend_internal_list) {
		send_message(message, dest, false);
	}
}

int sendStopMessage(int index) {
	if (index == 0 || index == server_index
			|| index >= frontend_internal_list.size())
		return -1;
	std::string message = prepareInternalMessage(STOP);
	return send_message(message, frontend_internal_list[index], false)
			- message.size();
}

int sendResumeMessage(int index) {
	if (index == 0 || index == server_index
			|| index >= frontend_internal_list.size())
		return -1;
	std::string message = prepareInternalMessage(RESUME);
	return send_message(message, frontend_internal_list[index], false)
			- message.size();
}

int getServerIndexFromAddr(std::string &addr) {
	auto it = std::find(frontend_server_list.begin(),
			frontend_server_list.end(), addr);
	return (it == frontend_server_list.end()) ?
			-1 : it - frontend_server_list.begin() + 1;
}

void resumeThisServer() {
	log("Resuming");
	pthread_mutex_unlock(&crashing);
}

void crashThisServer() {
	log("Crashing");
	pthread_mutex_lock(&crashing);
	// wait for resumption
	while (!shut_down) {
		log("need to wait for RESUME");
		sockaddr_in src;
		std::string raw_message = getMessageFromInternalSocket(src);
		struct internal_message message;
		try {
			message = parseRawMessage(raw_message);
			if (message.type == RESUME) {
				break;
			} else {
				log("Still waiting for RESUME message");
				continue;
			}
		} catch (const std::invalid_argument &ia) {
			log("Invalid argument: " + std::string(ia.what()));
		}
	}
	if (shut_down)
		exit(-1);
	resumeThisServer();
}

void* heartbeat(void *arg) {
	incrementThreadCounter(INTERNAL_THREAD);
	while (!shut_down && !amICrashing()) {
		sendStateTo(load_balancer_addr);
		sleep(2);
	}
	decrementThreadCounter(INTERNAL_THREAD);
	pthread_detach (pthread_self());pthread_exit
	(NULL);
}

void* handleInternalConnection(void *arg) {
	incrementThreadCounter(INTERNAL_THREAD);
	sockaddr_in src;
	std::string raw_message = getMessageFromInternalSocket(src);

	struct internal_message message;
	try {
		message = parseRawMessage(raw_message);
		switch (message.type) {
		case INFO_REQ:
			if (!load_balancer)
				sendStateTo(src);
			break;
		case INFO_RESP:
			storeServerState(message.state);
			break;
		case STOP:
			crashThisServer();
			break;
		case RESUME:
			resumeThisServer();
			break;
		}
	} catch (const std::invalid_argument &ia) {
		log("Invalid argument: " + std::string(ia.what()));
	}

	decrementThreadCounter(INTERNAL_THREAD);
	pthread_detach (pthread_self());pthread_exit
	(NULL);
}

/*********************** KVS Util function ***********************************/

int kvsResponseStatusCode(resp_tuple resp) {
	return std::get < 0 > (resp);
}

std::string kvsResponseMsg(resp_tuple resp) {
	return std::get < 1 > (resp);
}

void buildClusterToBackendServerMapping() {
	int masterPortNo = getPortNoFromString(kvMaster_addr);
	std::string masterServAddress = getAddrFromString(kvMaster_addr);
	rpc::client masterNodeRPCClient(masterServAddress, masterPortNo);
	try {
		log("Requesting all backend nodes");
		using server_addr_tuple = std::tuple<int, bool, std::string, std::string>;
		std::deque<server_addr_tuple> resp = masterNodeRPCClient.call(
				"getAllNodes").as<std::deque<server_addr_tuple>>();
		for (server_addr_tuple serverInfo : resp) {
			int clusterNum = std::get < 0 > (serverInfo);
			std::string serverAddr = std::get < 2 > (serverInfo);
			std::string serverAdminAddr = std::get < 3 > (serverInfo);
			log(
					"Received Server (" + serverAddr + ", " + serverAdminAddr
							+ " for Cluster " + std::to_string(clusterNum));
			clusterToServerListMap[clusterNum].push_back(
					std::make_tuple(serverAddr, serverAdminAddr));
		}
	} catch (rpc::rpc_error &e) {
		log("UNHANDLED ERROR IN buildClusterToBAckendServerMapping TRY CATCH");
	}
}

bool checkIfNodeIsAlive(server_tuple serverInfo) {
    // connect to heartbeat thread of backend server and check if it's alive w/ shorter timeout
    std::string targetServer = std::get<0>(serverInfo);
    std::string targetServerHeartbeatIP = std::get<1>(serverInfo);
    log("Checking heartbeat for "+ targetServer+" at heartbeat address: "+targetServerHeartbeatIP);
    int heartbeatPortNo = getPortNoFromString(targetServerHeartbeatIP);
    std::string heartbeatAddress = getAddrFromString(targetServerHeartbeatIP);
    rpc::client kvsHeartbeatRPCClient(heartbeatAddress, heartbeatPortNo);
    kvsHeartbeatRPCClient.set_timeout(2000); // 2000 milliseconds
    try {
        bool isAlive = kvsHeartbeatRPCClient.call("heartbeat").as<bool>();
        log("Heartbeat for "+targetServer+" returned true!");
        return isAlive;
    } catch(rpc::timeout &t) {
        log("Heartbeat for "+targetServer+" failed to return. Node is dead.");
        return false;
    }
    return false;
}

std::string whereKVS(std::string session_id, std::string row) {
	int masterPortNo = getPortNoFromString(kvMaster_addr);
	std::string masterServAddress = getAddrFromString(kvMaster_addr);
	rpc::client masterNodeRPCClient(masterServAddress, masterPortNo);
	try {
		if (rowToClusterNum.count(row) <= 0) {
			log(
					"MASTERNODE WHERE: (" + row + ") for session (" + session_id
							+ ")");
			// Returns cluster # for the row
			int resp =
					masterNodeRPCClient.call("where", row, session_id).as<int>();
			if (resp == -1) {
				// ERROR
				return "ERROR";
			}
			rowToClusterNum[row] = resp;
		}
		int clusterNum = rowToClusterNum[row];

		std::deque<server_tuple> serverList = clusterToServerListMap[clusterNum];
        log("whereKVS: Servers to choose from in cluster "+std::to_string(clusterNum)+" are:") ;
        for(server_tuple serverInfo : serverList) {
            log("--"+std::get<0>(serverInfo));
        }
		int serverIdx = 0;
		if (sessionToServerIdx.count(session_id) <= 0) {
			// Randomly generate index to pick server in cluster
			serverIdx = rand() % serverList.size();
			log(
					"whereKVS: New session " + session_id + " for row " + row
							+ "! Randomly picking server "
							+ std::to_string(serverIdx));
		} else {
			// Incrementing serverIdx to next server in list
			serverIdx = sessionToServerIdx[session_id];
			serverIdx++;
			log(
					"whereKVS: Existing session " + session_id + "for row "
							+ row + "! Incrementing server to "
							+ std::to_string(serverIdx));
		}
		serverIdx = serverIdx % serverList.size();
		sessionToServerIdx[session_id] = serverIdx;

		server_tuple chosenServerAddrs = serverList[serverIdx];
		sessionToServerMap[session_id] = chosenServerAddrs;
		log(
				"whereKVS: session_id " + session_id + " given server "
						+ std::get < 0
						> (chosenServerAddrs) + " for cluster "
								+ std::to_string(clusterNum));
		return std::get < 0 > (chosenServerAddrs);
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN whereKVS TRY CATCH"); // TODO
	}
	return "Error in whereKVS";
}

resp_tuple kvsFunc(std::string kvsFuncType, std::string session_id, std::string row, std::string column, std::string value, std::string old_value) {
    if(sessionToServerMap.count(session_id) <= 0) {
        log(kvsFuncType +": No server for session "+ session_id+". Calling whereKVS.");
        std::string newlyChosenServerAddr = whereKVS(session_id, row);
        log(kvsFuncType +": Server "+ newlyChosenServerAddr+" chosen for session "+ session_id+".");
    }
    uint64_t timeout = 5000; // 5000 milliseconds
    bool nodeIsAlive = true;
    int origServerIdx = sessionToServerIdx[session_id];
    int currServerIdx = -2; 
    // Continue trying RPC call until you've tried all backend servers
    while(origServerIdx != currServerIdx) {
        server_tuple serverInfo = sessionToServerMap[session_id];
        std::string targetServer = std::get<0>(serverInfo);
        if(!checkIfNodeIsAlive(serverInfo)) {
            // Resetting timeout for new server
            timeout = 2500; // 2500 milliseconds
            std::string newlyChosenServerAddr = whereKVS(session_id, row);
            currServerIdx = sessionToServerIdx[session_id];
            log("Node "+targetServer+" is dead! Trying new node "+ newlyChosenServerAddr);
            continue;
        }
        int serverPortNo = getPortNoFromString(targetServer);
        std::string servAddress = getAddrFromString(targetServer);
        rpc::client kvsRPCClient(servAddress, serverPortNo);
        resp_tuple resp;
        kvsRPCClient.set_timeout(timeout);
        try {
            if(kvsFuncType.compare("putKVS") == 0) {
                log("KVS PUT with kvServer "+targetServer+": " + row + ", " + column + ", " + value);
                resp = kvsRPCClient.call("put", row, column, value).as<resp_tuple>();
            } else if(kvsFuncType.compare("cputKVS") == 0) {
                log("KVS CPUT with kvServer "+targetServer+": " + row + ", " + column + ", " + old_value + ", " + value);
                resp = kvsRPCClient.call("cput", row, column, old_value, value).as<resp_tuple>();
            } else if(kvsFuncType.compare("deleteKVS") == 0) {
                log("KVS DELETE with kvServer "+targetServer+": " + row + ", " + column);
                resp = kvsRPCClient.call("del", row, column).as<resp_tuple>();
            } else if(kvsFuncType.compare("getKVS") == 0) {
                log("KVS GET with kvServer "+targetServer+": " + row + ", " + column);
                resp = kvsRPCClient.call("get", row, column).as<resp_tuple>();
            }
            log(kvsFuncType +" Response Status: " + std::to_string(kvsResponseStatusCode(resp)));
            log(kvsFuncType +" Response Value: " + kvsResponseMsg(resp));
            return resp;
        } catch (rpc::timeout &t) {
            log(kvsFuncType+" for ("+session_id+", "+row+") timed out!");
            // connect to heartbeat thread of backend server and check if it's alive w/ shorter timeout
            bool isAlive = checkIfNodeIsAlive(serverInfo);
            if(isAlive) {
                // Double timeout and try again if node is still alive
                timeout *= 2;
                log("Node "+targetServer+" is still alive! Doubling timeout to "+std::to_string(timeout)+" and trying again.");
            }  else {
                // Resetting timeout for new server
                timeout = 5000; // 5000 milliseconds
                std::string newlyChosenServerAddr = whereKVS(session_id, row);
                currServerIdx = sessionToServerIdx[session_id];
                log("Node "+targetServer+" is dead! Trying new node "+ newlyChosenServerAddr);
            }
        } catch (rpc::rpc_error &e) {
            /*
             std::cout << std::endl << e.what() << std::endl;
             std::cout << "in function " << e.get_function_name() << ": ";
             using err_t = std::tuple<std::string, std::string>;
             auto err = e.get_error().as<err_t>();
             */
            log("UNHANDLED ERROR IN "+kvsFuncType+" TRY CATCH"); // TODO
        }
    }
    log(kvsFuncType+" for ("+session_id+", "+row+"): All nodes in cluster down! ERROR.");
    return std::make_tuple(-2, "All nodes in cluster down!");
}

resp_tuple putKVS(std::string session_id, std::string row, std::string column, std::string value) {
	return kvsFunc("putKVS", session_id, row, column, value, "");
}

resp_tuple cputKVS(std::string session_id, std::string row, std::string column,
		std::string old_value, std::string value) {
	return kvsFunc("cputKVS", session_id, row, column, value, old_value);
}

resp_tuple deleteKVS(std::string session_id, std::string row,
		std::string column) {
	return kvsFunc("deleteKVS", session_id, row, column, "", "");
}

std::deque<server_addr_tuple> getActiveNodesKVS() {
	int masterPortNo = getPortNoFromString(kvMaster_addr);
	std::string masterServAddress = getAddrFromString(kvMaster_addr);
	rpc::client masterNodeRPCClient(masterServAddress, masterPortNo);
	std::deque<server_addr_tuple> resp;
	try {
		log("MASTERNODE getActiveNodes");
		resp = masterNodeRPCClient.call("getActiveNodes").as<
				std::deque<server_addr_tuple>>();
		return resp;
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getActiveNodesKVS TRY CATCH"); // TODO
	}
	log("Error in getActiveNodesKVS");
	return resp;
}

std::deque<server_addr_tuple> getAllNodesKVS() {
	int masterPortNo = getPortNoFromString(kvMaster_addr);
	std::string masterServAddress = getAddrFromString(kvMaster_addr);
	rpc::client masterNodeRPCClient(masterServAddress, masterPortNo);
	std::deque<server_addr_tuple> resp;
	try {
		log("MASTERNODE getAllNodesKVS");
		resp = masterNodeRPCClient.call("getAllNodes").as<
				std::deque<server_addr_tuple>>();
		return resp;
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getAllNodesKVS TRY CATCH"); // TODO
	}
	log("Error in getAllNodesKVS");
	return resp;
}

void stopServerKVS(std::string target) {
	int targetPortNo = getPortNoFromString(target);
	std::string targetServAddress = getAddrFromString(target);
	rpc::client targetNodeRPCClient(targetServAddress, targetPortNo);
	try {
		targetNodeRPCClient.call("killServer");
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getAllNodesKVS TRY CATCH"); // TODO
	}
}

void reviveServerKVS(std::string target) {
	int targetPortNo = getPortNoFromString(target);
	std::string targetServAddress = getAddrFromString(target);
	rpc::client targetNodeRPCClient(targetServAddress, targetPortNo);
	try {
		targetNodeRPCClient.call("reviveServer");
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getAllNodesKVS TRY CATCH"); // TODO
	}
}

resp_tuple getKVS(std::string session_id, std::string row, std::string column) {
	return kvsFunc("getKVS", session_id, row, column, "", "");
}

/***************************** Start storage service functions ************************/

int uploadFile(struct http_request req, std::string filepath) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];
	std::string filename = req.formData["filename"];
	std::string fileData = req.formData["file"];

	// Construct filepath of new file
    time_t rawtime;
	struct tm *timeinfo;
	time(&rawtime);
	timeinfo = gmtime(&rawtime);
	std::string temp = asctime(timeinfo);
	std::string filenameHash = generateStringHash(
			username + filepath + filename + temp);
	std::string kvsCol = "ss1_" + filenameHash;
	std::string newEntry = filename + "," + kvsCol + "\n";

	// Reading in response to GET --> list of files at filepath
	int count = 0;
	while (count < 10) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, filepath);
		resp_tuple cputCmdResponse;
		std::string fileList = kvsResponseMsg(getCmdResponse);
		std::stringstream ss(fileList);
		std::string fileEntry;
		std::string contents = "";
		std::string fileNameLower = filename;
		std::transform(fileNameLower.begin(), fileNameLower.end(),
				fileNameLower.begin(), ::tolower);
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::getline(ss, fileEntry, '\n');
			contents += fileEntry + "\n";
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currName = fileEntry.substr(0, foundPos);
					std::transform(currName.begin(), currName.end(),
							currName.begin(), ::tolower);
					if (currName == fileNameLower) {
						return -1;
					} else if (currName.compare(fileNameLower) < 0) {
						contents += fileEntry + "\n";
					} else {
						contents += newEntry;
						contents += fileEntry + "\n";
						found = true;
					}
				} else {
					contents += fileEntry + "\n";
				}
			}
			if (!found) {
				contents += newEntry;
			}

			// CPUT length,row,col,value for MODIFIED FILE LIST
			cputCmdResponse = cputKVS(sessionid, username, filepath, fileList,
					contents);
			int respStatus = kvsResponseStatusCode(cputCmdResponse);
			if (respStatus == 0) {
				putKVS(sessionid, username, kvsCol, fileData);
				return 0;
			}
			count++;
		} else {
			return -2;
		}
	}
	return -2;
}

int renameFile(struct http_request req, std::string filepath,
		std::string itemToRename, std::string newName) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	int count = 0;
	while (count < 10) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, filepath);
		resp_tuple cputCmdResponse;
		std::string fileList = kvsResponseMsg(getCmdResponse);
		std::stringstream ss(fileList);
		std::string fileEntry;
		std::string contents = "";
		std::string contentsFinal = "";
		std::string fileNameLower = newName;
		std::transform(fileNameLower.begin(), fileNameLower.end(),
				fileNameLower.begin(), ::tolower);
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::getline(ss, fileEntry, '\n');
			contents += fileEntry + "\n";
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currHash = fileEntry.substr(foundPos + 1);
					if (currHash != itemToRename) {
						contents += fileEntry + "\n";
					} else {
						found = true;
					}
				} else {
					contents += fileEntry + "\n";
				}
			}
			if (!found) {
				return -3;
			}

			std::string newEntry = newName + "," + itemToRename;
			std::stringstream ss2(contents);

			std::getline(ss2, fileEntry, '\n');
			contentsFinal += fileEntry + "\n";
			found = false;
			while (std::getline(ss2, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currName = fileEntry.substr(0, foundPos);
					std::transform(currName.begin(), currName.end(),
							currName.begin(), ::tolower);
					if (currName == fileNameLower) {
						return -1;
					} else if (currName.compare(fileNameLower) < 0) {
						contentsFinal += fileEntry + "\n";
					} else {
						contentsFinal += newEntry + "\n";
						contentsFinal += fileEntry + "\n";
						found = true;
					}
				} else {
					contentsFinal += fileEntry + "\n";
				}
			}
			if (!found) {
				contentsFinal += newEntry + "\n";
			}

			// CPUT length,row,col,value for MODIFIED FILE LIST
			cputCmdResponse = cputKVS(sessionid, username, filepath, fileList,
					contentsFinal);
			int respStatus = kvsResponseStatusCode(cputCmdResponse);
			if (respStatus == 0) {
				return 0;
			}
			count++;
		} else {
			return -2;
		}
	}
	return -2;
}

int isDirectoryGetHash(struct http_request req, std::string filepath,
		std::string &hash) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	std::string userRootDir = "ss0_" + generateStringHash(username + "/");

	if (filepath == "~" || filepath == "~/") {
		hash = userRootDir;
		return 0;
	}
	if (filepath.substr(0, 2) != "~/") {
		return -1;
	}

	size_t last = 0;
	size_t next = 0;
	next = filepath.find("/", last);
	last = next + 1;

	std::string relevantHash = userRootDir;

	while ((next = filepath.find("/", last)) != std::string::npos) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, relevantHash);
		std::string fileList = kvsResponseMsg(getCmdResponse);
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::stringstream ss(fileList);
			std::string fileEntry;
			std::string curr = filepath.substr(last, next - last);
			last = next + 1;
			std::getline(ss, fileEntry, '\n');
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currName = fileEntry.substr(0, foundPos);
					if (currName == curr) {
						found = true;
						relevantHash = fileEntry.substr(foundPos + 1);
					}
				}
			}
			if (!found) {
				return -1;
			}
		} else {
			return -2;
		}
	}

	std::string curr = filepath.substr(last);
	if (curr != "") {
		resp_tuple getCmdResponse = getKVS(sessionid, username, relevantHash);
		std::string fileList = kvsResponseMsg(getCmdResponse);
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::stringstream ss(fileList);
			std::string fileEntry;
			std::getline(ss, fileEntry, '\n');
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currName = fileEntry.substr(0, foundPos);
					if (currName == curr) {
						found = true;
						relevantHash = fileEntry.substr(foundPos + 1);
					}
				}
			}
			if (!found) {
				return -1;
			}
		} else {
			return -2;
		}
	}

	hash = relevantHash;
	return 0;
}

int moveFile(struct http_request req, std::string filepath,
		std::string itemToMove, std::string newLocation, std::string &target) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	std::string hash;
	int result = isDirectoryGetHash(req, newLocation, hash);

	if (result < 0)
		return result;

	int count = 0;
	while (count < 10) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, filepath);
		resp_tuple cputCmdResponse;
		std::string fileList = kvsResponseMsg(getCmdResponse);
		std::stringstream ss(fileList);
		std::string fileEntry;
		std::string contents = "";
		std::string contentsFinal = "";
		std::string fileNameLower;

		std::string oldEntry = "";
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::getline(ss, fileEntry, '\n');
			contents += fileEntry + "\n";
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currHash = fileEntry.substr(foundPos + 1);
					if (currHash != itemToMove) {
						contents += fileEntry + "\n";
					} else {
						fileNameLower = fileEntry.substr(0, foundPos);
						std::transform(fileNameLower.begin(),
								fileNameLower.end(), fileNameLower.begin(),
								::tolower);
						oldEntry = fileEntry;
						found = true;
					}
				} else {
					contents += fileEntry + "\n";
				}
			}
			if (!found) {
				return -3;
			}

			resp_tuple getCmdResponse2 = getKVS(sessionid, username, hash);
			resp_tuple cputCmdResponse2;
			std::string fileList2 = kvsResponseMsg(getCmdResponse2);
			std::stringstream ss2(fileList2);

			std::getline(ss2, fileEntry, '\n');
			contentsFinal += fileEntry + "\n";
			found = false;
			while (std::getline(ss2, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currName = fileEntry.substr(0, foundPos);
					std::transform(currName.begin(), currName.end(),
							currName.begin(), ::tolower);
					if (currName == fileNameLower) {
						return -1;
					} else if (currName.compare(fileNameLower) < 0) {
						contentsFinal += fileEntry + "\n";
					} else {
						contentsFinal += oldEntry + "\n";
						contentsFinal += fileEntry + "\n";
						found = true;
					}
				} else {
					contentsFinal += fileEntry + "\n";
				}
			}
			if (!found) {
				contentsFinal += oldEntry + "\n";
			}

			// CPUT length,row,col,value for MODIFIED FILE LIST
			cputCmdResponse = cputKVS(sessionid, username, hash, fileList2,
					contentsFinal);
			int respStatus = kvsResponseStatusCode(cputCmdResponse);
			if (respStatus == 0) {
				putKVS(sessionid, username, filepath, contents);
				target = hash;
				return 0;
			}
			count++;
		} else {
			return -2;
		}
	}
	return -2;
}

int deleteFile(struct http_request req, std::string containingDir,
		std::string itemToDeleteHash) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	int count = 0;
	while (count < 10) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, containingDir);
		resp_tuple cputCmdResponse;
		std::string fileList = kvsResponseMsg(getCmdResponse);
		std::stringstream ss(fileList);
		std::string fileEntry;
		std::string contents = "";
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::getline(ss, fileEntry, '\n');
			contents += fileEntry + "\n";
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currHash = fileEntry.substr(foundPos + 1);
					if (currHash != itemToDeleteHash) {
						contents += fileEntry + "\n";
					} else {
						found = true;
					}
				} else {
					contents += fileEntry + "\n";
				}
			}
			if (!found) {
				return -1;
			}

			// CPUT length,row,col,value for MODIFIED FILE LIST
			cputCmdResponse = cputKVS(sessionid, username, containingDir,
					fileList, contents);

			int respStatus = kvsResponseStatusCode(cputCmdResponse);
			if (respStatus == 0) {
				deleteKVS(sessionid, username, itemToDeleteHash);
				return 0;
			}
			count++;
		} else {
			return -2;
		}
	}
	return -2;
}

void forceDeleteDirectory(struct http_request req,
		std::string itemToDeleteHash) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	resp_tuple recursiveDeleteResp = getKVS(sessionid, username,
			itemToDeleteHash);
	int respStatus = kvsResponseStatusCode(recursiveDeleteResp);
	std::string respValue = kvsResponseMsg(recursiveDeleteResp);
	if (respStatus == 0) {
		std::deque < std::string > splt = split(respValue, "\n");
		int lineNum = 0;
		for (std::string line : splt) {
			if (line.length() > 0) {
				std::deque < std::string > lineSplt = split(line, ",");
				if (lineNum != 0) {
					// Delete Child Files or Directories
					if (lineSplt[1].at(2) == '1') {
						deleteKVS(sessionid, username, lineSplt[1]);
					} else if (lineSplt[1].at(2) == '0') {
						forceDeleteDirectory(req, lineSplt[1]);
					}
				}
				lineNum++;
			}
		}
	}
	deleteKVS(sessionid, username, itemToDeleteHash);
}

int deleteDirectory(struct http_request req, std::string containingDir,
		std::string itemToDeleteHash) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	int count = 0;
	while (count < 10) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, containingDir);
		resp_tuple cputCmdResponse;
		std::string fileList = kvsResponseMsg(getCmdResponse);
		std::stringstream ss(fileList);
		std::string fileEntry;
		std::string contents = "";
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::getline(ss, fileEntry, '\n');
			contents += fileEntry + "\n";
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currHash = fileEntry.substr(foundPos + 1);
					if (currHash != itemToDeleteHash) {
						contents += fileEntry + "\n";
					} else {
						found = true;
					}
				} else {
					contents += fileEntry + "\n";
				}
			}
			if (!found) {
				return -1;
			}

			// CPUT length,row,col,value for MODIFIED FILE LIST
			cputCmdResponse = cputKVS(sessionid, username, containingDir,
					fileList, contents);
			int respStatus = kvsResponseStatusCode(cputCmdResponse);
			if (respStatus == 0) {
				forceDeleteDirectory(req, itemToDeleteHash);
				return 0;
			}
			count++;
		} else {
			return -2;
		}
	}
	return -2;
}

std::string getParentDirLink(std::string fileHash) {
	std::string link = "<li>Go back<a href=/files/" + fileHash + ">Link</a>";
	return link;
}

std::string getFileLink(std::string fileName, std::string fileHash,
		std::string containingDirectory) {
	std::string link;
	if (fileHash.substr(0, 3).compare("ss0") == 0) {
// Directory
		link = "<li>" + fileName + "<a href=/files/" + fileHash
				+ ">Open Directory</a>";
	} else {
// File
		link = "<li>" + fileName + "<a download=\"" + fileName
				+ "\" href=/files/" + fileHash + ">Download</a>";
	}
	link +=
			"<form action=\"/files/" + containingDirectory
					+ "\" method=\"post\">"
							"<input type=\"hidden\" name=\"itemToDelete\" value=\""
					+ fileHash
					+ "\" />"
							"<input type=\"submit\" name=\"submit\" value=\"Delete\" />"
							"</form>"
							"<script>function encodeName() {document.getElementsByName(\"newName\")[0].value = encodeURIComponent(document.getElementsByName(\"newName\")[0].value); return true;}</script>"
							"<form accept-charset=\"utf-8\" onsubmit=\"return encodeName();\" action=\"/files/"
					+ containingDirectory
					+ "\" method=\"post\">"
							"<div style=\"display: flex; flex-direction: row;\">"
							"<input type=\"hidden\" name=\"itemToRename\" value=\""
					+ fileHash
					+ "\" />"
							"<label for=\"newName\">New Name</label><input required type=\"text\" name=\"newName\"/>"
							"<input type=\"submit\" name=\"submit\" value=\"Rename\" />"
							"</div>"
							"</form>"
							"<script>function encode() {document.getElementsByName(\"newLocation\")[0].value = encodeURIComponent(document.getElementsByName(\"newLocation\")[0].value); return true;}</script>"
							"<form accept-charset=\"utf-8\" onsubmit=\"return encode();\" action=\"/files/"
					+ containingDirectory
					+ "\" method=\"post\">"
							"<div style=\"display: flex; flex-direction: row;\">"
							"<input type=\"hidden\" name=\"itemToMove\" value=\""
					+ fileHash
					+ "\" />"
							"<label for=\"newLocation\">New Location</label><input required type=\"text\" name=\"newLocation\"/>"
							"<input type=\"submit\" name=\"submit\" value=\"Move\" />"
							"</div>"
							"</form>";
	return link;
}

std::string getFileList(struct http_request req, std::string filepath) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];
	resp_tuple filesResp = getKVS(sessionid, username, filepath);
	int respStatus = kvsResponseStatusCode(filesResp);
	std::string respValue = kvsResponseMsg(filesResp);
	int lineNum = 0;
	if (respStatus == 0) {
		if (respValue.length() == 0) {
			return "This directory is empty.";
		}
		std::string result = "<ul>";
		std::deque < std::string > splt = split(respValue, "\n");
		for (std::string line : splt) {
			if (line.length() > 0) {
				std::deque < std::string > lineSplt = split(line, ",");
				if (lineNum == 0) {
					// Parent Directory Line
					if (!(lineSplt[0].compare("ROOT") == 0
							&& lineSplt[1].compare("ROOT") == 0)) {
						result += getParentDirLink(lineSplt[1]);
					}
				} else {
					// Child Files or Directories
					result += getFileLink(lineSplt[0], lineSplt[1], filepath);
				}
				lineNum++;
			}
		}
		result += "</ul>";
		if (lineNum <= 1) {
			result += "<p>This directory is empty</p>";
		}
		return result;
	} else {
		return "No files available";
	}
}

bool isFileRouteDirectory(std::string filepath) {
// All directories should end with a '/'
	return (filepath.length() > 4 && filepath.substr(0, 4).compare("ss0_") == 0);
}

int createDirectory(struct http_request req, std::string filepath,
		std::string dirName) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

// Construct filepath of new directory
	time_t rawtime;
	struct tm *timeinfo;
	time(&rawtime);
	timeinfo = gmtime(&rawtime);
	std::string temp = asctime(timeinfo);
	std::string dirNameHash = generateStringHash(
			username + filepath + dirName + temp);
	std::string kvsCol = "ss0_" + dirNameHash;
	std::string newEntry = dirName + "," + kvsCol + "\n";

	// Reading in response to GET --> list of files at filepath
	int count = 0;
	while (count < 10) {
		resp_tuple getCmdResponse = getKVS(sessionid, username, filepath);
		resp_tuple cputCmdResponse;
		std::string fileList = kvsResponseMsg(getCmdResponse);
		std::stringstream ss(fileList);
		std::string fileEntry;
		std::string contents = "";
		std::string dirNameLower = dirName;
		std::transform(dirNameLower.begin(), dirNameLower.end(),
				dirNameLower.begin(), ::tolower);
		if (kvsResponseStatusCode(getCmdResponse) == 0) {
			std::getline(ss, fileEntry, '\n');
			contents += fileEntry + "\n";
			bool found = false;
			while (std::getline(ss, fileEntry, '\n')) {
				if (!found) {
					std::size_t foundPos = fileEntry.find_last_of(",");
					std::string currName = fileEntry.substr(0, foundPos);
					std::transform(currName.begin(), currName.end(),
							currName.begin(), ::tolower);
					if (currName == dirNameLower) {
						return -1;
					} else if (currName.compare(dirNameLower) < 0) {
						contents += fileEntry + "\n";
					} else {
						contents += newEntry;
						contents += fileEntry + "\n";
						found = true;
					}
				} else {
					contents += fileEntry + "\n";
				}
			}
			if (!found) {
				contents += newEntry;
			}

			// CPUT length,row,col,value for MODIFIED FILE LIST
			cputCmdResponse = cputKVS(sessionid, username, filepath, fileList,
					contents);
			int respStatus = kvsResponseStatusCode(cputCmdResponse);
			if (respStatus == 0) {
				putKVS(sessionid, username, kvsCol,
						"PARENT_DIR," + filepath + "\n");
				return 0;
			}
			count++;
		} else {
			return -2;
		}
	}
	return -2;
}

void createRootDirForNewUser(struct http_request req, std::string sessionid) {
	std::string username = req.formData["username"];
	std::string dirNameHash = generateStringHash(username + "/");
// PUT new column for root directory
	putKVS(sessionid, username, "ss0_" + dirNameHash, "ROOT,ROOT\n");
}

/***************************** End storage service functions ************************/

/*********************** Http Util function **********************************/
std::string generateSessionID() {
	return generateStringHash(
			this_server_state.http_address
					+ std::to_string(++session_id_counter));
}

std::string getBoundary(std::string &type) {
	std::deque < std::string > splt = split(type, ";");
	for (std::string potent : splt) {
		if (potent.find("boundary") != std::string::npos) {
			std::string boundary = trim(potent.substr(potent.find("=") + 1));
			return "--" + boundary + "\r\n";
		}
	}
	return "";
}

void processMultiPart(struct http_request &req) {
	log("Processing multipart");
	std::string boundary = getBoundary(req.headers["content-type"]);
	std::string content(req.content);
	std::string segment = "";
	content.erase(0, content.find(boundary) + boundary.length());

	while (trim((segment = content.substr(0, content.find(boundary)))).compare(
			"") != 0) {
		content.erase(0, content.find(boundary) + boundary.length());
		std::string line = "", fieldname = "", filename = "";
		bool segment_is_file = false;
		while (trim((line = getLineAndDelete(segment))).compare("") != 0) {
			if (line.find("filename") != std::string::npos)
				segment_is_file = true;
			if (lower(line).find("content-disposition") != std::string::npos) {
				std::deque < std::string > data = split((split(line, ":")[1]),
						";");
				for (std::string d : data) {
					if (d.find("filename") != std::string::npos) {
						filename = trim(split(d, "=")[1]);
						removeQuotes(filename);
						req.formData["filename"] = filename;
					} else if (d.find("name") != std::string::npos) {
						fieldname = trim(split(d, "=")[1]);
						removeQuotes(fieldname);
					}
				}
			}
		}

		if (segment_is_file) {
			req.formData["filename"] = filename;
			req.formData["file"] = segment;
		} else if (fieldname.compare("") != 0) {
			req.formData[fieldname] = trim(segment);
		}
	}

	log("Results of multi-part processing: ");
	log("Form data: ");
	for (std::map<std::string, std::string>::iterator it = req.formData.begin();
			it != req.formData.end(); it++) {
		log(
				"Key : " + it->first + " value : "
						+ std::to_string((it->second).size()));
	}
}

void processForm(struct http_request &req) {
	std::deque < std::string > queryPairs = split(req.content, "&");
	for (std::string pair : queryPairs) {
		size_t pos = pair.find("=");
		std::string key =
				(pos == std::string::npos) ? pair : pair.substr(0, pos);
		std::string value =
				(pos == std::string::npos) ?
						"" :
						((pos + 1 < pair.length()) ? pair.substr(pos + 1) : "");

		req.formData[key] = value;
	}

	log("Form data: ");
	for (std::map<std::string, std::string>::iterator it = req.formData.begin();
			it != req.formData.end(); it++) {
		log("Key: " + it->first + " Value: " + it->second);
	}
	log("End form data");
}

void processCookies(struct http_request &req) {
	if (req.headers.find("cookie") == req.headers.end())
		return;
	std::deque < std::string > cookies = split(req.headers["cookie"], ";");
	for (std::string cookie : cookies) {
		size_t pos = cookie.find("=");
		std::string key =
				(pos == std::string::npos) ?
						cookie : trim(split(cookie, "=").at(0));
		std::string value =
				(pos == std::string::npos) ?
						"" : trim(split(cookie, "=").at(1));
		req.cookies[trim(key)] = trim(value);
	}
	req.headers.erase("cookie");
}

std::string readLines(int *client_fd) {
	struct timeval timeout;
	timeout.tv_sec = 10;
	timeout.tv_usec = 0;
	if (setsockopt(*client_fd, SOL_SOCKET, SO_RCVTIMEO, (char*) &timeout,
			sizeof(timeout)) < 0)
		log("setsockopt failed\n");

	char buffer[1001];
	bzero(buffer, 1000);
	int message_size = 0;

	/* Read until carriage return */
	while (message_size < 1000 && strstr(buffer, "\n") == NULL) {
		int curr_bytes = read(*client_fd, &buffer[message_size],
				1000 - message_size);
		if (curr_bytes <= 0) {
			return "";
		}
		message_size += curr_bytes;
	}

	return std::string(buffer, message_size);
}

struct http_request parseRequest(int *client_fd) {
	struct http_request req;
	bool headers_done = false;

	std::string lines = readLines(client_fd);
	size_t newline_pos = lines.find("\n");
	std::string delimiter = "\n";
	std::string first_line = lines.substr(0, newline_pos);
	lines.erase(0, newline_pos + delimiter.length());

	log("First line: " + trim(first_line));
	if (first_line.compare("") == 0) {
		req.valid = false;
		return req;
	}
	std::deque < std::string > headr = split(trim(first_line), " ");
	req.type = trim(headr.at(0));
	req.filepath = trim(headr.at(1));
	req.version = trim(headr.at(2));

	int header_count = 0;
	if (lines.compare("") == 0)
		lines = readLines(client_fd);
	while (lines.length() > 0) {
		newline_pos = lines.find("\n");
		if (newline_pos == std::string::npos) {
			lines += readLines(client_fd);
			newline_pos = lines.find("\n");
			if (newline_pos == std::string::npos) {
				req.valid = false;
				return req;
			}
		}
		std::string line = lines.substr(0, newline_pos);
		lines.erase(0, newline_pos + delimiter.length());
		if ((trim(line)).compare("") == 0) {
			headers_done = true;
			break;
		}
		log("Header: " + line);
		headr = split(trim(line), ":");
		if (headr.size() == 0 || headr.at(0).compare(line) == 0) {
			req.valid = false;
			return req;
		}
		req.headers[lower(trim(headr.at(0)))] = trim(headr.at(1));
		header_count++;
		if (lines.compare("") == 0 && !headers_done)
			lines = readLines(client_fd);
	}
	processCookies(req);

// Remove used header from lines
	int content_length = 0;
	if (req.headers.find("content-length") != req.headers.end()) {
		req.content = lines;
		int content_length = 0;
		try {
			content_length = stoi(req.headers["content-length"])
					- req.content.size();
			log(
					"Trying to read content of length "
							+ req.headers["content-length"]);
			char *buffer;
			while ((buffer = (char*) malloc(sizeof(char) * (content_length + 1)))
					== NULL)
				;
			int rlen = readNBytes(client_fd, content_length, buffer);
			req.content.append(buffer, rlen);
			free(buffer);
		} catch (const std::invalid_argument &ia) {
			log("Invalid number: " + req.headers["content-length"]);
			return req;
		}
	}

	log("Actual content size: " + std::to_string(req.content.size()));

// Process form or file if necessary
	if (req.headers.find("content-type") != req.headers.end()) {
		if (req.headers["content-type"].find("multipart/form-data")
				!= std::string::npos)
			processMultiPart(req);
		if (req.headers["content-type"].find(
				"application/x-www-form-urlencoded") != std::string::npos)
			processForm(req);
	}
	return req;
}

// Attributed to https://stackoverflow.com/questions/1688432/querying-mx-record-in-c-linux and
// https://stackoverflow.com/questions/52727565/client-in-c-use-gethostbyname-or-getaddrinfo
int getSocketExternalMail(const char *name) {
	char *mxs[10];
	unsigned char response[NS_PACKETSZ];
	ns_msg handle;
	ns_rr rr;
	int mx_index, ns_index, len;
	char dispbuf[4096];
	if ((len = res_search(name, C_IN, T_MX, response, sizeof(response))) < 0) {
		return -1;
	}

	if (ns_initparse(response, len, &handle) < 0) {
		return 0;
	}

	len = ns_msg_count(handle, ns_s_an);
	if (len < 0)
		return 0;
	for (mx_index = 0, ns_index = 0; mx_index < 10 && ns_index < len;
			ns_index++) {
		if (ns_parserr(&handle, ns_s_an, ns_index, &rr)) {
			continue;
		}
		ns_sprintrr(&handle, &rr, NULL, NULL, dispbuf, sizeof(dispbuf));
		if (ns_rr_class(rr) == ns_c_in && ns_rr_type(rr) == ns_t_mx) {
			char mxname[MAXDNAME];
			dn_expand(ns_msg_base(handle),
					ns_msg_base(handle) + ns_msg_size(handle),
					ns_rr_rdata(rr) + NS_INT16SZ, mxname, sizeof(mxname));
			mxs[mx_index++] = strdup(mxname);
		}
	}
	const char *hostname = mxs[mx_index - 1];
	const char *port = "25";
	struct hostent *host;
	if ((host = gethostbyname(hostname)) == nullptr) {
		perror(hostname);
		return -1;
	}
	struct addrinfo hints = { 0 }, *addrs;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	const int ERROR_STATUS = -1;
	const int status = getaddrinfo(hostname, port, &hints, &addrs);
	if (status != 0) {
		fprintf(stderr, "%s: %s\n", hostname, gai_strerror(status));
		return -1;
	}
	int sfd, err;
	for (struct addrinfo *addr = addrs; addr != nullptr; addr = addr->ai_next) {
		sfd = socket(addrs->ai_family, addrs->ai_socktype, addrs->ai_protocol);
		if (sfd == ERROR_STATUS) {
			err = errno;
			continue;
		}

		if (connect(sfd, addr->ai_addr, addr->ai_addrlen) == 0) {
			break;
		}

		err = errno;
		sfd = ERROR_STATUS;
		close(sfd);
	}
	freeaddrinfo(addrs);

	if (sfd == ERROR_STATUS) {
		fprintf(stderr, "%s: %s\n", hostname, strerror(err));
		return -1;
	}

	return sfd;
}

std::string escape(std::string input) {
	std::string output = "";
	output.reserve(input.size());
	for (const char c : input) {
		switch (c) {
		case '<':
			output += "&lt;";
			break;
		case '>':
			output += "&gt;";
			break;
		default:
			output += c;
			break;
		}
	}
	return output;
}

///////////////////////////////////////////////////////////////////////////

// Attributed to arthurafarias on Github: https://gist.github.com/arthurafarias/56fec2cd49a32f374c02d1df2b6c350f

std::string decodeURIComponent(std::string encoded) {

	std::string decoded = encoded;
	std::smatch sm;
	std::string haystack;

	int dynamicLength = decoded.size() - 2;

	if (decoded.size() < 3)
		return decoded;

	for (int i = 0; i < dynamicLength; i++) {

		haystack = decoded.substr(i, 3);

		if (std::regex_match(haystack, sm, std::regex("%[0-9A-F]{2}"))) {
			haystack = haystack.replace(0, 1, "0x");
			std::string rc = { (char) std::stoi(haystack, nullptr, 16) };
			decoded = decoded.replace(decoded.begin() + i,
					decoded.begin() + i + 3, rc);
		}

		dynamicLength = decoded.size() - 2;

	}

	return decoded;
}

std::string encodeURIComponent(std::string decoded) {

	std::ostringstream oss;
	std::regex r("[!'\\(\\)*-.0-9A-Za-z_~]");

	for (char &c : decoded) {
		if (std::regex_match((std::string ) { c }, r)) {
			oss << c;
		} else {
			oss << "%" << std::uppercase << std::hex << (0xff & c);
		}
	}
	return oss.str();
}

///////////////////////////////////////////////////////////////////////////

struct http_response processRequest(struct http_request &req) {
	struct http_response resp;
	for (std::map<std::string, std::string>::iterator it = req.cookies.begin();
			it != req.cookies.end(); it++) {
		resp.cookies[it->first] = it->second;
	}

	/* Check to see if I'm the load balancer and if this request needs to be redirected */
	if (load_balancer && frontend_server_list.size() > 1
			&& req.cookies.find("redirected") == req.cookies.end()) {
		std::string redirect_server = "";
		pthread_mutex_lock(&access_state_map);
		int first_time = 0;
		while (redirect_server.compare("") == 0
				|| time(NULL)
						- frontend_state_map[redirect_server].last_modified > 3) {
			redirect_server = frontend_server_list[l_balancer_index];
			if (first_time < 2) {
				log("Redirect server: " + redirect_server);
				log_server_state(frontend_state_map[redirect_server]);
				first_time += 1;
			}
			l_balancer_index = (l_balancer_index + 1)
					% frontend_server_list.size();
		}
		pthread_mutex_unlock(&access_state_map);

		if (redirect_server.compare(this_server_state.http_address) == 0) {
			resp.cookies["redirected"] = "true";
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "http://" + redirect_server + "/";
			resp.cookies["redirected"] = "true";
			return resp;
		}
	}

	if (req.cookies.find("username") == req.cookies.end()
			&& req.cookies.find("sessionid") == req.cookies.end()) {
	} else if (req.cookies.find("username") == req.cookies.end()
			|| req.cookies.find("sessionid") == req.cookies.end()) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.cookies.erase("username");
		}
		if (req.cookies.find("sessionid") != req.cookies.end()) {
			resp.cookies.erase("sessionid");
		}
		resp.status_code = 307;
		resp.status = "Temporary Redirect";
		resp.headers["Location"] = "/";
		return resp;
	} else {
		resp_tuple getResp = getKVS("session", "session",
				req.cookies["sessionid"]);
		std::string getRespMsg = kvsResponseMsg(getResp);
		int getRespStatusCode = kvsResponseStatusCode(getResp);
		if (getRespStatusCode != 0 || getRespMsg != req.cookies["username"]) {
			resp.cookies.erase("username");
			resp.cookies.erase("sessionid");
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
			return resp;
		}
	}

	if (req.formData["dir_name"].size() > 0) {
// File present to upload
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			createDirectory(req, filepath, req.formData["dir_name"]);
		}
	} else if (req.formData["file"].size() > 0) {
// File present to upload
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			uploadFile(req, filepath);
		}
	} else if (req.formData["itemToDelete"].size() > 0) {
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			std::string itemToDelete = req.formData["itemToDelete"];
			if (itemToDelete.at(2) == '1') {
				// itemToDelete is a FILE
				deleteFile(req, filepath, itemToDelete);
			} else if (itemToDelete.at(2) == '0') {
				// itemToDelete is a DIRECTORY
				// Recursively delete all subdirectories and files
				deleteDirectory(req, filepath, itemToDelete);
			}
		}
	} else if (req.formData["itemToRename"].size() > 0
			&& req.formData["newName"].size() > 0) {
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			std::string itemToRename = req.formData["itemToRename"];
			std::string newName = req.formData["newName"];
			newName = decodeURIComponent(newName);
			newName = decodeURIComponent(newName);
			size_t index = 0;
			while (true) {
				index = newName.find("%D", index);
				if (index == std::string::npos)
					break;
				newName.replace(index, 2, "\r");
				index += 2;
			}
			renameFile(req, filepath, itemToRename, newName);
		}
	} else if (req.formData["itemToMove"].size() > 0
			&& req.formData["newLocation"].size() > 0) {
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			std::string itemToMove = req.formData["itemToMove"];
			std::string newLocation = req.formData["newLocation"];
			std::string target;
			newLocation = decodeURIComponent(newLocation);
			newLocation = decodeURIComponent(newLocation);
			size_t index = 0;
			while (true) {
				index = newLocation.find("%D", index);
				if (index == std::string::npos)
					break;
				newLocation.replace(index, 2, "\r");
				index += 2;
			}
			int status = moveFile(req, filepath, itemToMove, newLocation,
					target);
			if (status == 0) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/files/" + target;
				return resp;
			}
		}
	}

	if (req.filepath.compare("/") == 0) {
		if (req.cookies.find("username") == req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string test = "";
			bool signuperr = false;
			if (req.cookies.find("error") != req.cookies.end()) {
				test = "<p style=\"color:red\";>" + req.cookies["error"]
						+ "</p><br/>";
				resp.cookies.erase("error");
				if (req.cookies.find("signuperr") != req.cookies.end()) {
					signuperr = true;
					resp.cookies.erase("signuperr");
				}
			}
			resp.content =
					"<head><meta charset=\"UTF-8\"></head>"
							"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
							"center;\">" + test
							+ "<form id=\"login\" style=\"display:"
							+ (signuperr ? "none" : "block")
							+ ";\" action=\"/login\" enctype=\"multipart/form-data\" method=\"POST\""
									"<label for =\"username\">Username:</label><br/><input required name=\"username\" type=\"text\"/><br/>"
									"<label for=\"password\">Password:</label><br/><input required name=\"password\" "
									"type=\"password\"/><br/>"
									"<br/><input type=\"submit\" name=\"submit\" value=\"Log In\"><br/>"
									"</form>"
									"<form id=\"signup\" style=\"display:"
							+ (signuperr ? "block" : "none")
							+ ";\" action=\"/signup\" "
									"enctype=\"multipart/form-data\" "
									"method=\"POST\""
									"<label for =\"username\">Username:</label><br/><input required name=\"username\" type=\"text\"/><br/>"
									"<label for=\"password\">Password:</label><br/><input required name=\"password\" "
									"type=\"password\"/><br/>"
									"<label for=\"confirm_password\">Confirm Password:</label><br/><input required "
									"name=\"confirm_password\" "
									"type=\"password\"/><br/>"
									"<br/><input type=\"submit\" name=\"submit\" value=\"Sign Up\"><br/>"
									"</form>"
									"<br/><button id=\"switchButton\" type=\"button\">"
							+ (signuperr ?
									"Have an account? Log in!" :
									"Don't have an account? Sign up!")
							+ "</button>"
									"<script>"
									"var switchButton=document.getElementById('switchButton');"
									"switchButton.onclick=function(){var "
									"loginForm=document.getElementById('login');switchButton.innerHTML=(loginForm.style.display "
									"== "
									"'none') ? \"Don't have an account? Sign up!\" : 'Have an account? Log in!';"
									"loginForm.style.display=(loginForm.style.display == 'none') ? 'block' : 'none';"
									"var "
									"signupForm=document.getElementById('signup');signupForm.style.display=(signupForm.style."
									"display "
									"== 'none') ? 'block' : 'none';}"
									"</script>"
									"</body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		}
	} else if (req.filepath.compare("/login") == 0) {
		if (req.cookies.find("username") == req.cookies.end()) {
			if (req.formData["username"] == "") {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] = "Invalid username.";
			} else {
				resp_tuple getResp = getKVS("session", req.formData["username"],
						"password");
				std::string getRespMsg = kvsResponseMsg(getResp);
				int getRespStatusCode = kvsResponseStatusCode(getResp);
				if (getRespStatusCode != 0) {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/";
					resp.cookies["error"] = "Invalid username.";
				} else if (getRespMsg != req.formData["password"]) {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/";
					resp.cookies["error"] = "Invalid password.";
				} else {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					if (req.formData["username"].compare("admin") != 0) {
						resp.headers["Location"] = "/dashboard";
					} else {
						resp.headers["Location"] = "/admin";
					}
					resp.cookies["username"] = req.formData["username"];
					resp.cookies["sessionid"] = generateSessionID();
					putKVS(resp.cookies["username"], "session",
							resp.cookies["sessionid"],
							resp.cookies["username"]);
				}
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		}
	} else if (req.filepath.compare("/signup") == 0) {
		if (req.cookies.find("username") == req.cookies.end()) {
			bool valid = true;
			for (int i = 0; i < req.formData["username"].size(); i++) {
				if (!isalnum(req.formData["username"][i]))
					valid = false;
			}
			if (req.formData["username"].size() == 0 || !valid) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] =
						"Username is required, and must be alphanumeric and not contain any spaces.";
				resp.cookies["signuperr"] = "1";
			} else if (req.formData["password"].size() == 0
					|| std::all_of(req.formData["password"].begin(),
							req.formData["password"].end(), isspace)) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] =
						"Password cannot be empty or only spaces.";
				resp.cookies["signuperr"] = "1";
			} else if (req.formData["password"]
					!= req.formData["confirm_password"]) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] = "Passwords do not match.";
				resp.cookies["signuperr"] = "1";
			} else {
				resp_tuple getResp = getKVS("session", req.formData["username"],
						"mailbox");
				std::string getRespMsg = kvsResponseMsg(getResp);
				int getRespStatusCode = kvsResponseStatusCode(getResp);
				if (getRespStatusCode == 0) {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/";
					resp.cookies["error"] = "User already exists.";
					resp.cookies["signuperr"] = "1";
				} else {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/dashboard";
					resp.cookies["username"] = req.formData["username"];
					resp.cookies["sessionid"] = generateSessionID();
					putKVS(resp.cookies["username"], "session",
							resp.cookies["sessionid"],
							resp.cookies["username"]);
					putKVS(resp.cookies["sessionid"], req.formData["username"],
							"password", req.formData["password"]);
					putKVS(resp.cookies["sessionid"], req.formData["username"],
							"mailbox", "");
					createRootDirForNewUser(req, resp.cookies["sessionid"]);
				}
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		}
	} else if (req.filepath.compare("/dashboard") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string userRootDir = "ss0_"
					+ generateStringHash(req.cookies["username"] + "/");
			resp.content =
					"<head><meta charset=\"UTF-8\"></head>"
							"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
							"center;\">"
							"<form action=\"/mailbox\" method=\"POST\"> <input type = \"submit\" value=\"Mailbox\" /></form>"
							"<form action=\"/compose\" method=\"POST\"> <input type = \"submit\" value=\"Compose Email\" /></form>"
							"<form action=\"/files/" + userRootDir
							+ "\" method=\"POST\"> <input type = \"submit\" value=\"Storage Service\" /></form>"
									"<form action=\"/change-password\" method=\"POST\"><input type = \"submit\" value=\"Change Password\" /></form>"
									"</body></html>"
									"<form action=\"/logout\" method=\"POST\"><input type = \"submit\" value=\"Logout\" /></form>"
									"</body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 7, "/files/") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			if (req.filepath.length() > 7) {
				std::string filepath = req.filepath.substr(7);
				resp_tuple getFileResp = getKVS(req.cookies["sessionid"],
						req.cookies["username"], filepath);
				if (kvsResponseStatusCode(getFileResp) == 0) {
					// display list of files if route = directory. else, display file contents
					if (isFileRouteDirectory(filepath)) {
						resp.status_code = 200;
						resp.status = "OK";
						resp.headers["Content-type"] = "text/html";
						std::string fileList = getFileList(req, filepath);
						resp.content =
								"<head><meta charset=\"UTF-8\"></head>"
										"<html><body>"
										"" + fileList + "<br/>"
										"<form action=\"/files/" + filepath
										+ "\" enctype=\"multipart/form-data\" method=\"POST\""
												"<label for=\"file\">Upload a new File</label><br/><input type=\"file\" name=\"file\"/><br/>"
												"<input type=\"submit\" name=\"submit\" value=\"Upload\"><br/>"
												"</form>"
												"<form action=\"/files/"
										+ filepath
										+ "\" enctype=\"multipart/form-data\" method=\"POST\""
												"<label for=\"dir_name\">New Directory Name</label><br/><input type=\"text\" name=\"dir_name\" placeholder=\"Create Directory\"/><br/>"
												"<input type=\"submit\" name=\"submit\" value=\"Submit\"><br/>"
												"</form>"
												"</body></html>";
					} else {
						resp.status_code = 200;
						resp.status = "OK";
						resp.headers["Content-type"] = "text/plain";
						resp.content = kvsResponseMsg(getFileResp);
					}
				} else {
					resp.status_code = 404;
					resp.status = "Not found";
					resp.headers["Content-type"] = "text/html";
					resp.content = "<head><meta charset=\"UTF-8\"></head>"
							"<html><body>"
							"Requested file not found!"
							"</body></html>";
				}
			} else {
				resp.status_code = 404;
				resp.status = "Not found";
				resp.headers["Content-type"] = "text/html";
				resp.content = "<head><meta charset=\"UTF-8\"></head>"
						"<html><body>"
						"Requested file not found!"
						"</body></html>";
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/logout") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.cookies.erase("username");
		}
		if (req.cookies.find("sessionid") != req.cookies.end()) {
			deleteKVS(req.cookies["sessionid"], "session",
					req.cookies["sessionid"]);
			resp.cookies.erase("sessionid");
		}
		resp.status_code = 307;
		resp.status = "Temporary Redirect";
		resp.headers["Location"] = "/";
	} else if (req.filepath.compare("/mailbox") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			resp_tuple getResp = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "mailbox");
			std::string getRespMsg = kvsResponseMsg(getResp);
			std::stringstream ss(getRespMsg);
			std::string to;
			std::string subject;
			std::string display = "";
			if (getRespMsg != "") {
				while (std::getline(ss, to, '\n')) {
					if (to.rfind("From <", 0) == 0) {
						std::getline(ss, subject, '\n');
						std::string title = "";
						std::string title1 = "";
						std::string title2 = "";
						unsigned first = to.find('<');
						unsigned last = to.find('>');
						title = to.substr(first + 1, last - first - 1);
						title1 = subject.substr(9);
						title2 = to.substr(last + 2);
						display +=
								"<ul style=\"border-top: 1px solid black; padding:15px; margin: 0;\">";
						display +=
								"<div style=\"display:flex; flex-direction: row;\">"
										"<form action=\"/email\" method=\"post\" style=\"margin: 0;\">"
										"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to) + "\" />"
										+ "<label for =\"submit\" style=\"margin-right: 20px; width: 300px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
										+ escape(title) + "</label>"
										+ "<label for =\"submit\" style=\"margin-right: 20px; width: 300px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
										+ escape(title1) + "</label>"
										+ "<label for =\"submit\" style=\"margin-right: 20px; vertical-align: middle;\">"
										+ escape(title2) + "</label>"
										+ "<input type=\"submit\" name=\"submit\" value=\"View\" />"
												"</form>"
												"<form style=\"padding-left:15px; padding-right:15px; margin: 0;\" action=\"/compose\" method=\"POST\">"
												"<input type=\"hidden\" name=\"type\" value=\"reply\">"
												"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to)
										+ "\" />"
												"<input type = \"submit\" value=\"Reply\" /></form>"
												"<form action=\"/compose\" method=\"POST\" style=\"margin-bottom:0; padding-right:15px;\">"
												"<input type=\"hidden\" name=\"type\" value=\"forward\">"
												"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to)
										+ "\" />" "<input type = \"submit\" value=\"Forward\" /></form>"
												"<form action=\"/delete\" method=\"POST\" style=\"margin-bottom:0;\">"
												"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to)
										+ "\" />" "<input type = \"submit\" value=\"Delete\" /></form></div>";
						display += "</ul>";
					}
				}
			}
			if (display == "") {
				display +=
						"<ul style=\"border-top: 1px solid black; padding:15px; margin: 0;\">No mail yet!</ul>";
			}
			display +=
					"<ul style=\"border-top: 1px solid black; padding:0px; margin: 0;\"></ul>";
			resp.content =
					"<head><meta charset=\"UTF-8\"></head>"
							"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;padding:10px;\">"
							"<div style=\"display:flex; flex-direction: row;\"><form style=\"padding-left:15px; padding-right:15px; margin-bottom:18px;\" action=\"/dashboard\" method=\"POST\"> <input type = \"submit\" value=\"Dashboard\" /></form>"
							"<form action=\"/compose\" method=\"POST\" style=\"margin-bottom:18px;\"> <input type = \"submit\" value=\"Compose Email\"/></form></div>" "<div style=\"padding-left: 15px;padding-bottom: 5px;padding-top: 10px; display:flex; flex-direction: row;\"><label style=\"margin-right: 20px; width: 300px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">Sender</label>"
							"<label style=\"margin-right: 20px; width: 300px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">Subject</label>"
							"<label style=\"margin-right: 20px; vertical-align:middle;\">Date</label>"
							"</div>" + display + "</body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/compose") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string header = "";
			if (req.formData.find("header") != req.formData.end()) {
				header = decodeURIComponent(req.formData["header"]);
				header = decodeURIComponent(header);
				size_t index = 0;
				while (true) {
					index = header.find("%D", index);
					if (index == std::string::npos)
						break;
					header.replace(index, 2, "\r");
					index += 2;
				}
			}
			std::string type = "";
			if (req.formData.find("type") != req.formData.end()) {
				type = req.formData["type"];
			}
			resp_tuple getResp = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "mailbox");
			std::string getRespMsg = kvsResponseMsg(getResp);
			int getRespStatusCode = kvsResponseStatusCode(getResp);
			std::string existing = "";
			std::string fullSubject = "";
			std::string fullHeader = "";
			std::string rec = "";
			std::string sub = "";
			if (header != "" && (type == "forward" || type == "reply")) {
				std::stringstream ss(getRespMsg);
				std::string to;
				if (getRespMsg != "") {
					bool found = false;
					bool foundHeader = false;
					while (std::getline(ss, to, '\n')) {
						if (!foundHeader && to.rfind(header, 0) == 0) {
							fullHeader = to;
							foundHeader = true;
						} else if (foundHeader) {
							if (!found) {
								fullSubject = to;
								found = true;
							} else if (to.rfind("From <", 0) == 0) {
								break;
							} else {
								existing += to + "\n";
							}
						}
					}
				}
				if (existing != "") {
					std::string temp;
					if (type == "forward") {
						temp =
								"\n\n\n----------------------------------------\n\nFwd:&nbsp;"
										+ fullHeader + fullSubject + "\n\n";
						sub = "Fwd: "
								+ fullSubject.substr(9, fullSubject.size() - 2);
					} else {
						temp =
								"\n\n\n----------------------------------------\n\nRe:&nbsp;"
										+ fullHeader + fullSubject + "\n\n";
						unsigned first = fullHeader.find('<');
						unsigned last = fullHeader.find('>');
						rec = fullHeader.substr(first + 1, last - first - 1);
						sub = "Re: "
								+ fullSubject.substr(9, fullSubject.size() - 2);
					}
					temp += existing;
					existing = temp;
				}
			}
			resp.content =
					"<head><meta charset=\"UTF-8\"></head>"
							"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;padding:10px;\">"
							"<div style=\"display:flex; flex-direction: row;\"><form style=\"padding-left:15px; padding-right:15px; margin-bottom:18px;\" action=\"/mailbox\" method=\"POST\"> <input type = \"submit\" value=\"Discard\" /></form>"
							"<script>function encode() {document.getElementsByName(\"to\")[0].value = encodeURIComponent(document.getElementsByName(\"to\")[0].value); document.getElementsByName(\"subject\")[0].value = encodeURIComponent(document.getElementsByName(\"subject\")[0].value); document.getElementsByName(\"content\")[0].value = encodeURIComponent(document.getElementsByName(\"content\")[0].value); return true;}</script>"
							"<form accept-charset=\"utf-8\" id=\"compose\" action=\"/send\" onsubmit=\"return encode();\" method=\"POST\" style=\"margin-bottom:18px;\"> <input type = \"submit\" value=\"Send\" /></form></div>"
							"<ul style=\"border-top: 1px solid black; padding:0px; margin: 0;\"></ul>"
							"<div style=\"display:flex; flex-direction: row; padding: 15px; \">"
							"<label form=\"compose\" for=\"to\" style=\"height:30px; display: flex; align-items: center; width: 75px;\">To:&nbsp;</label><input required form=\"compose\" style=\"flex:1;\" name=\"to\" type=\"text\" value=\""
							+ rec + "\"/></div>"
							+ "<ul style=\"border-top: 1px solid black; padding:0px; margin: 0;\"></ul>"
									"<div style=\"display:flex; flex-direction: row; padding: 15px; \">"
									"<label form=\"compose\" for=\"subject\" style=\"height:30px; display: flex; align-items: center; width: 75px;\">Subject:&nbsp;</label><input required form=\"compose\" style=\"flex:1;\" name=\"subject\" type=\"text\" value=\""
							+ sub + "\"/></div>"
							+ "<ul style=\"border-top: 1px solid black; padding:0px; margin: 0;\"></ul>"
									"<div style=\"padding:15px\">"
									"<textarea name=\"content\" form=\"compose\" style=\"width:100%; height: 450px;\">"
							+ existing + "</textarea>"
									"</div>"
									"</body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/email") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			if (req.formData.find("header") != req.formData.end()) {
				resp.status_code = 200;
				resp.status = "OK";
				resp.headers["Content-type"] = "text/html";
				resp_tuple getResp = getKVS(req.cookies["sessionid"],
						req.cookies["username"], "mailbox");
				std::string getRespMsg = kvsResponseMsg(getResp);
				std::stringstream ss(getRespMsg);
				std::string to;
				std::string subject;
				std::string display = "";
				std::string header = decodeURIComponent(req.formData["header"]);
				header = decodeURIComponent(header);
				size_t index = 0;
				while (true) {
					index = header.find("%D", index);
					if (index == std::string::npos)
						break;
					header.replace(index, 2, "\r");
					index += 2;
				}
				if (getRespMsg != "") {
					bool found = false;
					std::string message = "";
					while (std::getline(ss, to, '\n')) {
						if (!found && to.rfind(header, 0) == 0) {
							std::getline(ss, subject, '\n');
							std::string title = "From: ";
							std::string title1 = "Subject: ";
							std::string title2 = "";
							unsigned first = to.find('<');
							unsigned last = to.find('>');
							title += to.substr(first + 1, last - first - 1);
							title1 += subject.substr(9);
							title2 = to.substr(last + 2);
							display +=
									"<ul style=\"border-bottom: 1px solid black; padding:15px; margin: 0;\">";
							display +=
									"<label style=\"margin-right: 20px; margin-bottom: 10px; width: 100%; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
											+ escape(title) + "</label>"
											+ "<label style=\"margin-right: 20px; margin-bottom: 10px; width: 100%; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
											+ escape(title1) + "</label>"
											+ "<label style=\"margin-right: 20px; margin-bottom: 10px; vertical-align: middle;\">"
											+ escape(title2) + "</label>";
							display += "</ul>";
							found = true;
							display +=
									"<span style=\"white-space: pre-wrap; padding:15px;\">";
						} else if (found) {
							if (to.rfind("From <", 0) == 0) {
								break;
							} else {
								message += to + "\n";
							}
						}
					}
					if (found) {
						display += escape(message);
						display += "</span>";
					}
				}
				resp.content =
						"<head><meta charset=\"UTF-8\"></head>"
								"<html><body "
								"style=\"display:flex;flex-direction:column;height:100%;padding:10px;\">"
								"<div style=\"display:flex; flex-direction: row;\"><form style=\"padding-left:15px; padding-right:15px; margin-bottom:18px;\" action=\"/dashboard\" method=\"POST\"> <input type = \"submit\" value=\"Dashboard\" /></form>"
								"<form action=\"/mailbox\" method=\"POST\" style=\"padding-right: 15px; margin-bottom:18px;\"> <input type = \"submit\" value=\"Mailbox\" /></form>"
								"<form style=\"padding-right:15px; margin: 0;\" action=\"/compose\" method=\"POST\">"
								"<input type=\"hidden\" name=\"type\" value=\"reply\">"
								"<input type=\"hidden\" name=\"header\" value=\""
								+ encodeURIComponent(header)
								+ "\" />"
										"<input type = \"submit\" value=\"Reply\" /></form>"
										"<form action=\"/compose\" method=\"POST\" style=\"margin-bottom:0; padding-right:15px;\">"
										"<input type=\"hidden\" name=\"type\" value=\"forward\">"
										"<input type=\"hidden\" name=\"header\" value=\""
								+ encodeURIComponent(header)
								+ "\" />" "<input type = \"submit\" value=\"Forward\" /></form>"
										"<form action=\"/delete\" method=\"POST\" style=\"margin-bottom:0;\">"
										"<input type=\"hidden\" name=\"header\" value=\""
								+ encodeURIComponent(header)
								+ "\" />" "<input type = \"submit\" value=\"Delete\" /></form></div>"
										"<ul style=\"border-top: 1px solid black; padding:0px; margin: 0;\"></ul>"
								+ display + "</body></html>";
				resp.headers["Content-length"] = std::to_string(
						resp.content.size());
			} else {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/mailbox";
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/delete") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			if (req.formData.find("header") != req.formData.end()) {
				std::string header = decodeURIComponent(req.formData["header"]);
				header = decodeURIComponent(header);
				size_t index = 0;
				while (true) {
					index = header.find("%D", index);
					if (index == std::string::npos)
						break;
					header.replace(index, 2, "\r");
					index += 2;
				}
				int respStatus2 = 1;
				while (respStatus2 != 0) {
					resp_tuple getResp = getKVS(req.cookies["sessionid"],
							req.cookies["username"], "mailbox");
					std::string getRespMsg = kvsResponseMsg(getResp);
					std::stringstream ss(getRespMsg);
					std::string to;
					std::string final = "";
					if (getRespMsg != "") {
						bool found = false;
						bool done = true;
						while (std::getline(ss, to, '\n')) {
							if (!found && to.rfind(header, 0) == 0) {
								found = true;
								done = false;
							} else if (found && !done) {
								if (to.rfind("From <", 0) == 0) {
									done = true;
									final += to + "\n";
								}
							} else {
								final += to + "\n";
							}
						}
					}
					resp_tuple resp2 = cputKVS(req.cookies["sessionid"],
							req.cookies["username"], "mailbox", getRespMsg,
							final);
					respStatus2 = kvsResponseStatusCode(resp2);
				}
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/mailbox";
			} else {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/mailbox";
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/send") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			if (req.formData.find("to") != req.formData.end()) {
				std::string to = decodeURIComponent(req.formData["to"]);
				to = decodeURIComponent(to);
				size_t index = 0;
				while (true) {
					index = to.find("%D", index);
					if (index == std::string::npos)
						break;
					to.replace(index, 2, "\r");
					index += 2;
				}
				std::string subject = decodeURIComponent(
						req.formData["subject"]);
				subject = decodeURIComponent(subject);
				index = 0;
				while (true) {
					index = subject.find("%D", index);
					if (index == std::string::npos)
						break;
					subject.replace(index, 2, "\r");
					index += 2;
				}
				std::string message = "";
				if (req.formData.find("content") != req.formData.end()) {
					message = req.formData["content"];
				}
				message = decodeURIComponent(message);
				message = decodeURIComponent(message);
				index = 0;
				while (true) {
					index = message.find("%D", index);
					if (index == std::string::npos)
						break;
					message.replace(index, 2, "\r");
					index += 2;
				}
				std::replace(to.begin(), to.end(), ';', ',');
				std::istringstream ss { to };
				std::string token;
				bool local;
				std::string ending = "@penncloud.com";
				std::string sender = req.cookies["username"] + ending;
				time_t rawtime;
				struct tm *timeinfo;
				time(&rawtime);
				timeinfo = localtime(&rawtime);
				std::string temp = "From <" + sender + "> " + asctime(timeinfo)
						+ "\n";
				temp[temp.length() - 2] = '\r';
				temp += "Subject: " + subject + "\r\n";
				temp += message;
				temp += "\r\n";
				std::string tempNotLocal = "Subject: " + subject + "\r\n";
				tempNotLocal += message;
				tempNotLocal += "\r\n";
				while (std::getline(ss, token, ',')) {
					if (!token.empty()) {
						size_t first = token.find_first_not_of(' ');
						if (std::string::npos != first) {
							size_t last = token.find_last_not_of(' ');
							token = token.substr(first, (last - first + 1));
						}
						if (token.length() >= ending.length()) {
							local = (0
									== token.compare(
											token.length() - ending.length(),
											ending.length(), ending));
						} else {
							local = false;
						}
						if (!local) {
							std::size_t found = token.find("@");
							if (found != std::string::npos) {
								usleep(1000000);
								int sd = getSocketExternalMail(
										token.substr(found + 1).c_str());
								if (sd > 0) {
									char helo[] = "HELO penncloud.com\r\n";
									do_write(sd, helo, sizeof(helo) - 1);
									char buf[1000];
									buf[20] = '\0';
									read(sd, &buf[0], 1000);
									std::string mailStr = "MAIL FROM:<" + sender
											+ ">\r\n";
									send(sd, mailStr.c_str(), mailStr.length(),
											0);
									read(sd, &buf[0], 1000);
									std::string rcptStr = "RCPT TO:<" + token
											+ ">\r\n";
									send(sd, rcptStr.c_str(), rcptStr.length(),
											0);
									read(sd, &buf[0], 1000);
									char data[] = "DATA\r\n";
									do_write(sd, data, sizeof(data) - 1);
									read(sd, &buf[0], 1000);
									send(sd, tempNotLocal.c_str(),
											tempNotLocal.length(), 0);
									char ending[] = ".\r\n";
									do_write(sd, ending, sizeof(ending) - 1);
									read(sd, &buf[0], 1000);
									char quit[] = "QUIT\r\n";
									do_write(sd, quit, sizeof(quit) - 1);
									read(sd, &buf[0], 1000);
									close(sd);
								}
							}
						} else {
							std::string addr = token.substr(0, token.find("@"));
							resp_tuple getResp = getKVS(addr, addr, "mailbox");
							std::string current = kvsResponseMsg(getResp);
							int getRespStatusCode = kvsResponseStatusCode(
									getResp);
							if (getRespStatusCode == 0) {
								std::string final = temp;
								final += current;
								resp_tuple resp2 = cputKVS(addr, addr,
										"mailbox", current, final);
								int respStatus2 = kvsResponseStatusCode(resp2);
								while (respStatus2 != 0) {
									getResp = getKVS(addr, addr, "mailbox");
									getRespStatusCode = kvsResponseStatusCode(
											getResp);
									current = kvsResponseMsg(getResp);
									final = temp;
									final += current;
									resp2 = cputKVS(addr, addr, "mailbox",
											current, final);
									respStatus2 = kvsResponseStatusCode(resp2);
								}
							}
						}
					}
				}
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/mailbox";
			} else {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/mailbox";
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/admin") == 0) {
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("admin") == 0) {
			time_t now = time(NULL);
			log("now" + std::to_string(now));
			requstStateFromAllServers();
			auto allBackendNodes = getAllNodesKVS();
			auto activeBackendNodes = getActiveNodesKVS();
			std::deque < std::string > activeBackendNodesCollection;
			for (auto node : activeBackendNodes)
				activeBackendNodesCollection.push_back(std::get < 2 > (node));
			sleep(1);
			std::string message =
					"<head><meta charset=\"UTF-8\"></head><html><body><form action=\"/logout\" method=\"POST\">"
							"<input type = \"submit\" value=\"Logout\" /></form><br><h3>Frontend servers:</h3><br><ul><hr>";
			for (std::map<std::string, struct server_state>::iterator it =
					frontend_state_map.begin(); it != frontend_state_map.end();
					it++) {
				log_server_state(it->second);
				log(
						"Last modified: "
								+ std::to_string(it->second.last_modified));
				std::string status =
						(it->second.last_modified >= now) ?
								"Active" : "Not Responding";
				if (this_server_state.http_address.compare(it->first) != 0) {
					message += "<l1>" + it->first;
					message += " Status: " + status;
					message +=
							"<form action=\"/serverinfo/f/" + it->first
									+ "\" method=\"POST\">"
											"<input type = \"submit\" value=\"More Info\" /></form>";
					message +=
							"<form action=\"/stopserver/f/" + it->first
									+ "\" method=\"POST\">"
											"<input type = \"submit\" value=\"Stop\" /></form>";
					message +=
							"<form action=\"/resumeserver/f/" + it->first
									+ "\" method=\"POST\">"
											"<input type = \"submit\" value=\"Resume\" /></form>";
				} else {
					message += "<l1>" + it->first + " (this node)";
					message += " Status: " + status;
					message +=
							"<form action=\"/serverinfo/f/" + it->first
									+ "\" method=\"POST\">"
											"<input type = \"submit\" value=\"More Info\" /></form>";
				}
				message += "</l1><hr>";
			}
			message += "</ul><h3>Backend servers:</h3><ul>";
			log("ADMIN all nodes");
			for (auto node : allBackendNodes) {
				log(std::get < 2 > (node));
				std::string status =
						(std::find(activeBackendNodesCollection.begin(),
								activeBackendNodesCollection.end(),
								std::get < 2 > (node))
								!= activeBackendNodesCollection.end()) ?
								"Active" : "Not Responding";
				message += "<l1>" + std::get < 2 > (node);
				message += " Status: " + status;
				message +=
						"<form action=\"/serverinfo/b/" + std::get < 3
								> (node)
										+ "\" method=\"POST\">"
												"<input type = \"submit\" value=\"More Info\" /></form>";
				message +=
						"<form action=\"/stopserver/b/" + std::get < 3
								> (node)
										+ "\" method=\"POST\">"
												"<input type = \"submit\" value=\"Stop\" /></form>";
				message +=
						"<form action=\"/resumeserver/b/" + std::get < 3
								> (node)
										+ "\" method=\"POST\">"
												"<input type = \"submit\" value=\"Resume\" /></form>";
				message += "</l1><hr>";
			}
			message += "</ul></body></html>";
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			resp.headers["Content-length"] = std::to_string(message.length());
			resp.content = message;
		}
	} else if (req.filepath.compare("/change-password") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string test = "";
			if (req.cookies.find("error") != req.cookies.end()) {
				test = "<p style=\"color:red\";>" + req.cookies["error"]
						+ "</p><br/>";
				resp.cookies.erase("error");
			}
			resp.content =
					"<head><meta charset=\"UTF-8\"></head>"
							"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
							"center;\">" + test
							+ "<form id=\"change\" style=\"display: block;\""
							+ "action=\"/change\" "
									"enctype=\"multipart/form-data\" "
									"method=\"POST\""
									"<label for =\"old\">Current Password:</label><br/><input required name=\"old\" type=\"password\"/><br/>"
									"<label for=\"new\">New Password:</label><br/><input required name=\"new\" "
									"type=\"password\"/><br/>"
									"<label for=\"confirm_new\">Confirm Password:</label><br/><input required "
									"name=\"confirm_new\" "
									"type=\"password\"/><br/>"
									"<br/><input type=\"submit\" name=\"submit\" value=\"Change Password\"><br/>"
									"</form>"
									"</body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/change") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			if (req.formData["old"].size() == 0) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/change-password";
				resp.cookies["error"] = "Current password is required.";
			} else if (req.formData["new"].size() == 0
					|| std::all_of(req.formData["new"].begin(),
							req.formData["new"].end(), isspace)) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/change-password";
				resp.cookies["error"] =
						"New password cannot be empty or only spaces.";
			} else if (req.formData["new"] != req.formData["confirm_new"]) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/change-password";
				resp.cookies["error"] = "New passwords do not match.";
			} else {
				resp_tuple getResp = getKVS(req.cookies["sessionid"],
						req.cookies["username"], "password");
				std::string getRespMsg = kvsResponseMsg(getResp);
				int getRespStatusCode = kvsResponseStatusCode(getResp);
				if (getRespMsg != req.formData["old"]
						|| getRespStatusCode != 0) {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/change-password";
					resp.cookies["error"] = "Current password is not correct.";
				} else {
					resp_tuple resp2 = cputKVS(req.cookies["sessionid"],
							req.cookies["username"], "password",
							req.formData["old"], req.formData["new"]);
					int respStatus2 = kvsResponseStatusCode(resp2);
					if (respStatus2 != 0) {
						resp.status_code = 307;
						resp.status = "Temporary Redirect";
						resp.headers["Location"] = "/change-password";
						resp.cookies["error"] =
								"Error updating password or current password is not correct.";
					} else {
						resp.status_code = 307;
						resp.status = "Temporary Redirect";
						resp.headers["Location"] = "/dashboard";
					}
				}
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 12, "/stopserver/") == 0) {
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("admin") == 0) {
			std::deque < std::string > tokens = split(req.filepath, "/");
			log("SUSPICION: " + tokens[0]);
			std::string target = trim(tokens.back());
			tokens.pop_back();
			bool frontend_target = (trim(tokens.back()).compare("f") == 0);
			log("Stopping: " + target);
			if (frontend_target) {
				int target_index = getServerIndexFromAddr(target);
				log("Target index: " + std::to_string(target_index));
				sendStopMessage(target_index);
			} else {
				stopServerKVS(target);
			}
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/admin";
		}
	} else if (req.filepath.compare(0, 14, "/resumeserver/") == 0) {
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("admin") == 0) {
			std::deque < std::string > tokens = split(req.filepath, "/");
			log("SUSPICION: " + tokens[0]);
			std::string target = trim(tokens.back());
			tokens.pop_back();
			log("Resuming: " + target);
			bool frontend_target = (trim(tokens.back()).compare("f") == 0);
			if (frontend_target) {
				int target_index = getServerIndexFromAddr(target);
				log("Target index: " + std::to_string(target_index));
				sendResumeMessage(target_index);
			} else {
				reviveServerKVS(target);
			}
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/admin";
		}
	} else {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	}
	return resp;
}

void sendResponseToClient(struct http_response &resp, int *client_fd) {
	std::string response;
	response = "HTTP/1.0 " + std::to_string(resp.status_code) + " "
			+ resp.status + "\r\n";
	response += "Connection: close\r\n";
	for (std::map<std::string, std::string>::iterator it = resp.headers.begin();
			it != resp.headers.end(); it++) {
		response += it->first + ":" + it->second + "\r\n";
	}
	if (resp.cookies.size() > 0) {
		for (std::map<std::string, std::string>::iterator it =
				resp.cookies.begin(); it != resp.cookies.end(); it++) {
			response += "Set-cookie: ";
			response += it->first + "=" + it->second + "\r\n";
		}
	}
	if (resp.cookies.find("error") == resp.cookies.end()) {
		response += "Set-cookie: error=deleted; Max-Age=-1\r\n";
	}
	if (resp.cookies.find("signuperr") == resp.cookies.end()) {
		response += "Set-cookie: signuperr=deleted; Max-Age=-1\r\n";
	}
	if (resp.cookies.find("username") == resp.cookies.end()) {
		response += "Set-cookie: username=deleted; Max-Age=-1\r\n";
	}
	if (resp.cookies.find("sessionid") == resp.cookies.end()) {
		response += "Set-cookie: sessionid=deleted; Max-Age=-1\r\n";
	}
	if (resp.content.compare("") != 0) {
		response += "\r\n" + resp.content;
	}
	writeNBytes(client_fd, response.size(), response.data());
	log("Sent: " + response);
}

/***************************** End http util functions ************************/

void* handleClient(void *arg) {
	signal(SIGPIPE, SIG_IGN);
	incrementThreadCounter(HTTP_THREAD);

	/* Initialize buffer and client fd */
	int *client_fd = (int*) arg;
	log("Handling client " + std::to_string(*client_fd));

	/* Parse request from client */
	struct http_request req = parseRequest(client_fd);
	if (!req.valid) {
		close(*client_fd);
		return NULL;
	}

	/* Process newly filled buffer and add commands to queue */
	struct http_response resp = processRequest(req);

	/* Send response to client */
	sendResponseToClient(resp, client_fd);

	pthread_mutex_lock(&fd_mutex);
	fd.erase(client_fd);
	pthread_mutex_unlock(&fd_mutex);
	close(*client_fd);
	free(client_fd);
	decrementThreadCounter(HTTP_THREAD);
	pthread_detach (pthread_self());pthread_exit
	(NULL);
}

void* handle_smtp_connections(void *arg) {
	incrementThreadCounter(SMTP_THREAD);
	sigset_t newmask;
	sigemptyset(&newmask);
	sigaddset(&newmask, SIGINT);
	pthread_sigmask(SIG_BLOCK, &newmask, NULL);
	int comm_fd = *(int*) arg;
	struct timeval timeout;
	timeout.tv_sec = 100;
	timeout.tv_usec = 0;
	if (setsockopt(comm_fd, SOL_SOCKET, SO_RCVTIMEO, (char*) &timeout,
			sizeof(timeout)) < 0)
		log("setsockopt failed\n");
	free(arg);
	char buf[1000];
	std::string sender;
	std::string recipient;
	std::vector < std::string > recipients;
	std::string dataLine;
	std::vector < std::string > data;
	int len = 0;
	int rcvd;
	int start;
	int index;
	int checkIndex;
	bool checked = false;
	bool alreadySet;
	int state = 0;
	char helo[] = "250 penncloud.com\r\n";
	char stateError[] = "503 Bad sequence of commands\r\n";
	char paramError[] = "501 Syntax error in parameters or arguments\r\n";
	char commandError[] = "500 Syntax error, command unrecognized\r\n";
	char quit[] = "221 penncloud.com Service closing transmission channel\r\n";
	char ok[] = "250 OK\r\n";
	char penncloud[] = "@penncloud.com";
	char notLocalError[] = "551 User not local\r\n";
	char noUserError[] = "550 No such user here\r\n";
	char intermediateReply[] =
			"354 Start mail input; end with <CRLF>.<CRLF>\r\n";
	char shutDown[] = "421 penncloud.com Service not available\r\n";
	while (true) {
		alreadySet = false;
		if (checked) {
			rcvd = read(comm_fd, &buf[len], 1000 - len);
		} else
			rcvd = 0;
		len += rcvd;
		if (checked)
			start = len - rcvd;
		else
			start = 0;
		checked = true;
// Check for complete command in unchecked or newly read buffer.
		for (int i = start; i < len - 1; i++) {
			if (buf[i] == '\r' && buf[i + 1] == '\n') {
				if (vflag)
					fprintf(stderr, "[%d] C: %.*s", comm_fd, i + 2, &buf[0]);
				// Handle HELO command.
				if (tolower(buf[0]) == 'h' && i > 3 && tolower(buf[1]) == 'e'
						&& tolower(buf[2]) == 'l' && tolower(buf[3]) == 'o'
						&& (i == 4 || buf[4] == ' ')) {
					if (state != 0 && state != 1) {
						do_write(comm_fd, stateError, sizeof(stateError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(stateError) - 1, stateError);
					} else if (i > 5 && buf[5] != ' ') {
						// Successful HELO command.
						do_write(comm_fd, helo, sizeof(helo) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(helo) - 1, helo);
						state = 1;
					} else {
						do_write(comm_fd, paramError, sizeof(paramError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(paramError) - 1, paramError);
					}
				} // Handle MAIL command.
				else if (tolower(buf[0]) == 'm' && i > 3
						&& tolower(buf[1]) == 'a' && tolower(buf[2]) == 'i'
						&& tolower(buf[3]) == 'l'
						&& (i == 4 || buf[4] == ' ')) {
					if (state == 0) {
						do_write(comm_fd, stateError, sizeof(stateError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(stateError) - 1, stateError);
					} else if (i > 14 && tolower(buf[5]) == 'f'
							&& tolower(buf[6]) == 'r' && tolower(buf[7]) == 'o'
							&& tolower(buf[8]) == 'm' && buf[9] == ':'
							&& buf[10] == '<' && buf[i - 1] == '>') {
						bool validAddress = false;
						for (int j = 12; j < i - 2; j++) {
							if (buf[j] == '@')
								validAddress = true;
						}
						if (validAddress) {
							// Successful MAIL command.
							sender = "";
							for (int j = 11; j < i - 1; j++) {
								sender = sender + buf[j];
							}
							recipients.clear();
							data.clear();
							state = 2;
							do_write(comm_fd, ok, sizeof(ok) - 1);
							if (vflag)
								fprintf(stderr, "[%d] S: %.*s", comm_fd,
										(int) sizeof(ok) - 1, ok);
						} else {
							do_write(comm_fd, paramError,
									sizeof(paramError) - 1);
							if (vflag)
								fprintf(stderr, "[%d] S: %.*s", comm_fd,
										(int) sizeof(paramError) - 1,
										paramError);
						}
					} else {
						do_write(comm_fd, paramError, sizeof(paramError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(paramError) - 1, paramError);
					}
				} // Handle RCPT command.
				else if (tolower(buf[0]) == 'r' && i > 3
						&& tolower(buf[1]) == 'c' && tolower(buf[2]) == 'p'
						&& tolower(buf[3]) == 't'
						&& (i == 4 || buf[4] == ' ')) {
					if (state != 2 && state != 3) {
						do_write(comm_fd, stateError, sizeof(stateError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(stateError) - 1, stateError);
					} else if (i > 12 && tolower(buf[5]) == 't'
							&& tolower(buf[6]) == 'o' && buf[7] == ':'
							&& buf[8] == '<' && buf[i - 1] == '>') {
						bool validAddress = false;
						for (int j = 10; j < i - 2; j++) {
							if (buf[j] == '@')
								validAddress = true;
						}
						if (validAddress) {
							checkIndex = 0;
							bool validDomain = true;
							for (int j = i - 11; j < i - 1; j++) {
								if (tolower(buf[j]) != penncloud[checkIndex])
									validDomain = false;
								checkIndex++;
							}
							if (validDomain) {
								// Successful RCPT command.
								recipient = "";
								for (int j = 9; j < i - 11; j++) {
									recipient = recipient + buf[j];
								}
								resp_tuple resp = getKVS(recipient, recipient,
										"mailbox");
								int respStatus = kvsResponseStatusCode(resp);
								if (respStatus == 0) {
									recipients.push_back(recipient);
									state = 3;
									do_write(comm_fd, ok, sizeof(ok) - 1);
									if (vflag)
										fprintf(stderr, "[%d] S: %.*s", comm_fd,
												(int) sizeof(ok) - 1, ok);
								} else {
									do_write(comm_fd, noUserError,
											sizeof(noUserError) - 1);
									if (vflag)
										fprintf(stderr, "[%d] S: %.*s", comm_fd,
												(int) sizeof(noUserError) - 1,
												noUserError);
								}
							} else {
								do_write(comm_fd, notLocalError,
										sizeof(notLocalError) - 1);
								if (vflag)
									fprintf(stderr, "[%d] S: %.*s", comm_fd,
											(int) sizeof(notLocalError) - 1,
											notLocalError);
							}
						} else {
							do_write(comm_fd, paramError,
									sizeof(paramError) - 1);
							if (vflag)
								fprintf(stderr, "[%d] S: %.*s", comm_fd,
										(int) sizeof(paramError) - 1,
										paramError);
						}
					} else {
						do_write(comm_fd, paramError, sizeof(paramError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(paramError) - 1, paramError);
					}
				} // Handle DATA command.
				else if (tolower(buf[0]) == 'd' && i > 3
						&& tolower(buf[1]) == 'a' && tolower(buf[2]) == 't'
						&& tolower(buf[3]) == 'a' && i == 4) {
					if (state != 3) {
						do_write(comm_fd, stateError, sizeof(stateError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(stateError) - 1, stateError);
					} else {
						// Successful DATA command.
						do_write(comm_fd, intermediateReply,
								sizeof(intermediateReply) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(intermediateReply) - 1,
									intermediateReply);
						index = 0;
						for (int j = i + 2; j < len; j++) {
							buf[index] = buf[j];
							index++;
						}
						len = index;
						if (len > 0)
							checked = false;
						bool dataComplete = false;
						bool seenAlready = false;
						bool isPeriodEnd = false;
						// Read text of email.
						while (!dataComplete) {
							if (checked)
								rcvd = read(comm_fd, &buf[len], 1000 - len);
							else
								rcvd = 0;
							len += rcvd;
							if (checked)
								start = len - rcvd;
							else
								start = 0;
							checked = true;
							for (int j = start; j < len - 1; j++) {
								if (buf[j] == '\r' && buf[j + 1] == '\n') {
									dataLine = "";
									for (int k = start; k < j + 2; k++) {
										dataLine = dataLine + buf[k];
									}
									index = 0;
									if (j < len - 3 && buf[j + 2] == '.'
											&& buf[j + 3] == '\r'
											&& buf[j + 4] == '\n') {
										dataComplete = true;
										for (int k = j + 5; k < len; k++) {
											buf[index] = buf[k];
											index++;
										}
										alreadySet = true;
									} else if (j == start + 1
											&& buf[start] == '.'
											&& seenAlready) {
										dataComplete = true;
										isPeriodEnd = true;
										for (int k = j + 2; k < len; k++) {
											buf[index] = buf[k];
											index++;
										}
										alreadySet = true;
									} else {
										for (int k = j + 2; k < len; k++) {
											buf[index] = buf[k];
											index++;
										}
									}
									if (!isPeriodEnd)
										data.push_back(dataLine);
									len = index;
									if (len > 0)
										checked = false;
									seenAlready = true;
									break;
								}
							}
						}
						time_t rawtime;
						struct tm *timeinfo;
						time(&rawtime);
						timeinfo = localtime(&rawtime);
						std::string temp = "From <" + sender + "> "
								+ asctime(timeinfo) + "\n";
						temp[temp.length() - 2] = '\r';
						// Write email to all recipient mailboxes.
						for (std::size_t a = 0; a < recipients.size(); a++) {
							for (std::size_t b = 0; b < data.size(); b++) {
								temp += data[b];
							}
							resp_tuple resp = getKVS(recipients[a],
									recipients[a], "mailbox");
							int respStatus = kvsResponseStatusCode(resp);
							std::string current = kvsResponseMsg(resp);
							std::string final = temp;
							final += current;
							resp_tuple resp2 = cputKVS(recipients[a],
									recipients[a], "mailbox", current, final);
							int respStatus2 = kvsResponseStatusCode(resp2);
							while (respStatus2 != 0) {
								resp = getKVS(recipients[a], recipients[a],
										"mailbox");
								respStatus = kvsResponseStatusCode(resp);
								current = kvsResponseMsg(resp);
								final = temp;
								final += current;
								resp2 = cputKVS(recipients[a], recipients[a],
										"mailbox", current, final);
								respStatus2 = kvsResponseStatusCode(resp2);
							}
						}
						state = 1;
						do_write(comm_fd, ok, sizeof(ok) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(ok) - 1, ok);
					}
				} // Handle RSET command.
				else if (tolower(buf[0]) == 'r' && i > 3
						&& tolower(buf[1]) == 's' && tolower(buf[2]) == 'e'
						&& tolower(buf[3]) == 't' && i == 4) {
					if (state == 0) {
						do_write(comm_fd, stateError, sizeof(stateError) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(stateError) - 1, stateError);
					} else {
						// Successful RSET command.
						sender = "";
						recipients.clear();
						data.clear();
						state = 1;
						do_write(comm_fd, ok, sizeof(ok) - 1);
						if (vflag)
							fprintf(stderr, "[%d] S: %.*s", comm_fd,
									(int) sizeof(ok) - 1, ok);
					}
				} // Handle NOOP command.
				else if (tolower(buf[0]) == 'n' && i > 3
						&& tolower(buf[1]) == 'o' && tolower(buf[2]) == 'o'
						&& tolower(buf[3]) == 'p' && i == 4) {
					do_write(comm_fd, ok, sizeof(ok) - 1);
					if (vflag)
						fprintf(stderr, "[%d] S: %.*s", comm_fd,
								(int) sizeof(ok) - 1, ok);
				} // Handle QUIT command.
				else if (tolower(buf[0]) == 'q' && i > 3
						&& tolower(buf[1]) == 'u' && tolower(buf[2]) == 'i'
						&& tolower(buf[3]) == 't' && i == 4) {
					do_write(comm_fd, quit, sizeof(quit) - 1);
					if (vflag)
						fprintf(stderr, "[%d] S: %.*s", comm_fd,
								(int) sizeof(quit) - 1, quit);
					close(comm_fd);
					if (vflag)
						fprintf(stderr, "[%d] Connection closed\n", comm_fd);
					pthread_detach (pthread_self());pthread_exit
					(NULL);
				} // Handle unknown command.
				else {
					do_write(comm_fd, commandError, sizeof(commandError) - 1);
					if (vflag)
						fprintf(stderr, "[%d] S: %.*s", comm_fd,
								(int) sizeof(commandError) - 1, commandError);
				}
				if (!alreadySet) {
					index = 0;
					for (int j = i + 2; j < len; j++) {
						buf[index] = buf[j];
						index++;
					}
					len = index;
					if (len > 0)
						checked = false;
				}
				break;
			}
		}
	}
	do_write(comm_fd, shutDown, sizeof(shutDown) - 1);
	close(comm_fd);
	if (vflag)
		fprintf(stderr, "[%d] Connection closed\n", comm_fd);
	decrementThreadCounter(SMTP_THREAD);
	pthread_detach (pthread_self());pthread_exit
	(NULL);
}

int create_thread(int socket_fd, bool http) {
	struct sockaddr_in client_addr;
	unsigned int clientaddrlen = sizeof(client_addr);
	int *client_fd = (int*) malloc(sizeof(int));
	*client_fd = accept(socket_fd, (struct sockaddr*) &client_addr,
			&clientaddrlen);
	if (*client_fd <= 0) {
		free(client_fd);
		if (!shut_down) {
			std::cerr << "Accept system call failed \n";
			exit(-1);
		}
		return -1;
	} else {
		pthread_t pthread_id;
		if (http) {
			pthread_create(&pthread_id, NULL, handleClient, client_fd);
		} else {
			pthread_create(&pthread_id, NULL, handle_smtp_connections,
					client_fd);
		}
		if (shut_down) {
			write(*client_fd, error_msg.data(), error_msg.size());
			close(*client_fd);
			free(client_fd);
		} else {
			if (verbose)
				std::cerr << "[" << *client_fd << "] New connection\n";
			pthread_mutex_lock(&fd_mutex);
			fd.insert(client_fd);
			pthread_mutex_unlock(&fd_mutex);
		}
	}
	return 0;
}

int initialize_socket(int port_no, bool datagram) {
	int socket_fd;
	if (datagram) {
		socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	} else {
		socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	}
	if (socket_fd < 0) {
		std::cerr << "Socket failed to initialize for port no " << port_no
				<< "\n";
		return -1;
	} else if (verbose) {
		std::cerr << "Socket initialized successfully!\n";
	}
	int true_opt = 1;
	if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &true_opt, sizeof(int))
			< 0) {
		if (verbose)
			std::cerr << "Setsockopt failed\n";
	}
	pthread_mutex_lock(&fd_mutex);
	fd.insert(&socket_fd);
	pthread_mutex_unlock(&fd_mutex);

	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));

	/* Assign port and ip address */
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(port_no);
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);

	/* Bind socket */
	if (bind(socket_fd, (struct sockaddr*) &servaddr, sizeof(servaddr)) != 0) {
		std::cerr << "Sockets couldn't bind for port " << port_no << "\n";
		exit(-1);
	} else if (verbose) {
		std::cerr << "Sockets Successfully binded\n";
	}

	/* Start listening */
	if (!datagram) {
		if (listen(socket_fd, 20) != 0) {
			std::cerr << "Listening failed!\n";
			exit(-1);
		} else if (verbose) {
			std::cerr << "Successfully started listening!\n";
		}
	}

	/* Add a timeout to socket to handle ctrl c periodically */
	struct timeval timeout;
	timeout.tv_sec = 3;
	timeout.tv_usec = 0;
	if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (char*) &timeout,
			sizeof(timeout)) < 0)
		log("setsockopt failed\n");

	log("Socket_fd: " + std::to_string(socket_fd));
	return socket_fd;
}

int main(int argc, char *argv[]) {
	/* Set signal handler */
//signal(SIGINT, sigint_handler);
	/* Initialize mutexes */
	if (pthread_mutex_init(&fd_mutex, NULL) != 0)
		log("Couldn't initialize mutex for fd set");
	if (pthread_mutex_init(&modify_server_state, NULL) != 0)
		log("Couldn't initialize mutex for modify_server_state");
	if (pthread_mutex_init(&crashing, NULL) != 0)
		log("Couldn't initialize mutex for crashing");
	if (pthread_mutex_init(&access_state_map, NULL) != 0)
		log("Couldn't initialize mutex for access_state_map");

	/* Parse command line args */
	int c, port_no = 10000, smtp_port_no = 35000, internal_port_no = 40000;
	std::string list_of_frontend = "";
	while ((c = getopt(argc, argv, ":vlp:q:r:k:m:s:c:i:a")) != -1) {
		switch (c) {
		case 'v':
			verbose = true;
			vflag = 1;
			break;
		case 'l':
			load_balancer = true;
			break;
		case 'p':
			port_no = atoi(optarg);
			if (port_no == 0) {
				std::cerr
						<< "Port number is 0 or '-p' is followed by non integer! Using default\n";
				port_no = 10000;
			}
			break;
		case 'k':
			kvMaster_addr = trim(std::string(optarg));
			break;
		case 'q':
			smtp_port_no = atoi(optarg);
			if (smtp_port_no == 0) {
				std::cerr
						<< "Port number is 0 or '-q' is followed by non integer! Using default\n";
				smtp_port_no = 15000;
			}
			break;
		case 'r':
			internal_port_no = atoi(optarg);
			if (internal_port_no == 0) {
				std::cerr
						<< "Port number is 0 or '-r' is followed by non integer! Using default\n";
				internal_port_no = 20000;
			}
			break;
		case 'm':
			mail_addr = trim(std::string(optarg));
			break;
		case 's':
			storage_addr = trim(std::string(optarg));
			break;
		case 'c':
			list_of_frontend = trim(std::string(optarg));
			break;
		case 'a':
			std::cerr << "TEAM 20\n";
			exit(0);
			break;
		case 'i':
			server_index = atoi(optarg);
			if (server_index == 0) {
				std::cerr
						<< "Port number is 0 or '-n' is followed by non integer! Using default\n";
				server_index = 1;
			}
			break;
		case ':':
			switch (optopt) {
			case 'p':
				std::cerr
						<< "'-p' should be followed by a number! Using port 10000\n";
				break;
			case 'k':
				std::cerr << "'-k' should be followed by an address!\n";
				break;
			case 'q':
				std::cerr
						<< "'-q' should be followed by a number! Using port 10000\n";
				break;
			case 'r':
				std::cerr
						<< "'-r' should be followed by a number! Using port 10000\n";
				break;
			case 'm':
				std::cerr << "'-m' should be followed by an address!\n";
				break;
			case 's':
				std::cerr << "'-s' should be followed by an address!\n";
				break;
			case 'c':
				std::cerr << "'-c' should be followed by a file name!\n";
				break;
			case 'i':
				std::cerr
						<< "'-i' should be followed by a number! Using 1 as default";
				break;
			}
			break;
		}
	}

// Requesting list of all backend servers to build mapping (clusterNum) -> (list of servers)
	buildClusterToBackendServerMapping();

	/* Add the list of all frontend servers to queue for load balancing if this node is load balancer */
	if (load_balancer) {
		server_index = 0;
		log("Successfully initialized load balancer!");
	}

	if (list_of_frontend.length() > 0) {
		FILE *f = fopen(list_of_frontend.c_str(), "r");
		if (f == NULL) {
			std::cerr
					<< "Provide a valid list of frontend servers to the load balancer!"
					<< "File " << list_of_frontend
					<< " not found or couldn't be opened!\n";
			exit(-1);
		}
		char buffer[300];
		fgets(buffer, 300, f);
		auto load_balancer_address = split(
				(split(trim(std::string(buffer)), ",")).at(1), ":");
		load_balancer_addr.sin_family = AF_INET;
		load_balancer_addr.sin_port = htons(
				std::stoi(load_balancer_address[1]));
		load_balancer_addr.sin_addr.s_addr = inet_addr(
				load_balancer_address[0].data());
		frontend_internal_list.push_back(load_balancer_addr);
		int i = 1;
		while (fgets(buffer, 300, f)) {
			auto tokens = split(trim(std::string(buffer)), ",");
			struct server_state dummy_state;
			frontend_server_list.push_back(trim(tokens[0]));
			frontend_state_map[trim(tokens[0])] = dummy_state;
			auto internal_server_address = split(trim(tokens[2]), ":");
			sockaddr_in internal_server;
			internal_server.sin_family = AF_INET;
			internal_server.sin_port = htons(
					std::stoi(internal_server_address[1]));
			internal_server.sin_addr.s_addr = inet_addr(
					internal_server_address[0].data());
			frontend_internal_list.push_back(internal_server);
			if (i == server_index)
				this_server_state.http_address = trim(tokens[0]);
			i++;
		}
		fclose(f);

// Start heartbeat thread if not load balancer
		if (!load_balancer) {
			log("Starting hearbeat");
			pthread_t pthread_id;
			pthread_create(&pthread_id, NULL, heartbeat, NULL);
		}
	}

	/* Initialize socket: http and smtp are tcp, internal is UDP */
	int socket_fd, smtp_socket_fd;
	if ((socket_fd = initialize_socket(port_no, false)) < 0)
		exit(-1);
	if ((smtp_socket_fd = initialize_socket(smtp_port_no, false)) < 0)
		exit(-1);
	if ((internal_socket_fd = initialize_socket(internal_port_no, true)) < 0)
		exit(-1);

	sigset_t empty_set;
	sigemptyset(&empty_set);

//set up admin account (preferably in only one place: load balancer) TODO
//putKVS("admin", "password", "505");

	while (!shut_down && !amICrashing()) {
		/* Initialize read set for select and call select */
		fd_set read_set;
		FD_ZERO(&read_set);
		FD_SET(socket_fd, &read_set);
		FD_SET(smtp_socket_fd, &read_set);
		FD_SET(internal_socket_fd, &read_set);
		int nfds = std::max(internal_socket_fd,
				std::max(smtp_socket_fd, socket_fd)) + 1;
		int r = 0;
		while (r <= 0 && !shut_down) {
			r = pselect(nfds, &read_set, NULL, NULL, NULL, &empty_set);
		}
		if (r <= 0 || shut_down)
			continue;

		if (FD_ISSET(socket_fd, &read_set)) {
			if (create_thread(socket_fd, true) < 0)
				break;
		} else if (FD_ISSET(smtp_socket_fd, &read_set)) {
			if (create_thread(smtp_socket_fd, false) < 0)
				break;
		} else if (FD_ISSET(internal_socket_fd, &read_set)) {
			// TODO handle internal server message
			pthread_t pthread_id;
			pthread_create(&pthread_id, NULL, handleInternalConnection, NULL);
		}
	}

	/* Close all connections */
	pthread_mutex_lock(&fd_mutex);
	fd.erase(&socket_fd);
	for (int *f : fd) {
		write(*f, error_msg.data(), error_msg.size());
		if (verbose)
			std::cerr << "[" << *f << "] S: " << error_msg;
		close(*f);
		if (verbose)
			std::cerr << "[" << *f << "] Connection closed\n";
		free(f);
	}
	close(socket_fd);
	pthread_mutex_unlock(&fd_mutex);
	return 0;
}
