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

/********************** Internal message and chat stuff ******************/
using server_addr_tuple = std::tuple<int, bool, std::string, std::string>;
std::string INTERNAL_THREAD = "i", SMTP_THREAD = "s", HTTP_THREAD = "h";
pthread_mutex_t modify_server_state, crashing, access_state_map,
		access_fifo_seq_num;
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
	INFO_REQ = 0, INFO_RESP = 1, STOP = 2, RESUME = 3, ACK = 4, CHAT = 5
};
struct internal_message {
	int sequence_number;
	message_type type;
	std::string sender, content, group, group_owner;
	struct server_state state;
} internal_message;

struct internal_message_comparator {
	bool operator()(const struct internal_message &a,
			const struct internal_message &b) const {
		return a.sequence_number < b.sequence_number;
	}
};

std::vector<sockaddr_in> frontend_internal_list;

/********************* chat stuff **********************************/
std::map<std::string, std::set<std::string>> group_to_clients;
std::map<std::string, std::string> client_to_group;
volatile int fifo_seq = 0;
std::map<std::string, int> server_sequence_nums;
std::map<std::string, std::deque<struct internal_message>> fifo_holdbackQ;
std::map<std::string, std::map<std::string, std::string>> deliverable_messages;

struct admin_console_cache {
	bool initialized = false;
	time_t last_modified = time(NULL);
	std::string last_modified_by, last_accessed_for;
	std::vector<std::tuple<std::string, std::deque<std::string>>> rowToAllItsCols;
	std::vector<std::string> activeBackendServersList;
	std::set<std::string> stopped_servers;
	std::deque<server_addr_tuple> activeBackendServers, allBackendServers;
	std::map<std::string, std::string> frontendToAdminComm;
} admin_console_cache;
struct admin_console_cache my_admin_console_cache;

// maps session IDs to a specific backend server as specified by whereKVS
using server_tuple = std::tuple<std::string, std::string>;
std::map<std::string, server_tuple> rowSessionIdToServerMap;
std::map<std::string, int> rowSessionIdToServerIdx;
std::map<std::string, int> rowToClusterNum;
std::map<int, std::deque<server_tuple>> clusterToServerListMap;

using resp_tuple = std::tuple<int, std::string>;

std::deque<std::string> paxosServers;
std::map<std::string, std::string> paxosServersHeartbeatMap;

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
	std::string targetServer = std::get < 0 > (serverInfo);
	std::string targetServerHeartbeatIP = std::get < 1 > (serverInfo);
	log(
			"Checking heartbeat for " + targetServer + " at heartbeat address: "
					+ targetServerHeartbeatIP);
	int heartbeatPortNo = getPortNoFromString(targetServerHeartbeatIP);
	std::string heartbeatAddress = getAddrFromString(targetServerHeartbeatIP);
	rpc::client kvsHeartbeatRPCClient(heartbeatAddress, heartbeatPortNo);
	kvsHeartbeatRPCClient.set_timeout(2000); // 2000 milliseconds
	try {
		bool isAlive = kvsHeartbeatRPCClient.call("heartbeat").as<bool>();
		log("Heartbeat for " + targetServer + " returned true!");
		return isAlive;
	} catch (rpc::timeout &t) {
		log(
				"Heartbeat for " + targetServer
						+ " failed to return. Node is dead.");
		return false;
	}
	return false;
}

std::string whereKVS(std::string session_id, std::string row) {
	std::string rowSessionId = row + session_id;
	int masterPortNo = getPortNoFromString(kvMaster_addr);
	std::string masterServAddress = getAddrFromString(kvMaster_addr);
	rpc::client masterNodeRPCClient(masterServAddress, masterPortNo);
	try {
		if (rowToClusterNum.count(row) <= 0) {
			log(
					"MASTERNODE WHERE: row (" + row + ") for session ("
							+ session_id + ")");
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
		log(
				"whereKVS: Servers to choose from in cluster "
						+ std::to_string(clusterNum) + " are:");
		for (server_tuple serverInfo : serverList) {
			log("--" + std::get < 0 > (serverInfo));
		}
		int serverIdx = 0;
		if (rowSessionIdToServerIdx.count(rowSessionId) <= 0) {
			// Randomly generate index to pick server in cluster
			serverIdx = rand() % serverList.size();
			log(
					"whereKVS: New session (" + session_id + ") for row (" + row
							+ ")! Randomly picking server "
							+ std::to_string(serverIdx));
		} else {
			// Incrementing serverIdx to next server in list
			serverIdx = rowSessionIdToServerIdx[rowSessionId];
			serverIdx++;
			log(
					"whereKVS: Existing session (" + session_id + ") for row "
							+ row + "! Incrementing server to "
							+ std::to_string(serverIdx));
		}
		serverIdx = serverIdx % serverList.size();
		rowSessionIdToServerIdx[rowSessionId] = serverIdx;

		server_tuple chosenServerAddrs = serverList[serverIdx];
		rowSessionIdToServerMap[rowSessionId] = chosenServerAddrs;
		log(
				"whereKVS: session_id (" + session_id + ") given server "
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

resp_tuple kvsFunc(std::string kvsFuncType, std::string session_id,
		std::string row, std::string column, std::string value,
		std::string old_value) {
	std::string rowSessionId = row + session_id;
	if (rowSessionIdToServerMap.count(rowSessionId) <= 0) {
		log(
				kvsFuncType + ": No server for session (" + session_id
						+ "). Calling whereKVS.");
		std::string newlyChosenServerAddr = whereKVS(session_id, row);
		log(
				kvsFuncType + ": Server " + newlyChosenServerAddr
						+ " chosen for session (" + session_id + ").");
	}
	uint64_t timeout = 5000; // 5000 milliseconds
	bool nodeIsAlive = true;
	int origServerIdx = rowSessionIdToServerIdx[rowSessionId];
	int currServerIdx = -2;
	// Continue trying RPC call until you've tried all backend servers
	while (origServerIdx != currServerIdx) {
		server_tuple serverInfo = rowSessionIdToServerMap[rowSessionId];
		std::string targetServer = std::get < 0 > (serverInfo);
		if (!checkIfNodeIsAlive(serverInfo)) {
			// Resetting timeout for new server
			timeout = 2500; // 2500 milliseconds
			std::string newlyChosenServerAddr = whereKVS(session_id, row);
			currServerIdx = rowSessionIdToServerIdx[rowSessionId];
			log(
					"Node " + targetServer + " is dead! Trying new node "
							+ newlyChosenServerAddr);
			continue;
		}
		int serverPortNo = getPortNoFromString(targetServer);
		std::string servAddress = getAddrFromString(targetServer);
		rpc::client kvsRPCClient(servAddress, serverPortNo);
		resp_tuple resp;
		kvsRPCClient.set_timeout(timeout);
		try {
			if (kvsFuncType.compare("putKVS") == 0) {
				log("KVS PUT VAL LENGTH:" + std::to_string(value.length()));
				log(
						"KVS PUT with kvServer " + targetServer + ": " + row
								+ ", " + column + ", " + value);
				resp = kvsRPCClient.call("put", row, column, value).as<
						resp_tuple>();
			} else if (kvsFuncType.compare("cputKVS") == 0) {
				log(
						"KVS CPUT with kvServer " + targetServer + ": " + row
								+ ", " + column + ", " + old_value + ", "
								+ value);
				resp =
						kvsRPCClient.call("cput", row, column, old_value, value).as<
								resp_tuple>();
			} else if (kvsFuncType.compare("deleteKVS") == 0) {
				log(
						"KVS DELETE with kvServer " + targetServer + ": " + row
								+ ", " + column);
				resp = kvsRPCClient.call("del", row, column).as<resp_tuple>();
			} else if (kvsFuncType.compare("getKVS") == 0) {
				log(
						"KVS GET with kvServer " + targetServer + ": " + row
								+ ", " + column);
				resp = kvsRPCClient.call("get", row, column).as<resp_tuple>();
			}
			log(
					kvsFuncType + " Response Status: "
							+ std::to_string(kvsResponseStatusCode(resp)));
			log(kvsFuncType + " Response Value: " + kvsResponseMsg(resp));
			log(
					kvsFuncType + " Response Value Length: "
							+ std::to_string(kvsResponseMsg(resp).length()));
			return resp;
		} catch (rpc::timeout &t) {
			log(
					kvsFuncType + " for (" + session_id + ", " + row
							+ ") timed out!");
			// connect to heartbeat thread of backend server and check if it's alive w/ shorter timeout
			bool isAlive = checkIfNodeIsAlive(serverInfo);
			if (isAlive) {
				// Double timeout and try again if node is still alive
				timeout *= 2;
				log(
						"Node " + targetServer
								+ " is still alive! Doubling timeout to "
								+ std::to_string(timeout)
								+ " and trying again.");
			} else {
				// Resetting timeout for new server
				timeout = 5000; // 5000 milliseconds
				std::string newlyChosenServerAddr = whereKVS(session_id, row);
				currServerIdx = rowSessionIdToServerIdx[rowSessionId];
				log(
						"Node " + targetServer + " is dead! Trying new node "
								+ newlyChosenServerAddr);
			}
		} catch (rpc::rpc_error &e) {
			/*
			 std::cout << std::endl << e.what() << std::endl;
			 std::cout << "in function " << e.get_function_name() << ": ";
			 using err_t = std::tuple<std::string, std::string>;
			 auto err = e.get_error().as<err_t>();
			 */
			log("UNHANDLED ERROR IN " + kvsFuncType + " TRY CATCH"); // TODO
		}
	}
	log(
			kvsFuncType + " for (" + session_id + ", " + row
					+ "): All nodes in cluster down! ERROR.");
	return std::make_tuple(-2, "All nodes in cluster down!");
}

resp_tuple getKVS(std::string session_id, std::string row, std::string column) {
	return kvsFunc("getKVS", session_id, row, column, "", "");
}

resp_tuple putKVS(std::string session_id, std::string row, std::string column,
		std::string value) {
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

std::deque<std::string> getAllRowsKVS(std::string addr) {
	int portNo = getPortNoFromString(addr);
	std::string servAddress = getAddrFromString(kvMaster_addr);
	rpc::client nodeRPCClient(servAddress, portNo);
	log("ge all rows port no: " + std::to_string(portNo));
	std::deque < std::string > resp;
	try {
		log("NODE getAllRowsKVS");
		resp = nodeRPCClient.call("getAllRows").as<std::deque<std::string>>();
		return resp;
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getAllRowsKVS TRY CATCH"); // TODO
	}
	log("Error in getAllRowsKVS");
	return resp;
}

std::deque<std::string> getAllColsForRowKVS(std::string addr, std::string row) {
	int portNo = getPortNoFromString(addr);
	std::string servAddress = getAddrFromString(kvMaster_addr);
	rpc::client nodeRPCClient(servAddress, portNo);
	std::deque < std::string > resp;
	try {
		log("NODE getAllColsForRowKVS");
		resp = nodeRPCClient.call("getAllColsForRow", row).as<
				std::deque<std::string>>();
		return resp;
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getAllColsForRowKVS TRY CATCH"); // TODO
	}
	log("Error in getAllColsForRowKVS");
	return resp;
}

std::tuple<int, int, std::string> getFirstNBytesKVS(std::string addr,
		std::string row, std::string col, int n) {
	int portNo = getPortNoFromString(addr);
	std::string servAddress = getAddrFromString(kvMaster_addr);
	rpc::client nodeRPCClient(servAddress, portNo);
	std::tuple<int, int, std::string> resp;
	try {
		log("NODE getFirstNBytesKVS");
		resp = nodeRPCClient.call("getFirstNBytes", row, col, n).as<
				std::tuple<int, int, std::string>>();
		return resp;
	} catch (rpc::rpc_error &e) {
		std::cout << std::endl << e.what() << std::endl;
		std::cout << "in function " << e.get_function_name() << ": ";
		using err_t = std::tuple<std::string, std::string>;
		auto err = e.get_error().as<err_t>();
		log("UNHANDLED ERROR IN getFirstNBytesKVS TRY CATCH"); // TODO
	}
	log("Error in getFirstNBytesKVS");
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
	} else if (message.type == CHAT) {
		ret += std::to_string(message.sequence_number) + ",";
		ret += message.sender + ",";
		ret += message.group_owner + ",";
		ret += message.group + "__chatboundary__";
		ret += message.content + ",";
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
	} else if (ret.type == CHAT) {
		ret.sequence_number = stoi(message.substr(0, message.find(",")));
		message.erase(0, message.find(",") + 1);
		ret.sender = message.substr(0, message.find(","));
		message.erase(0, message.find(",") + 1);
		ret.group_owner = message.substr(0, message.find(","));
		message.erase(0, message.find(",") + 1);
		ret.group = message.substr(0, message.find("__chatboundary__"));
		message.erase(0, message.find("__chatboundary__") + 16);
		ret.content = message;
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
//			log("Invalid argument: " + std::string(ia.what()));
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

void handleHoldbackQ(struct internal_message &message, sockaddr_in &src) {
	std::string src_server = getAddressFromSockaddr(src);

	log(
			"GOT CHAT MESSAGE FROM : " + src_server + " WITH CONTENT: "
					+ message.content);

	if (server_sequence_nums.find(src_server) == server_sequence_nums.end())
		server_sequence_nums[src_server] = 0;

	/* Add message to the holdback q */
	fifo_holdbackQ[src_server].push_back(message);
	std::push_heap(fifo_holdbackQ[src_server].begin(),
			fifo_holdbackQ[src_server].end(), internal_message_comparator());

//	log("after pushback");
//	log_heap(fifo_holdbackQ[broadcast_server]);

	/* Obtain the seq number for message and seq number expected,  and
	 * deliver while they match */
	int expected_seq = -1;
	int actual_seq = -1;
	while (expected_seq == actual_seq && fifo_holdbackQ[src_server].size() > 0) {
		expected_seq = server_sequence_nums[src_server] + 1;
		actual_seq = -(fifo_holdbackQ[src_server].front().sequence_number);
		log(
				"Expected " + std::to_string(expected_seq) + " for "
						+ src_server + " but actual was "
						+ std::to_string(actual_seq) + "\n");
		if (expected_seq == actual_seq) {
			struct internal_message deliverable_message =
					fifo_holdbackQ[src_server].front();
			std::string my_message = "<li>" + escape(deliverable_message.sender)
					+ ": " + escape(deliverable_message.content) + "</li>";
			resp_tuple raw_chatroom = getKVS(deliverable_message.group_owner,
					deliverable_message.group_owner, deliverable_message.group);
			std::string old_chat = kvsResponseMsg(raw_chatroom);
			std::string new_chat = old_chat + my_message;

			for (std::string member : group_to_clients[deliverable_message.group]) {
				deliverable_messages[member][deliverable_message.group] =
						new_chat;
				log("NEW CHAT IS " + new_chat);
			}
			int resp_code = -1, timeout_count = 0;
			while (resp_code != 0 && (timeout_count++) < 10) {
				raw_chatroom = getKVS(deliverable_message.group_owner,
						deliverable_message.group_owner,
						deliverable_message.group);
				old_chat = kvsResponseMsg(raw_chatroom);
				new_chat = old_chat + my_message;
				if (old_chat.find(my_message) != std::string::npos)
					break;
				resp_tuple ret = cputKVS(deliverable_message.group_owner,
						deliverable_message.group_owner,
						deliverable_message.group, old_chat, new_chat);
				resp_code = kvsResponseStatusCode(ret);
				log("Try: " + std::to_string(timeout_count));
				log("resp_code : " + std::to_string(resp_code));
				log("Old chat: " + old_chat);
				log("My message: " + my_message);
				log("New chat: " + new_chat);
			}

			std::pop_heap(fifo_holdbackQ[src_server].begin(),
					fifo_holdbackQ[src_server].end(),
					internal_message_comparator());
			fifo_holdbackQ[src_server].pop_back();
			server_sequence_nums[src_server] += 1;
//			log("after pop back");
//			log_heap(fifo_holdbackQ[broadcast_server]);
		}
	}
//	log("after final pop back");
//	log_heap(fifo_holdbackQ[broadcast_server]);
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
		case CHAT:
			if (!load_balancer)
				handleHoldbackQ(message, src);
		}
	} catch (const std::invalid_argument &ia) {
		//log("Invalid argument: " + std::string(ia.what()));
	}

	decrementThreadCounter(INTERNAL_THREAD);
	pthread_detach (pthread_self());pthread_exit
	(NULL);
}

void chatMulticast(std::string sender, std::string message,
		std::string group_hash, std::string owner) {
	struct internal_message msg;
	msg.content = message;
	msg.sender = sender;
	msg.group = group_hash;
	msg.group_owner = owner;
	pthread_mutex_lock(&access_fifo_seq_num);
	msg.sequence_number = -(++fifo_seq);
	pthread_mutex_unlock(&access_fifo_seq_num);
	msg.type = CHAT;

	std::string message_to_send = internalMessageToString(msg);
	log("MULTICASTING MESSAGE: " + message_to_send);
	for (sockaddr_in dest : frontend_internal_list) {
		send_message(message_to_send, dest, false);
	}
}

/***************************** Admin stuff ****************************/

bool isAdminCacheValidFor(std::string modified_by, std::string accessed_for,
		int timeout) {
	if ((time(NULL) - my_admin_console_cache.last_modified > timeout)
			|| (modified_by.compare(my_admin_console_cache.last_modified_by)
					!= 0)
			|| (accessed_for.compare(my_admin_console_cache.last_accessed_for)
					!= 0))
		return false;
	return true;
}

void populateAdminCache() {
	my_admin_console_cache.allBackendServers = getAllNodesKVS();
	for (auto node : my_admin_console_cache.allBackendServers) {
		my_admin_console_cache.frontendToAdminComm[trim(std::get < 2 > (node))] =
				trim(std::get < 3 > (node));
		log(
				"Frontend comm: " + trim(std::get < 2 > (node)) + " mapped to "
						+ trim(std::get < 3 > (node)));
	}
	my_admin_console_cache.activeBackendServers = getActiveNodesKVS();
	for (auto node : my_admin_console_cache.activeBackendServers) {
		my_admin_console_cache.activeBackendServersList.push_back(
				std::get < 2 > (node));
		my_admin_console_cache.frontendToAdminComm[trim(std::get < 2 > (node))] =
				trim(std::get < 3 > (node));
		log(
				"Active Frontend comm: " + trim(std::get < 2 > (node))
						+ " mapped to " + trim(std::get < 3 > (node)));
	}
	my_admin_console_cache.initialized = true;
}

void registerCacheAccess(std::string modified_by, std::string accessed_for) {
	my_admin_console_cache.last_modified = time(NULL);
	my_admin_console_cache.last_modified_by = modified_by;
	my_admin_console_cache.last_accessed_for = accessed_for;
}

void getMoreInfoFor(std::string target) {
	std::string adminCommAddr =
			my_admin_console_cache.frontendToAdminComm[target];
	log("Admin comm for " + target + " is " + adminCommAddr);
	auto allRows = getAllRowsKVS(target); //TODO: ask amit
	for (std::string row : allRows) {
		auto cols = getAllColsForRowKVS(target, row);
		if (cols.size() < 1)
			continue;
		my_admin_console_cache.rowToAllItsCols.push_back(
				std::tuple<std::string, std::deque<std::string>>(row, cols));
	}
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
						return -1; // file with same name already exists in directory
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
			return -2; // unknown error please try again later
		}
	}
	return -2; // unknown error please try again later
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
				return -3; // file to be renamed not found
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
						return -1; // file with name already exists
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
			return -2; // unknown
		}
	}
	return -2; // unknown
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
		return -3;
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
			return -2; // unknown
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
		return result - 10;

	if (hash == itemToMove)
		return -4;

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
	std::string link =
			"<form method=\"POST\" action=\"/files/" + fileHash
					+ "\"><button class=\"sidebar-link\" type = \"submit\"><i class=\"fas fa-folder-minus\"></i>&nbsp;&nbsp;Parent Directory</button></form>";
	return link;
}

std::string getFileLink(std::string fileName, std::string fileHash,
		std::string containingDirectory) {
	fileName = decodeURIComponent(fileName);
	fileName = decodeURIComponent(fileName);
	size_t index = 0;
	while (true) {
		index = fileName.find("%D", index);
		if (index == std::string::npos)
			break;
		fileName.replace(index, 2, "\r");
		index += 2;
	}
	std::string link;
	if (fileHash.substr(0, 3).compare("ss0") == 0) {
// Directory
		link =
				"<div class=\"file-item\"><div class=\"file-first-row\">"
						"<i class=\"item-button fas fa-folder\" style=\"cursor: default;\">&nbsp;&nbsp;</i><a title=\"Open "
						+ escape(fileName) + "\" href=/files/" + fileHash + ">"
						+ escape(fileName) + "</a>";
	} else {
// File
		link =
				"<div class=\"file-item\"><div class=\"file-first-row\">"
						"<i class=\"item-button fas fa-file\" style=\"cursor: default;\">&nbsp;&nbsp;</i><a title=\"Download "
						+ escape(fileName) + "\" download=\"" + escape(fileName)
						+ "\" href=/files/" + fileHash + ">" + escape(fileName)
						+ "</a>";
	}
	link +=
			"<script>function encodeName() {for (var i = 0; i < document.getElementsByName(\"newName\").length; i++) {document.getElementsByName(\"newName\")[i].value = encodeURIComponent(document.getElementsByName(\"newName\")[i].value);} return true;}</script>"
					"<form accept-charset=\"utf-8\" onsubmit=\"return encodeName();\" action=\"/files/"
					+ containingDirectory
					+ "\" method=\"post\">"
							"</div><div class=\"file-second-row\"><div style=\"display: flex; flex-direction: row;\">"
							"<input type=\"hidden\" name=\"itemToRename\" value=\""
					+ fileHash
					+ "\" />"
							"<label for=\"newName\"></label><input placeholder=\"New Name...\" required type=\"text\" name=\"newName\"/>"
							"<input type=\"submit\" name=\"submit\" value=\"Rename\" />"
							"</div>"
							"</form>"
							"<script>function encode() {for (var i = 0; i < document.getElementsByName(\"newName\").length; i++) {document.getElementsByName(\"newLocation\")[i].value = encodeURIComponent(document.getElementsByName(\"newLocation\")[i].value);} return true;}</script>"
							"<form accept-charset=\"utf-8\" onsubmit=\"return encode();\" action=\"/files/"
					+ containingDirectory
					+ "\" method=\"post\">"
							"<div style=\"display: flex; flex-direction: row;\">"
							"<input type=\"hidden\" name=\"itemToMove\" value=\""
					+ fileHash
					+ "\" />"
							"<label for=\"newLocation\"></label><input placeholder=\"New Absolute Path...\" required type=\"text\" name=\"newLocation\"/>"
							"<input type=\"submit\" name=\"submit\" value=\"Move\" />"
							"</div>"
							"</form><form action=\"/files/"
					+ containingDirectory
					+ "\" method=\"post\" style=\"margin: auto;\">"
							"<input type=\"hidden\" name=\"itemToDelete\" value=\""
					+ fileHash
					+ "\" />"
							"<button class=\"item-button\" type = \"submit\"><i title=\"Delete\" class=\"fas fa-trash-alt\"></i></button>"
							"</form></div></div>";
	return link;
}

std::string getFileList(struct http_request req, std::string filepath,
		std::string &parentDirLink) {
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];
	resp_tuple filesResp = getKVS(sessionid, username, filepath);
	int respStatus = kvsResponseStatusCode(filesResp);
	std::string respValue = kvsResponseMsg(filesResp);
	int lineNum = 0;
	if (respStatus == 0) {
		if (respValue.length() == 0) {
			return "<p>This directory is empty.</p>";
		}
		std::string result = "";
		std::deque < std::string > splt = split(respValue, "\n");
		for (std::string line : splt) {
			if (line.length() > 0) {
				std::size_t foundPos = line.find_last_of(",");
				std::string currName = line.substr(0, foundPos);
				std::deque < std::string > lineSplt;
				lineSplt.push_back(currName);
				lineSplt.push_back(line.substr(foundPos + 1));
				if (lineNum == 0) {
					// Parent Directory Line
					if (!(lineSplt[0].compare("ROOT") == 0
							&& lineSplt[1].compare("ROOT") == 0)) {
						parentDirLink = getParentDirLink(lineSplt[1]);
					}
				} else {
					// Child Files or Directories
					result += getFileLink(lineSplt[0], lineSplt[1], filepath);
				}
				lineNum++;
			}
		}
		result += "";
		if (lineNum <= 1) {
			result += "<p>This directory is empty.</p>";
		}
		return result;
	} else {
		return "-1";
	}
}

// Returns a deque of <str, str> tuples where [0] is the dir name, and [1] is the dir hash
std::deque<std::tuple<std::string, std::string>> getFilePath(
		struct http_request req, std::string targetDirHash) {
	log("Getting filepath for dir hash: " + targetDirHash);
	std::string username = req.cookies["username"];
	std::string sessionid = req.cookies["sessionid"];

	std::string currDirHash = targetDirHash;
	std::string dirContents = kvsResponseMsg(
			getKVS(sessionid, username, currDirHash));
	std::deque<std::tuple<std::string, std::string>> result;
	bool notAtRoot = true;
	while (notAtRoot) {
		std::string parentDirHash = dirContents.substr(0,
				dirContents.find("\n"));
		parentDirHash = parentDirHash.substr(parentDirHash.find(",") + 1);
		resp_tuple resp = getKVS(sessionid, username, parentDirHash);
		std::string parentDirContents = kvsResponseMsg(resp);
		if (kvsResponseStatusCode(resp) != 0) {
			break;
		}
		std::deque < std::string > parentDirContentsSplt = split(
				parentDirContents, "\n");
		for (std::string line : parentDirContentsSplt) {
			std::deque < std::string > lineSplt = split(line, ",");
			if (lineSplt[0].compare("ROOT") == 0) {
				notAtRoot = false;
			} else if (lineSplt[1].compare(currDirHash) == 0) {
				// getting name of curr dir from parent dir's contents
				result.push_back(std::make_tuple(lineSplt[0], currDirHash));
				break;
			}
		}
		currDirHash = parentDirHash;
		dirContents = parentDirContents;
	}
	result.push_back(
			std::make_tuple("~", "ss0_" + generateStringHash(username + "/")));
	std::reverse(result.begin(), result.end());
	std::string filePath = "";
	for (std::tuple<std::string, std::string> filePathEntry : result) {
		filePath += std::get < 0 > (filePathEntry) + "/";
	}
	log("File path is: " + filePath);
	return result;
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
						return -1; // file or directory with this name already exists
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
/***************************** Start Discussion forum functions ************************/
std::string displayAllLedgerMessages(std::string ledgerHash) {
	std::string htmlMsgs = "";
	std::string targetPaxosServer = paxosServers[rand() % paxosServers.size()];
	rpc::client paxosServerClient(getAddrFromString(targetPaxosServer),
			getPortNoFromString(targetPaxosServer));
	paxosServerClient.set_timeout(5000);
	try {
		log(
				"getPaxos: Getting messages for ledger " + ledgerHash
						+ " from paxosServer: " + targetPaxosServer);
		resp_tuple resp = paxosServerClient.call("getPaxos", ledgerHash,
				"samecolumn").as<resp_tuple>();
		if (kvsResponseStatusCode(resp) == 0) {
			std::string allMessages = kvsResponseMsg(resp);
			std::deque < std::string > messageDeque = split(allMessages, "\n");
			log(
					"getPaxos: Got " + std::to_string(messageDeque.size())
							+ " messages for ledger " + ledgerHash
							+ " from getPaxos");
			for (std::string messageRaw : messageDeque) {
				if (messageRaw.length() > 0) {
					std::string sender = messageRaw.substr(0,
							messageRaw.find(":"));
					std::string message = messageRaw.substr(
							messageRaw.find(":") + 1);
					htmlMsgs +=
							"<div class=\"ledger-item\"><div class=\"ledger-info\">"
									+ sender + ":&nbsp;&nbsp;" + message
									+ "</div></div>";
				}
			}
			log(
					"getPaxos: Returning formatted messages for ledger "
							+ ledgerHash + " from getPaxos");
			return htmlMsgs;
		} else {
			log("getPaxos: no messages to display!");
			return "<div class=\"ledger-item\"><div class=\"ledger-info\">No posts yet.</div></div>";
		}
	} catch (rpc::timeout &t) {
		log("getPaxos: getPaxos call timed out! Returning error message");
		return "<div class=\"ledger-item\"><div class=\"ledger-info\">Failed to load messages! Please try again later.</div></div>";
	}
}

std::tuple<std::string, std::string> getLedgerNameFromHash(
		std::string targetLedgerHash) {
	std::string htmlMsgs = "";
	std::string targetPaxosServer = paxosServers[rand() % paxosServers.size()];
	rpc::client paxosServerClient(getAddrFromString(targetPaxosServer),
			getPortNoFromString(targetPaxosServer));
	paxosServerClient.set_timeout(5000);
	try {
		log(
				"getPaxos: Getting name of ledger with hash " + targetLedgerHash
						+ " from paxosServer: " + targetPaxosServer);
		// writing listOfLedgers to an arbitrary column so that it doesn't clash w/ a listOfLedgers ledger if a user
		// makes one
		resp_tuple resp = paxosServerClient.call("getPaxos", "listOfLedgers",
				"anythingbut_samecolumn").as<resp_tuple>();
		if (kvsResponseStatusCode(resp) == 0) {
			std::string allLedgers = kvsResponseMsg(resp);
			std::deque < std::string > ledgerDeque = split(allLedgers, "\n");
			log(
					"getPaxos: Got " + std::to_string(ledgerDeque.size())
							+ " ledgers from getPaxos");
			for (std::string ledgerRaw : ledgerDeque) {
				if (ledgerRaw.length() > 0) {
					std::string ledgerCreator = ledgerRaw.substr(0,
							ledgerRaw.find(","));
					ledgerRaw = ledgerRaw.substr(ledgerRaw.find(",") + 1);
					std::string ledgerHash = ledgerRaw.substr(0,
							ledgerRaw.find(","));
					std::string ledgerName = ledgerRaw.substr(
							ledgerRaw.find(",") + 1);
					if (ledgerHash.compare(targetLedgerHash) == 0) {
						return std::make_tuple(ledgerCreator, ledgerName);
					}
				}
			}
			log(
					"getPaxos: Returning formatted list of all ledgers from getPaxos");
		} else {
			log("getPaxos: no messages to display!");
		}
		return std::make_tuple("", "");
	} catch (rpc::timeout &t) {
		log("getPaxos: getPaxos call timed out! Returning error message");
		return std::make_tuple("", "");
	}
}

std::string displayAllLedgers() {
	std::string htmlMsgs = "";
	std::string targetPaxosServer = paxosServers[rand() % paxosServers.size()];
	rpc::client paxosServerClient(getAddrFromString(targetPaxosServer),
			getPortNoFromString(targetPaxosServer));
	paxosServerClient.set_timeout(5000);
	try {
		log(
				"getPaxos: Getting all ledgers from paxosServer: "
						+ targetPaxosServer);
		// writing listOfLedgers to an arbitrary column so that it doesn't clash w/ a listOfLedgers ledger if a user
		// makes one
		resp_tuple resp = paxosServerClient.call("getPaxos", "listOfLedgers",
				"anythingbut_samecolumn").as<resp_tuple>();
		if (kvsResponseStatusCode(resp) == 0) {
			std::string allLedgers = kvsResponseMsg(resp);
			std::deque < std::string > ledgerDeque = split(allLedgers, "\n");
			log(
					"getPaxos: Got " + std::to_string(ledgerDeque.size())
							+ " ledgers from getPaxos");
			std::deque<std::string>::reverse_iterator rit;
			for (rit = ledgerDeque.rbegin(); rit != ledgerDeque.rend(); ++rit) {
				std::string ledgerRaw = *rit;
				if (ledgerRaw.length() > 0) {
					std::string ledgerCreator = ledgerRaw.substr(0,
							ledgerRaw.find(","));
					ledgerRaw = ledgerRaw.substr(ledgerRaw.find(",") + 1);
					std::string ledgerHash = ledgerRaw.substr(0,
							ledgerRaw.find(","));
					std::string ledgerName = ledgerRaw.substr(
							ledgerRaw.find(",") + 1);
					htmlMsgs +=
							"<div style=\"cursor: pointer;\" onclick=\"window.location='/discuss/"
									+ ledgerHash
									+ "';\" class=\"ledger-item\"><div class=\"ledger-info\">"
									+ ledgerName
									+ "</div><div class=\"ledger-creator\">By&nbsp;"
									+ ledgerCreator + "</div></div>";
				}
			}
			log(
					"getPaxos: Returning formatted list of all ledgers from getPaxos");
			return htmlMsgs;
		} else {
			log("getPaxos: no messages to display!");
			return "<div class=\"ledger-item\"><div class=\"ledger-info\">No forums yet."
					"</div></div>";
		}
	} catch (rpc::timeout &t) {
		log("getPaxos: getPaxos call timed out! Returning error message");
		return "<div class=\"ledger-item\"><div class=\"ledger-info\">Failed to load forums. Please try again later!"
				"</div></div>";
	}
}

void createNewLedger(std::string ledgerInfo) {
	std::string targetPaxosServer = paxosServers[rand() % paxosServers.size()];
	rpc::client paxosServerClient(getAddrFromString(targetPaxosServer),
			getPortNoFromString(targetPaxosServer));
	paxosServerClient.set_timeout(5000);
	try {
		log(
				"putPaxos: Creating new ledger " + ledgerInfo
						+ " via paxosServer: " + targetPaxosServer);
		resp_tuple resp = paxosServerClient.call("putPaxos", "listOfLedgers",
				"anythingbut_samecolumn", ledgerInfo).as<resp_tuple>();
		log(
				"putPaxos returned with Status Code: "
						+ std::to_string(kvsResponseStatusCode(resp)));
		log("putPaxos returned with Value: " + kvsResponseMsg(resp));
		log(
				"putPaxos returned with value length: "
						+ std::to_string(kvsResponseMsg(resp).length()));
	} catch (rpc::timeout &t) {
		log("putPaxos: putPaxos call timed out!");
	}
	log("putPaxos: complete");
}

void sendNewMessage(std::string targetLedger, std::string message) {
	std::string targetPaxosServer = paxosServers[rand() % paxosServers.size()];
	rpc::client paxosServerClient(getAddrFromString(targetPaxosServer),
			getPortNoFromString(targetPaxosServer));
	paxosServerClient.set_timeout(5000);
	try {
		log(
				"putPaxos: Putting new message into ledger " + targetLedger
						+ " via paxosServer: " + targetPaxosServer);
		resp_tuple resp = paxosServerClient.call("putPaxos", targetLedger,
				"samecolumn", message).as<resp_tuple>();
		log(
				"putPaxos returned with Status Code: "
						+ std::to_string(kvsResponseStatusCode(resp)));
		log("putPaxos returned with Value: " + kvsResponseMsg(resp));
		log(
				"putPaxos returned with value length: "
						+ std::to_string(kvsResponseMsg(resp).length()));
	} catch (rpc::timeout &t) {
		log("putPaxos: putPaxos call timed out!");
	}
	log("putPaxos: complete");
}
/***************************** End Discussion forum functions ************************/
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

	/*
	 log("Results of multi-part processing: ");
	 log("Form data: ");
	 for (std::map<std::string, std::string>::iterator it = req.formData.begin();
	 it != req.formData.end(); it++) {
	 log(
	 "Key : " + it->first + " value : "
	 + std::to_string((it->second).size()));
	 }
	 */
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

	/*
	 log("Form data: ");
	 for (std::map<std::string, std::string>::iterator it = req.formData.begin();
	 it != req.formData.end(); it++) {
	 log("Key: " + it->first + " Value: " + it->second);
	 }
	 log("End form data");
	 */
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
		// log("Header: " + line);
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

struct http_response processRequest(struct http_request &req) {
	struct http_response resp;
	log("Entering process request");
	for (std::map<std::string, std::string>::iterator it = req.cookies.begin();
			it != req.cookies.end(); it++) {
		resp.cookies[it->first] = it->second;
	}

	/* Check to see if I'm the load balancer and if this request needs to be redirected */
	if (load_balancer && frontend_server_list.size() > 1) {
		std::string redirect_server = "";
		pthread_mutex_lock(&access_state_map);
		while (redirect_server.compare("") == 0
				|| frontend_state_map.find(redirect_server)
						== frontend_state_map.end()
				|| time(NULL)
						- frontend_state_map[redirect_server].last_modified > 3
				|| redirect_server.compare(this_server_state.http_address) == 0) {
			redirect_server = frontend_server_list[l_balancer_index];
			l_balancer_index = (l_balancer_index + 1)
					% frontend_server_list.size();
		}
		log("Redirect server: " + redirect_server);
		log_server_state(frontend_state_map[redirect_server]);
		pthread_mutex_unlock(&access_state_map);

		resp.status_code = 307;
		resp.status = "Temporary Redirect";
		resp.headers["Location"] = "http://" + redirect_server + "/";
		resp.cookies["redirected"] = "true";
		return resp;
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

	std::string fileError = "";
	if (req.formData["dir_name"].size() > 0) {
// File present to upload
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			std::string dirName = decodeURIComponent(req.formData["dir_name"]);
			dirName = decodeURIComponent(dirName);
			size_t index = 0;
			while (true) {
				index = dirName.find("%D", index);
				if (index == std::string::npos)
					break;
				dirName.replace(index, 2, "\r");
				index += 2;
			}
			int res = createDirectory(req, filepath, dirName);
			if (res == -1) {
				fileError =
						"File or directory with this name already exists within this directory.";
			} else if (res == -2) {
				fileError = "Something went wrong; please try again later.";
			}
		}
	} else if (req.formData["file"].size() > 0) {
// File present to upload
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			int res = uploadFile(req, filepath);
			if (res == -1) {
				fileError =
						"File or directory with this name already exists within this directory.";
			} else if (res == -2) {
				fileError = "Something went wrong; please try again later.";
			}
		}
	} else if (req.formData["itemToDelete"].size() > 0) {
		if (req.filepath.substr(0, 7).compare("/files/") == 0
				&& req.filepath.length() > 7
				&& isFileRouteDirectory(req.filepath.substr(7))) {
			std::string filepath = req.filepath.substr(7);
			std::string itemToDelete = req.formData["itemToDelete"];
			int res;
			if (itemToDelete.at(2) == '1') {
				// itemToDelete is a FILE
				res = deleteFile(req, filepath, itemToDelete);
				if (res == -1) {
					fileError = "File to be deleted was not found.";
				} else if (res == -2) {
					fileError = "Something went wrong; please try again later.";
				}
			} else if (itemToDelete.at(2) == '0') {
				// itemToDelete is a DIRECTORY
				// Recursively delete all subdirectories and files
				res = deleteDirectory(req, filepath, itemToDelete);
				if (res == -1) {
					fileError = "Directory to be deleted was not found.";
				} else if (res == -2) {
					fileError = "Something went wrong; please try again later.";
				}
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
			int res = renameFile(req, filepath, itemToRename, newName);
			if (res == -1) {
				fileError =
						"File or directory with this new name already exists within this directory.";
			} else if (res == -2) {
				fileError = "Something went wrong; please try again later.";
			} else if (res == -3) {
				fileError = "File to be renamed was not found.";
			}
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

			if (status == -1) {
				fileError =
						"File or directory with this name already exists within the target directory.";
			} else if (status == -2 || status == -12) {
				fileError = "Something went wrong; please try again later.";
			} else if (status == -3) {
				fileError = "File to be moved was not found.";
			} else if (status == -4) {
				fileError = "Cannot move directory into itself.";
			} else if (status == -13 || status == -11) {
				fileError =
						"Cannot find target directory. Remember that absolute path to target directory must begin with '~/'.";
			}
		}
	} else if (req.formData["newMessage"].size() > 0
			&& req.formData["targetLedger"].size() > 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			std::string message = req.cookies["username"] + ":"
					+ req.formData["newMessage"];
			log("Sending new message via putPaxos: " + message);
			message = decodeURIComponent(message);
			message = decodeURIComponent(message);
			sendNewMessage(req.formData["targetLedger"], message);
		}
	} else if (req.formData["newLedger"].size() > 0
			&& req.formData["ledgerCreator"].size() > 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			std::string ledgerCreator = req.formData["ledgerCreator"];
			std::string ledgerName = req.formData["newLedger"];
			ledgerName = decodeURIComponent(ledgerName);
			ledgerName = decodeURIComponent(ledgerName);
			std::string ledgerHash = generateStringHash(ledgerName);
			std::string ledgerInfo = ledgerCreator + "," + ledgerHash + ","
					+ ledgerName;
			log("Creating a new ledger via putPaxos: " + ledgerInfo);
			createNewLedger(ledgerInfo);
		}
	}

	if (req.filepath.compare("/") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") == req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string test = "<div class=\"error\"></div>";
			bool signuperr = false;
			if (req.cookies.find("error") != req.cookies.end()) {
				test =
						"<div class=\"error\" style=\"color:red; text-align:center;\";>"
								+ req.cookies["error"] + "</div>";
				resp.cookies.erase("error");
				if (req.cookies.find("signuperr") != req.cookies.end()) {
					signuperr = true;
					resp.cookies.erase("signuperr");
				}
			}
			resp.content =
					"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\"></head>"
							"<html>"
							"<body><div style=\"display:flex; flex-direction: row;\">"
							"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"

							"<div class=\"heading-wrapper\"><div class=\"heading\">PennCloud</div><div class=\"subtitle\">Increase your productivity and safely store what matters most with PennCloud.</div></div>"
							"<div style=\"display:flex; flex-direction: column;\">"
							+ test + "<div class=\"form-structor\">"
									"<div class=\"signup"
							+ (signuperr ? " slide-up" : "")
							+ "\">"
									"<h2 class=\"form-title\" id=\"signup\"><span>or</span>Sign Up</h2>"
									"<form id=\"signupF\" action=\"/signup\" enctype=\"multipart/form-data\" method=\"POST\">"
									"<div class=\"form-holder\">"
									"<input required class=\"input\" placeholder=\"Username\" name=\"username\" type=\"text\"/>"
									"<input placeholder=\"Password\" class=\"input\"  required name=\"password\""
									"type=\"password\"/>"
									"<input placeholder=\"Confirm Password\" class=\"input\"  required name=\"confirm_password\""
									"type=\"password\"/>"
									"</div>"
									"<input  class=\"submit-btn\" type=\"submit\" name=\"submit\" value=\"Sign Up\">"
									"</form>"
									"</div>"
									"<div class=\"login"
							+ (signuperr ? "" : " slide-up")
							+ "\">"
									"<div class=\"center\">"
									"<h2 class=\"form-title\" id=\"login\"><span>or</span>Log In</h2>"
									"<form id=\"loginF\" action=\"/login\" enctype=\"multipart/form-data\" method=\"POST\">"
									"<div class=\"form-holder\">"
									"<input required class=\"input\" placeholder=\"Username\" name=\"username\" type=\"text\"/>"
									"<input placeholder=\"Password\" class=\"input\"  required name=\"password\""
									"type=\"password\"/>"
									"</div>"
									"<input  class=\"submit-btn\" type=\"submit\" name=\"submit\" value=\"Log In\">"
									"</form>"
									"</div>"
									"</div>"
									"</div>" "<div class=\"error\"></div>"
									"</div>"
									"</body>"
									"<script type=\"text/javascript\" src=\"https://drive.google.com/uc?export=view&id=1Z08NGbZZz6WmAZocW6Oi_BR1hjCAzIMn\"></script>"
									/*"<body "
									 "style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
									 "center;\">" + test
									 + "<form id=\"login\" style=\"flex-direction: column; margin-bottom: 15px; display:"
									 + (signuperr ? "none" : "flex")
									 + ";\" action=\"/login\" enctype=\"multipart/form-data\" method=\"POST\">"
									 "<input style=\"margin-bottom: 15px;\" required placeholder=\"Username\" name=\"username\" type=\"text\"/><br/>"
									 "<input placeholder=\"Password\" style=\"margin-bottom: 15px;\" required name=\"password\" "
									 "type=\"password\"/><br/>"
									 "<input style=\"width: 100%; line-height: 24px;\" type=\"submit\" name=\"submit\" value=\"Log In\"><br/>"
									 "</form>"
									 "<form id=\"signup\" style=\"flex-direction: column; margin-bottom: 15px; display:"
									 + (signuperr ? "flex" : "none")
									 + ";\" action=\"/signup\" "
									 "enctype=\"multipart/form-data\" "
									 "method=\"POST\">"
									 "<input style=\"margin-bottom: 15px;\" placeholder=\"Username\"  required name=\"username\" type=\"text\"/><br/>"
									 "<input style=\"margin-bottom: 15px;\" placeholder=\"Password\"  required name=\"password\" "
									 "type=\"password\"/><br/>"
									 "<input style=\"margin-bottom: 15px;\" placeholder=\"Confirm Password\" required "
									 "name=\"confirm_password\" "
									 "type=\"password\"/>"
									 "<br/><input style=\"width: 100%; line-height: 24px;\" type=\"submit\" name=\"submit\" value=\"Sign Up\"><br/>"
									 "</form>"
									 "<br/><button style=\"line-height: 24px;\" id=\"switchButton\" type=\"button\">"
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
									 "</body>"*/
									"</html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		}
	} else if (req.filepath.compare("/login") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") == req.cookies.end()) {
			if (!req.formData["username"].empty()) {
				req.formData["username"][0] = std::toupper(
						req.formData["username"][0]);

				for (std::size_t i = 1; i < req.formData["username"].length();
						++i)
					req.formData["username"][i] = std::tolower(
							req.formData["username"][i]);
			}
			if (req.formData["username"] == "") {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] = "Invalid username.";
				resp.cookies["signuperr"] = "1";
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
					resp.cookies["signuperr"] = "1";
				} else if (getRespMsg != req.formData["password"]) {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/";
					resp.cookies["error"] = "Invalid password.";
					resp.cookies["signuperr"] = "1";
				} else {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					if (req.formData["username"].compare("Admin") != 0) {
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
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") == req.cookies.end()) {
			if (!req.formData["username"].empty()) {
				req.formData["username"][0] = std::toupper(
						req.formData["username"][0]);

				for (std::size_t i = 1; i < req.formData["username"].length();
						++i)
					req.formData["username"][i] = std::tolower(
							req.formData["username"][i]);
			}
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
			} else if (req.formData["password"].size() == 0
					|| std::all_of(req.formData["password"].begin(),
							req.formData["password"].end(), isspace)) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] =
						"Password cannot be empty or only spaces.";
			} else if (req.formData["password"]
					!= req.formData["confirm_password"]) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] = "Passwords do not match.";
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
					putKVS(resp.cookies["sessionid"], req.formData["username"],
							"chats", "");
					createRootDirForNewUser(req, resp.cookies["sessionid"]);
				}
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		}
	} else if (req.filepath.compare("/dashboard") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string userRootDir = "ss0_"
					+ generateStringHash(req.cookies["username"] + "/");
			resp.content =
					"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1yp7bV2amcRJTWoW6GpcXZ_D95Lg9_WNU\"><link href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.1/css/all.min.css\" rel=\"stylesheet\"></head>"
							"<html><body><div class=\"total-wrapper\">"
							"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"

							"<div class=\"nav\">"
							"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
							"<div class=\"nav-right\">"
							"<form action=\"/change-password\" method=\"POST\"><button style=\"line-height: 24px;\" type = \"submit\" >Change Password</button></form>"
							"</body></html>"
							"<form action=\"/logout\" method=\"POST\"><button style=\"line-height: 24px;\"  type = \"submit\" >Logout</button></form>"
							"</div>"
							"</div>"
							"<div class=\"main-content\">"
							"<div class=\"row-1\">"
							"<form action=\"/mailbox\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><i class=\"button-icon fas fa-mail-bulk\"></i><div class=\"button-text\">PennMail</div></div></button></form>"
							//"<form action=\"/compose\" method=\"POST\"> <input style=\"line-height: 24px;\" type = \"submit\" value=\"Compose Email\" /></form>"
							"<form action=\"/files/" + userRootDir
							+ "\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><i class=\"button-icon fas fa-box-open\"></i><div class=\"button-text\">PennDrive</div></div></button></form>"
									"</div><div class=\"row-2\"><form action=\"/discuss\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><i class=\"button-icon fas fa-users\"></i><div class=\"button-text\">PennDiscuss</div></div></button></form>"
									"<form action=\"/chat\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><i class=\"button-icon fas fa-comments\"></i><div class=\"button-text\">PennChat</div></div></button></form>"
									"</div></div></div></body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 7, "/files/") == 0) {
		log(std::to_string(__LINE__));
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
						std::string parentDirLink = "";
						std::deque<std::tuple<std::string, std::string>> paths =
								getFilePath(req, filepath);
						std::string filepathlink = "";
						for (std::tuple<std::string, std::string> a : paths) {
							filepathlink += ""
									"<a title=\"Open " + std::get < 0
									> (a) + "\" href=/files/" + std::get < 1
									> (a) + ">" + std::get < 0
									> (a) + "</a>" + "/";
						}
						std::string fileList = getFileList(req, filepath,
								parentDirLink);
						if (fileList == "-1") {
							resp.status_code = 307;
							resp.status = "Temporary Redirect";
							resp.headers["Location"] = "/dashboard";
							return resp;
						}
						/*resp.content =
						 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\"></head>"
						 "<html><body style ="
						 "\"display:flex;flex-direction:column;height:100%;padding:10px;\">"
						 "<div style=\"display:flex; flex-direction: row;\"><form style=\"padding-left:15px; padding-right:15px; margin-bottom:18px;\" action=\"/dashboard\" method=\"POST\"> <input style=\"line-height: 24px;\"  type = \"submit\" value=\"Dashboard\" /></form>"
						 + parentDirLink
						 + "<script> function getFile() {document.getElementById(\"upfile\").click();}</script>"
						 + "<script> function sub(obj) {var file = obj.value; document.getElementById(\"yourBtn\").innerHTML = file.substring(12); event.preventDefault();}</script>"
						 + "<form style=\"padding-left:45px; padding-right:45px; margin-bottom:18px;\" action=\"/files/"
						 + filepath
						 + "\" enctype=\"multipart/form-data\" method=\"POST\" name=\"myForm\">"
						 //"<label for=\"file\" style=\"line-height: 30px;\"></label>"
						 //"<input style=\"width: 232px;\" required type =\"file\" name=\"file\"/>"
						 "<button style=\"line-height: 24px;\" id=\"yourBtn\" onclick=\"getFile()\">Select File</button>"
						 "<div style=\"height: 0px; width: 0px; overflow:hidden;\"><input required id=\"upfile\" type=\"file\" value=\"upload\" name=\"file\" onchange=\"sub(this)\" /></div>"
						 "<input type=\"submit\" name=\"submit\" value=\"Upload\">"
						 "</form>"
						 "<form style=\"padding-left: 15px; margin-bottom:18px;\" action=\"/files/"
						 + filepath
						 + "\" enctype=\"multipart/form-data\" method=\"POST\""
						 "<label for=\"dir_name\"></label><input required type=\"text\" name=\"dir_name\" />"
						 "<input type=\"submit\" name=\"submit\" style=\"margin-left: 15px; line-height: 24px;\" value=\"Create Directory\"><br/>"
						 "</form>"
						 "</div>" + fileList + "<br/>"
						 "</body></html>";
						 */

						resp.content =
								"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1pOieVTXa5tbbaK7eg7aX3dMTWE_UR1Jm\"><link href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.1/css/all.min.css\" rel=\"stylesheet\"></head>"
										"<html><body ><div class=\"total-wrapper\">"
										"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"
										"<div class=\"nav\">"
										"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
										"<div class=\"nav-right\">"
										"<form action=\"/change-password\" method=\"POST\"><button  type = \"submit\" >Change Password</button></form>"
										"</body></html>"
										"<form action=\"/logout\" method=\"POST\"><button   type = \"submit\" >Logout</button></form>"
										"</div>"
										"</div>"
										"<div class=\"main-content\">"
										"<div class=\"sidebar\">"
										//"<button class=\"sidebar-link\" ><i class=\"fas fa-folder\"></i>&nbsp;&nbsp;Current Directory</button>"
										+ parentDirLink
										+ "<script> function getFile() {document.getElementById(\"upfile\").click();}</script>"
										+ "<script> function sub(obj) {var file = obj.value; console.log(file); document.getElementById(\"yourBtn\").innerHTML = file.substring(12); event.preventDefault();}</script>"
												"<form action=\"/files/"
										+ filepath
										+ "\" enctype=\"multipart/form-data\" method=\"POST\" name=\"myForm\">"
										//"<label for=\"file\" style=\"line-height: 30px;\"></label>"
										//"<input style=\"width: 232px;\" required type =\"file\" name=\"file\"/>"
												"<div class=\"upload_fil\">"
												"<button style=\"line-height: 24px;\" id=\"yourBtn\" onclick=\"getFile()\">Select File...</button>"
												"<div style=\"height: 0px; width: 0px; overflow:hidden;\"><input required id=\"upfile\" type=\"file\" value=\"upload\" name=\"file\" onchange=\"sub(this)\" /></div>"
												"<span class=\"input-wrap\"><input type=\"submit\" name=\"submit\" value=\"&nbsp;&nbsp;Upload File\"></span></div>"
										//"<button class=\"sidebar-link-special\" type=\"submit\"><i class=\"fas fa-file-medical\"></i>&nbsp;&nbsp;Upload</button></div>"
												"</form>"
												"<script>function encodeD() {document.getElementsByName(\"dir_name\")[0].value = encodeURIComponent(document.getElementsByName(\"dir_name\")[0].value); return true;}</script>"
												"<form accept-charset=\"utf-8\" onsubmit=\"return encodeD();\" action=\"/files/"
										+ filepath
										+ "\" method=\"POST\">"
												"<div class=\"create_dir\">"
												"<label for=\"dir_name\"></label><input placeholder=\"New Directory Name...\" class=\"dir_input\" required type=\"text\" name=\"dir_name\" />"
												"<button class=\"sidebar-link-special\" type = \"submit\"><i class=\"fas fa-folder-plus\"></i>&nbsp;&nbsp;Create Directory</button></div>"
												"</form>"
										+ "<div class=\"error-holder\">"
										+ fileError + "</div></div>"
												"<div class=\"inbox\">"
										+ "<div class=\"filepathlink-wrapper\">"
										+ filepathlink + "</div>" + fileList
										+ "</div></div></div></body></html>";
					} else {
						resp.status_code = 200;
						resp.status = "OK";
						resp.headers["Content-type"] = "text/plain";
						resp.content = kvsResponseMsg(getFileResp);
					}
				} else {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/dashboard";
					/*resp.status_code = 404;
					 resp.status = "Not found";
					 resp.headers["Content-type"] = "text/html";
					 resp.content =
					 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\"></head>"
					 "<html><body>"
					 "Requested file not found!"
					 "</body></html>";*/
				}
			} else {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/dashboard";
				/*resp.status_code = 404;
				 resp.status = "Not found";
				 resp.headers["Content-type"] = "text/html";
				 resp.content =
				 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\"></head>"
				 "<html><body>"
				 "Requested file not found!"
				 "</body></html>";*/
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/logout") == 0) {
		log(std::to_string(__LINE__));
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
		log(std::to_string(__LINE__));
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
								"<ul style=\"border-bottom: 1px solid #ccc; padding:10px 15px; margin: 0; width: 95%;\">";
						display +=
								"<div style=\"display:flex; flex-direction: row;\">"
										"<form action=\"/email\" method=\"post\" style=\"margin: 0;\">"
										"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to) + "\" />"
										+ "<label for =\"submit\" style=\"margin-right: 20px; width: 255px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
										+ escape(title) + "</label>"
										+ "<label for =\"submit\" style=\"margin-right: 20px; width: 255px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
										+ escape(title1) + "</label>"
										+ "<label for =\"submit\" style=\"margin-right: 20px; width: 255px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align: middle;\">"
										+ escape(title2) + "</label>"
										+ "<button class=\"item-button\" type = \"submit\"><i title=\"View\" class=\"fas fa-eye\"></i></button>"
												"</form>"
												"<form style=\"padding-left:15px; padding-right:15px; margin: 0;\" action=\"/compose\" method=\"POST\">"
												"<input type=\"hidden\" name=\"type\" value=\"reply\">"
												"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to)
										+ "\" />"
												"<button class=\"item-button\" type = \"submit\"><i title=\"Reply\" class=\"fas fa-reply\"></i></button></form>"
												"<form action=\"/compose\" method=\"POST\" style=\"margin-bottom:0; padding-right:15px;\">"
												"<input type=\"hidden\" name=\"type\" value=\"forward\">"
												"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to)
										+ "\" />" "<button class=\"item-button\" type = \"submit\"><i title=\"Forward\" class=\"fas fa-share\"></i></button></form>"
												"<form action=\"/delete\" method=\"POST\" style=\"margin-bottom:0;\">"
												"<input type=\"hidden\" name=\"header\" value=\""
										+ encodeURIComponent(to)
										+ "\" />" "<button class=\"item-button\" type = \"submit\"><i title=\"Delete\" class=\"fas fa-trash-alt\"></i></button></form></div>";
						display += "</ul>";
					}
				}
			}
			if (display == "") {
				display +=
						"<ul style=\"border-bottom: 1px solid #ccc; padding:15px; margin: 0; width: 95%; text-align: center;\">No mail yet!</ul>";
			}
			resp.content =
					"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1aO2UaTSAoXOhadVi5HXHN8RCLbE4O_Qt\"><link href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.1/css/all.min.css\" rel=\"stylesheet\"></head>"
							"<html><body ><div class=\"total-wrapper\">"
							"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"
							"<div class=\"nav\">"
							"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
							"<div class=\"nav-right\">"
							"<form action=\"/change-password\" method=\"POST\"><button  type = \"submit\" >Change Password</button></form>"
							"</body></html>"
							"<form action=\"/logout\" method=\"POST\"><button   type = \"submit\" >Logout</button></form>"
							"</div>"
							"</div>"
							"<div class=\"main-content\">"
							"<div class=\"sidebar\">"
							"<form action=\"/compose\" method=\"POST\" > <button class=\"sidebar-link\" type = \"submit\"><i class=\"far fa-edit\"></i>&nbsp;&nbsp;Compose</button></form>"
							"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-button\" type = \"submit\"><i class=\"fas fa-inbox\"></i>&nbsp;&nbsp;Inbox</button></form>"
							//"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-link\" type = \"submit\"><i class=\"far fa-paper-plane\"></i>&nbsp;&nbsp;Sent</button></form>"
							"</div>"
							"<div class=\"inbox\">"
							"<div style=\"font-weight: bold; padding-left: 15px; padding-right: 15px; width: 95%; padding-bottom: 10px;padding-top: 20px; display:flex; flex-direction: row;\"><label style=\"margin-right: 20px; width: 255px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">Sender</label>"
							"<label style=\"margin-right: 20px; width: 255px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">Subject</label>"
							"<label style=\"margin-right: 20px; width: 255px; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">Date</label>"
							"</div><div style=\"border-bottom: 1px solid black; width: 100%;\"></div><div class=\"emails\">"
							+ display
							+ "</div></div></div></div></body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/compose") == 0) {
		log(std::to_string(__LINE__));
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
					"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1rfpVOIMTUZBvu1pWILDKiRrDi9u0oLpi\"><link href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.1/css/all.min.css\" rel=\"stylesheet\"></head>"
							"<html><body ><div class=\"total-wrapper\">"

							"<div class=\"nav\">"
							"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
							"<div class=\"nav-right\">"
							"<form action=\"/change-password\" method=\"POST\"><button  type = \"submit\" >Change Password</button></form>"
							"</body></html>"
							"<form action=\"/logout\" method=\"POST\"><button   type = \"submit\" >Logout</button></form>"
							"</div>"
							"</div>"
							"<script>function encode() {document.getElementsByName(\"to\")[0].value = encodeURIComponent(document.getElementsByName(\"to\")[0].value); document.getElementsByName(\"subject\")[0].value = encodeURIComponent(document.getElementsByName(\"subject\")[0].value); document.getElementsByName(\"content\")[0].value = encodeURIComponent(document.getElementsByName(\"content\")[0].value); return true;}</script>"
							"<div class=\"main-content\">"
							"<div class=\"sidebar\">"
							"<form action=\"/compose\" method=\"POST\" > <button class=\"sidebar-button\" type = \"submit\"><i class=\"far fa-edit\"></i>&nbsp;&nbsp;Compose</button></form>"
							"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-link\" type = \"submit\"><i class=\"fas fa-inbox\"></i>&nbsp;&nbsp;Inbox</button></form>"
							//"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-link\" type = \"submit\"><i class=\"far fa-paper-plane\"></i>&nbsp;&nbsp;Sent</button></form>"
							"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-link danger\" type = \"submit\"><i class=\"fas fa-trash-alt\"></i>&nbsp;&nbsp;Discard</button></form>"
							"<form accept-charset=\"utf-8\" id=\"compose\" action=\"/send\" onsubmit=\"return encode();\" method=\"POST\"> <button class=\"sidebar-link success\" type = \"submit\"><i class=\"far fa-paper-plane\"></i>&nbsp;&nbsp;Send</button></form>"
							"</div>"
							"<div class=\"inbox\">"
							"<div style=\"display:flex; flex-direction: row; \">"
							/*"<label form=\"compose\" for=\"to\" style=\"height:30px; display: flex; align-items: center; width: 75px;\">To:&nbsp;</label>"*/
							"<input placeholder=\"Recipients\" required form=\"compose\" style=\"flex:1;\" name=\"to\" type=\"text\" value=\""
							+ rec + "\"/></div>"
							+ "<div style=\"display:flex; flex-direction: row; \">"
							/*"<label form=\"compose\" for=\"subject\" style=\"height:30px; display: flex; align-items: center; width: 75px;\">Subject:&nbsp;</label>"*/
									"<input placeholder=\"Subject\" required form=\"compose\" style=\"flex:1;\" name=\"subject\" type=\"text\" value=\""
							+ sub + "\"/></div>"
							+ "<div style=\"flex:1\">"
									"<textarea name=\"content\" form=\"compose\" style=\"width:100%; height: 100%;\">"
							+ existing + "</textarea>"
									"</div>"
									"</div></div></div></body></html>";

			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/email") == 0) {
		log(std::to_string(__LINE__));
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
							std::string title =
									"<span style=\"font-weight: bold\">From: </span>";
							std::string title1 =
									"<span style=\"font-weight: bold\">Subject: </span>";
							std::string title2 =
									"<span style=\"font-weight: bold\">Date: </span>";
							unsigned first = to.find('<');
							unsigned last = to.find('>');
							std::string titleA = to.substr(first + 1,
									last - first - 1);
							std::string title1A = subject.substr(9);
							std::string title2A = to.substr(last + 2);
							display +=
									"<ul style=\"border-bottom: 1px solid #ccc; padding:15px; margin: 0;\">";
							display +=
									"<label style=\"margin-right: 20px; margin-bottom: 10px; width: 100%; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
											+ title + escape(titleA)
											+ "</label>"
											+ "<label style=\"margin-right: 20px; margin-bottom: 10px; width: 100%; display: inline-block; overflow: hidden; text-overflow: ellipsis; vertical-align:middle;\">"
											+ title1 + escape(title1A)
											+ "</label>"
											+ "<label style=\"margin-right: 20px; margin-bottom: 10px; vertical-align: middle;\">"
											+ title2 + escape(title2A)
											+ "</label>";
							display += "</ul>";
							found = true;
							display +=
									"<span style=\"white-space: pre-wrap; padding:15px; overflow-y:scroll; overflow-x: hidden; word-break: break-word;\">";
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
				/*resp.content =
				 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\"></head>"
				 "<html><body "
				 "style=\"display:flex;flex-direction:column;height:100%;padding:10px;\">"
				 "<div style=\"display:flex; flex-direction: row;\"><form style=\"padding-left:15px; padding-right:15px; margin-bottom:18px;\" action=\"/dashboard\" method=\"POST\"> <input style=\"line-height:24px;\" type = \"submit\" value=\"Dashboard\" /></form>"
				 "<form action=\"/mailbox\" method=\"POST\" style=\"padding-right: 15px; margin-bottom:18px;\"> <input style=\"line-height:24px;\" type = \"submit\" value=\"Mailbox\" /></form>"
				 "<form style=\"padding-right:15px; margin: 0;\" action=\"/compose\" method=\"POST\">"
				 "<input type=\"hidden\" name=\"type\" value=\"reply\">"
				 "<input type=\"hidden\" name=\"header\" value=\""
				 + encodeURIComponent(header)
				 + "\" />"
				 "<input style=\"line-height:24px;\" type = \"submit\" value=\"Reply\" /></form>"
				 "<form action=\"/compose\" method=\"POST\" style=\"margin-bottom:0; padding-right:15px;\">"
				 "<input type=\"hidden\" name=\"type\" value=\"forward\">"
				 "<input type=\"hidden\" name=\"header\" value=\""
				 + encodeURIComponent(header)
				 + "\" />" "<input style=\"line-height:24px;\" type = \"submit\" value=\"Forward\" /></form>"
				 "<form action=\"/delete\" method=\"POST\" style=\"margin-bottom:0;\">"
				 "<input type=\"hidden\" name=\"header\" value=\""
				 + encodeURIComponent(header)
				 + "\" />" "<input style=\"line-height:24px;\" type = \"submit\" value=\"Delete\" /></form></div>"
				 "<ul style=\"border-top: 1px solid black; padding:0px; margin: 0;\"></ul>"
				 + display + "</body></html>";*/

				resp.content =
						"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1RvFSbln2s830QKnz2aBobC4d4Vck_6KN\"><link href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.1/css/all.min.css\" rel=\"stylesheet\"></head>"
								"<html><body ><div class=\"total-wrapper\">"
								"<div class=\"nav\">"
								"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
								"<div class=\"nav-right\">"
								"<form action=\"/change-password\" method=\"POST\"><button  type = \"submit\" >Change Password</button></form>"
								"</body></html>"
								"<form action=\"/logout\" method=\"POST\"><button   type = \"submit\" >Logout</button></form>"
								"</div>"
								"</div>"
								"<script>function encode() {document.getElementsByName(\"to\")[0].value = encodeURIComponent(document.getElementsByName(\"to\")[0].value); document.getElementsByName(\"subject\")[0].value = encodeURIComponent(document.getElementsByName(\"subject\")[0].value); document.getElementsByName(\"content\")[0].value = encodeURIComponent(document.getElementsByName(\"content\")[0].value); return true;}</script>"
								"<div class=\"main-content\">"
								"<div class=\"sidebar\">"
								"<form action=\"/compose\" method=\"POST\" > <button class=\"sidebar-button\" type = \"submit\"><i class=\"far fa-edit\"></i>&nbsp;&nbsp;Compose</button></form>"
								"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-link\" type = \"submit\"><i class=\"fas fa-inbox\"></i>&nbsp;&nbsp;Inbox</button></form>"
								//"<form action=\"/mailbox\" method=\"POST\" > <button class=\"sidebar-link\" type = \"submit\"><i class=\"far fa-paper-plane\"></i>&nbsp;&nbsp;Sent</button></form>"

								"<form  action=\"/compose\" method=\"POST\">"
								"<input type=\"hidden\" name=\"type\" value=\"reply\">"
								"<input type=\"hidden\" name=\"header\" value=\""
								+ encodeURIComponent(header)
								+ "\" />"
										"<button class=\"sidebar-link\" type = \"submit\"><i class=\"fas fa-reply\"></i>&nbsp;&nbsp;Reply</button></form>"
										"<form action=\"/compose\" method=\"POST\" >"
										"<input type=\"hidden\" name=\"type\" value=\"forward\">"
										"<input type=\"hidden\" name=\"header\" value=\""
								+ encodeURIComponent(header)
								+ "\" />" "<button class=\"sidebar-link\" type = \"submit\"><i class=\"fas fa-share\"></i>&nbsp;&nbsp;Forward</button></form>"
										"<form action=\"/delete\" method=\"POST\" >"
										"<input type=\"hidden\" name=\"header\" value=\""
								+ encodeURIComponent(header)
								+ "\" />" "<button class=\"sidebar-link danger\" type = \"submit\"><i class=\"fas fa-trash-alt\"></i>&nbsp;&nbsp;Delete</button></form>"
										"</div>"
										"<div class=\"inbox\">" + display
								+ "</div></div></body></html>";

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
		log(std::to_string(__LINE__));
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
		log(std::to_string(__LINE__));
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
						std::string example = token;
						for (std::size_t i = 1; i < example.length(); ++i)
							example[i] = std::tolower(example[i]);
						if (example.length() >= ending.length()) {
							local = (0
									== example.compare(
											example.length() - ending.length(),
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
							if (!addr.empty()) {
								addr[0] = std::toupper(addr[0]);

								for (std::size_t i = 1; i < addr.length(); ++i)
									addr[i] = std::tolower(addr[i]);

								resp_tuple getResp = getKVS(addr, addr,
										"mailbox");
								std::string current = kvsResponseMsg(getResp);
								int getRespStatusCode = kvsResponseStatusCode(
										getResp);
								if (getRespStatusCode == 0) {
									std::string final = temp;
									final += current;
									resp_tuple resp2 = cputKVS(addr, addr,
											"mailbox", current, final);
									int respStatus2 = kvsResponseStatusCode(
											resp2);
									while (respStatus2 != 0) {
										getResp = getKVS(addr, addr, "mailbox");
										getRespStatusCode =
												kvsResponseStatusCode(getResp);
										current = kvsResponseMsg(getResp);
										final = temp;
										final += current;
										resp2 = cputKVS(addr, addr, "mailbox",
												current, final);
										respStatus2 = kvsResponseStatusCode(
												resp2);
									}
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
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("Admin") == 0) {
			time_t now = time(NULL);
			log("now" + std::to_string(now));
			requstStateFromAllServers();
			populateAdminCache();
			auto allBackendNodes = my_admin_console_cache.allBackendServers;
			auto activeBackendNodesCollection =
					my_admin_console_cache.activeBackendServersList;
			sleep(1);
			std::string message = "<head><meta charset=\"UTF-8\">"
			//"<link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\">"
							"</head><html><body><form action=\"/logout\" method=\"POST\">"
							"<input type = \"submit\" value=\"Logout\" /></form><br><h3>Frontend servers:</h3><br><ul><hr>";
			for (std::map<std::string, struct server_state>::iterator it =
					frontend_state_map.begin(); it != frontend_state_map.end();
					it++) {
				log_server_state(it->second);
				log(
						"Last modified: "
								+ std::to_string(it->second.last_modified));
				bool stopped_by_console =
						my_admin_console_cache.stopped_servers.find(
								trim(it->first))
								!= my_admin_console_cache.stopped_servers.end();
				std::string status =
						(stopped_by_console) ?
								"Not Responding" :
								((it->second.last_modified >= now) ?
										"Active" : "Not Responding");
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
			message += "</ul><h3>Backend servers:</h3><br><ul>";
			log("ADMIN all nodes");
			for (auto node : allBackendNodes) {
				log(std::get < 2 > (node));
				bool stopped_by_console =
						my_admin_console_cache.stopped_servers.find(
								trim(std::get < 3 > (node)))
								!= my_admin_console_cache.stopped_servers.end();
				;
				std::string status =
						(stopped_by_console) ?
								"Not Responding" :
								((std::find(
										activeBackendNodesCollection.begin(),
										activeBackendNodesCollection.end(),
										std::get < 2 > (node))
										!= activeBackendNodesCollection.end()) ?
										"Active" : "Not Responding");
				message += "<l1>" + std::get < 2 > (node);
				message += " Status: " + status;
				message +=
						"<form action=\"/serverinfo/b/1/" + std::get < 3
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
			registerCacheAccess("admin", "all");
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			resp.headers["Content-length"] = std::to_string(message.length());
			resp.content = message;
		}
	} else if (req.filepath.compare("/change-password") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			std::string test = "<div class=\"error\"></div>";
			if (req.cookies.find("error") != req.cookies.end()) {
				test = "<div class=\"error\" style=\"color:red\";>"
						+ req.cookies["error"] + "</div>";
				resp.cookies.erase("error");
			}
			resp.content =
					"<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=13aiSuASW9mzew62vNPzC3AFBWJ1YVTtg\"><link href=\"https://cdnjs.cloudflare.com/ajax/libs/font-awesome/5.15.1/css/all.min.css\" rel=\"stylesheet\"></head>"
							"<html><body><div class=\"total-wrapper\">"
							"<div class=\"nav\">"
							"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
							"<div class=\"nav-right\">"
							"<form action=\"/dashboard\" method=\"POST\"><button style=\"line-height: 24px;\" type = \"submit\" >Cancel</button></form>"
							"</body></html>"
							"<form action=\"/logout\" method=\"POST\"><button style=\"line-height: 24px;\"  type = \"submit\" >Logout</button></form>"
							"</div>"
							"</div>"
							"<div style=\"display:flex; flex-direction: column; justify-content:center; align-items: center; width: 100vw; height: 92vh; overflow: hidden;\">"
							+ test
							+ "<div class=\"form-structor\">"
									"<div class=\"signup\">"
									"<h2 class=\"form-title\" id=\"signup\"><span></span>Change Password</h2>"
									"<form id=\"signupF\" action=\"/change\" enctype=\"multipart/form-data\" method=\"POST\">"
									"<div class=\"form-holder\">"
									"<input required class=\"input\" placeholder=\"Current Password\" name=\"old\" type=\"password\"/>"
									"<input placeholder=\"New Password\" class=\"input\"  required name=\"new\""
									"type=\"password\"/>"
									"<input placeholder=\"Confirm New Password\" class=\"input\"  required name=\"confirm_new\""
									"type=\"password\"/>"
									"</div>"
									"<input  class=\"submit-btn\" type=\"submit\" name=\"submit\" value=\"Change Password\">"
									"</form>"
									"</div>"
									"</div>" "<div class=\"error\"></div>"
									"</div>"
									"</body>"
									"</html>";

			/*resp.content =
			 "<head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1iikoQUWZmEpJ6XyCKMU4hrnkA9ZTg_5B\"></head>"
			 "<html><body "
			 "style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
			 "center;\">" + test
			 + "<form id=\"change\" style=\"display: flex; flex-direction: column; margin-bottom: 15px;\""
			 + "action=\"/change\" "
			 "enctype=\"multipart/form-data\" "
			 "method=\"POST\""
			 "<input style=\"margin-bottom: 15px;\" placeholder=\"Current Password\" required name=\"old\" type=\"password\"/><br/>"
			 "<input style=\"margin-bottom: 15px;\" placeholder=\"New Password\" required name=\"new\" "
			 "type=\"password\"/><br/>"
			 "<input style=\"margin-bottom: 15px;\" placeholder=\"Confirm New Password\"  required "
			 "name=\"confirm_new\" "
			 "type=\"password\"/><br/>"
			 "<input style=\"width: 100%; line-height: 24px;\" type=\"submit\" name=\"submit\" value=\"Change Password\"></form><br/><br/>"
			 "<form id=\"cancel\" style=\"\" action=\"/dashboard\" method=\"POST\"> <input style=\"width: 100%; line-height: 24px;\" type = \"submit\" value=\"Cancel\" /></form>"
			 "</body></html>";*/
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/change") == 0) {
		log(std::to_string(__LINE__));
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
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("Admin") == 0) {
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
			my_admin_console_cache.stopped_servers.insert(trim(target));
			log(
					"Stopped servers size: "
							+ std::to_string(
									my_admin_console_cache.stopped_servers.size()));
			registerCacheAccess("stopserver", target);
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/admin";
		}
	} else if (req.filepath.compare(0, 14, "/resumeserver/") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("Admin") == 0) {
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
			my_admin_console_cache.stopped_servers.erase(trim(target));
			registerCacheAccess("resumeserver", target);
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/admin";
		}
	} else if (req.filepath.compare(0, 12, "/serverinfo/") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("Admin") == 0) {
			std::deque < std::string > tokens = split(req.filepath, "/");
			std::string target = trim(tokens.back());
			tokens.pop_back();
			log("Server info for: " + target);
			bool frontend_target = (trim(tokens.back()).compare("f") == 0);
			std::string message =
					"<head><meta charset=\"UTF-8\"></head><html><body><form action=\"/logout\" method=\"POST\">"
							"<input type = \"submit\" value=\"Logout\" /></form><form action=\"/admin\" method=\"POST\">"
							"<input type = \"submit\" value=\"Back\" /></form><br>";
			if (frontend_target) {
				// Show front end state from frontend state map
				log("Showing frontend info for : " + target);
				struct server_state state = frontend_state_map[target];
				message += "State of " + target
						+ " <ul><li> Number of http connections: "
						+ std::to_string(state.http_connections) + "</li>"
								"<li> Number of smtp connections: "
						+ std::to_string(state.smtp_connections) + "</li>"
								"<li> Number of internal connections: "
						+ std::to_string(state.internal_connections) + "</li>"
								"<li> Number of active threads: "
						+ std::to_string(state.num_threads) + "</li></ul>";
			} else {
				log("Target: " + target);
				message +=
						"<form action=\"/refreshadmincache\" method=\"POST\">"
								"<input type = \"submit\" value=\"Refresh\" /></form><br>";

				// TODO: Get all rows from backend
				bool cache_valid = isAdminCacheValidFor("serverinfo", target,
						10);
				if (!cache_valid) {
					if (!my_admin_console_cache.initialized)
						populateAdminCache();
					getMoreInfoFor(target);
				}
				int page = 1;
				try {
					page = stoi(trim(tokens.back()));
				} catch (const std::invalid_argument &ia) {
					page = 1;
				}
				if (page > 1
						&& my_admin_console_cache.rowToAllItsCols.size()
								<= (10 * (page - 1)))
					page = 1;
				log("Page: " + std::to_string(page));
				// TODO: display first n bytes (with link to new page ...)
				message += "KVS Table for " + target + "<hr>";
				int i;
				for (i = (page - 1) * 10;
						i
								< std::min(
										(int) my_admin_console_cache.rowToAllItsCols.size(),
										page * 10); i++) {
					std::tuple<std::string, std::deque<std::string>> entry =
							my_admin_console_cache.rowToAllItsCols.at(i);
					std::string row = std::get < 0 > (entry);
					message += "Row name: " + row + "<br><ul>";
					for (std::string col : std::get < 1 > (entry)) {
						auto first50BytesRaw = getFirstNBytesKVS(target, row,
								col, 50);
						if (std::get < 0 > (first50BytesRaw) != 0)
							continue;
						int total_size = std::get < 1 > (first50BytesRaw);
						std::string raw_bytes = std::get < 2
								> (first50BytesRaw);
						log(
								"ERROR CODE GETNBYTES: "
										+ std::to_string(
												std::get < 0
														> (first50BytesRaw)));
						log("SIZE GETNBYTES: " + std::to_string(total_size));
						log("RAW BYTES GETNBYTES" + raw_bytes);
						message += "<li> Col: " + col;
						message +=
								(total_size <= 50) ?
										"<br> Entire entry: <br>" :
										"<br> First 50 Bytes: <br>";
						message.append(raw_bytes);
						message += "<br></li>";
						if (total_size > 50)
							message += "<a href = \"/adminfiles/"
									+ std::to_string(total_size) + "/" + col
									+ "/" + row + "/" + target
									+ "\" target=\"_blank\"> See More</a>";
					}
					message += "</ul><hr>";
				}
				if (page > 1) {
					message +=
							"<form action=\"/serverinfo/"
									+ std::to_string(page - 1) + "/" + target
									+ "\" method=\"POST\">"
											"<input type = \"submit\" value=\"Prev\" /></form><br>";
				}
				if (i < my_admin_console_cache.rowToAllItsCols.size()) {
					message +=
							"<form action=\"/serverinfo/"
									+ std::to_string(page + 1) + "/" + target
									+ "\" method=\"POST\">"
											"<input type = \"submit\" value=\"Next\" /></form><br>";
				}
				registerCacheAccess("serverinfo", target);
			}
			message += "</body></html>";
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			resp.headers["Content-length"] = std::to_string(message.size());
			resp.content = message;
		}
	} else if (req.filepath.compare(0, 12, "/adminfiles/") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("Admin") == 0) {
			std::deque < std::string > tokens = split(req.filepath, "/");
			std::string target = trim(tokens.back());
			tokens.pop_back();
			std::string row = trim(tokens.back());
			tokens.pop_back();
			std::string col = trim(tokens.back());
			tokens.pop_back();
			int total_size = std::stoi(trim(tokens.back()));
			auto raw_data_tuple = getFirstNBytesKVS(target, row, col,
					total_size);
			if (std::get < 0 > (raw_data_tuple) != 0) {
				resp.status = "Not Found";
				resp.status_code = 404;
			} else {
				std::string raw_data = std::get < 2 > (raw_data_tuple);
				resp.status = "OK";
				resp.status_code = 200;
				resp.headers["Content-type"] = "text/html";
				std::string message = "<html><body>";
				message.append(raw_data);
				message += "</body></html>";
				resp.headers["Content-length"] = std::to_string(message.size());
				resp.content = message;
			}
		}
	} else if (req.filepath.compare("/refreshadmincache") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()
				&& req.cookies["username"].compare("Admin") == 0) {
			std::string redirect = my_admin_console_cache.last_accessed_for;
			my_admin_console_cache.rowToAllItsCols.clear();
			my_admin_console_cache.last_modified = time(NULL);
			my_admin_console_cache.last_modified_by = "refreshadmincache";
			my_admin_console_cache.last_accessed_for = "none";
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/serverinfo/1/" + redirect;
		}
	} else if (req.filepath.substr(0, 8).compare("/discuss") == 0) {
		log(std::to_string(__LINE__));
		if (req.cookies.find("username") != req.cookies.end()) {
			if (req.filepath.length() > 10
					&& req.filepath.substr(0, 9).compare("/discuss/") == 0) {
				// View specific ledger
				std::string ledgerHash = req.filepath.substr(9);
				std::tuple < std::string, std::string > ledgerInfo =
						getLedgerNameFromHash(ledgerHash);
				std::string ledgerCreator = std::get < 0 > (ledgerInfo);
				std::string ledgerName = std::get < 1 > (ledgerInfo);
				if (ledgerCreator == "" && ledgerName == "") {
					resp.status_code = 307;
					resp.status = "Temporary Redirect";
					resp.headers["Location"] = "/discuss";
					return resp;
				}
				resp.status = "OK";
				resp.status_code = 200;
				resp.headers["Content-type"] = "text/html";

				std::string message =
						"<html><head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1AzSbD5ig5sSeHDvxG4yTneWVpXx5gnwJ\"></head><body><div class=\"total-wrapper\">"
								"<div class=\"nav\">"
								"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
								"<div class=\"nav-right\">"
								"<form action=\"/change-password\" method=\"POST\"><button style=\"line-height: 24px;\" type = \"submit\" >Change Password</button></form>"
								"</body></html>"
								"<form action=\"/logout\" method=\"POST\"><button style=\"line-height: 24px;\"  type = \"submit\" >Logout</button></form>"
								"</div>"
								"</div>"
								"<div class=\"main-content\">";
				message +=
						"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"
								"<script>function encodeMessage() {document.getElementsByName(\"newMessage\")[0].value = encodeURIComponent(document.getElementsByName(\"newMessage\")[0].value); return true;}</script>"
								"<div class=\"discuss-header\">"
								"<form action=\"/discuss\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><div class=\"button-text\">PennDiscuss</div></div></button></form>"
								"<div class=\"forum-name\">" + ledgerCreator
								+ ":&nbsp;&nbsp;" + ledgerName + "</div></div>"
										"<div class=\"discuss-content\">";
				message += displayAllLedgerMessages(ledgerHash);
				message +=
						"</div>"
								"<div class=\"discuss-new\">"
								"<form accept-charset=\"utf-8\" onsubmit=\"return encodeMessage();\" action=\"/discuss/"
								+ ledgerHash
								+ "\" method=\"post\">"
										"<div class=\"discuss-new-row\">"
										"<input type=\"hidden\" name=\"targetLedger\" value=\""
								+ ledgerHash
								+ "\"/>"
										"<label for=\"newMessage\"></label><input required type=\"text\" name=\"newMessage\" placeholder=\"Write a post\">"
										"<input type=\"submit\" name=\"submit\" value=\"Submit\" />"
										"</div>"
										"</form>"
										"</div>"
										"</div>";
				message += "</div></body></html>";
				resp.headers["Content-length"] = std::to_string(message.size());
				resp.content = message;
			} else {
				// View list of ledgers
				resp.status = "OK";
				resp.status_code = 200;
				resp.headers["Content-type"] = "text/html";
				std::string message =
						"<html><head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1xheWcLxhfn4LNAupczXfW-gUUuahw1Lx\"></head><body><div class=\"total-wrapper\">"
								"<div class=\"nav\">"
								"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
								"<div class=\"nav-right\">"
								"<form action=\"/change-password\" method=\"POST\"><button style=\"line-height: 24px;\" type = \"submit\" >Change Password</button></form>"
								"</body></html>"
								"<form action=\"/logout\" method=\"POST\"><button style=\"line-height: 24px;\"  type = \"submit\" >Logout</button></form>"
								"</div>"
								"</div>"
								"<div class=\"main-content\">";
				message +=
						"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"
								"<script>function encodeLedger() {document.getElementsByName(\"newLedger\")[0].value = encodeURIComponent(document.getElementsByName(\"newLedger\")[0].value); return true;}</script>"
								"<div class=\"discuss-header\">"
								"<form action=\"/discuss\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><div class=\"button-text\">PennDiscuss</div></div></button></form>"
								"</div>"
								"<div class=\"discuss-new\">"
								"<form accept-charset=\"utf-8\" onsubmit=\"return encodeLedger();\" action=\"/discuss\" method=\"post\">"
								"<div class=\"discuss-new-row\">"
								"<input type=\"hidden\" name=\"ledgerCreator\" value=\""
								+ req.cookies["username"]
								+ "\"/>"
										"<label for=\"newLedger\"></label><input required type=\"text\" name=\"newLedger\" placeholder=\"New Forum Name...\">"
										"<input type=\"submit\" name=\"submit\" value=\"Create\" />"
										"</div>"
										"</form>"
										"</div>"
										"<div class=\"discuss-content\">";
				message += displayAllLedgers();
				message += "</div>"

						"</div>";
				message += "</div></div></body></html>";
				resp.headers["Content-length"] = std::to_string(message.size());
				resp.content = message;
			}
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare("/chat") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";
			resp_tuple getResp = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "chats");
			std::string getRespMsg = kvsResponseMsg(getResp);
			std::stringstream ss(getRespMsg);
			std::string chatroom;
			std::string display = "";
			if (getRespMsg != "") {
				display += "";
				while (std::getline(ss, chatroom, '\n')) {

					auto tokens = split(chatroom, "\t");
					std::string chatname = tokens.at(0), owner = tokens.at(1),
							chathash = tokens.at(2);
					display +=
							"<div style=\"cursor: pointer;\" onclick=\"window.location='/joinchat/"
									+ owner + "/" + chathash
									+ "';\" class=\"ledger-item\"><div class=\"ledger-info\">"
									+ escape(chatname) + "</div></div>";
					group_to_clients[chathash].insert(req.cookies["username"]);
				}
				display += "";
			}
			if (display == "") {
				display +=
						"<div class=\"ledger-item\"><div class=\"ledger-info\">"
								"No chat rooms yet!" "</div></div>";
			}
			resp.content =
					"<html><head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1l3sfLH9I09uAtIIDNiQt_6I710SRu3zO\"></head><body><div class=\"total-wrapper\">"
							"<div class=\"nav\">"
							"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
							"<div class=\"nav-right\">"
							"<form action=\"/change-password\" method=\"POST\"><button style=\"line-height: 24px;\" type = \"submit\" >Change Password</button></form>"
							"</body></html>"
							"<form action=\"/logout\" method=\"POST\"><button style=\"line-height: 24px;\"  type = \"submit\" >Logout</button></form>"
							"</div>"
							"</div>"
							"<div class=\"main-content\">"
							"<div class=\"discuss-header\">"
							"<form action=\"/chat\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><div class=\"button-text\">PennChat</div></div></button></form>"
							"</div>"
							"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"

							"<script>function encodeLedger() {document.getElementsByName(\"members\")[0].value = encodeURIComponent(document.getElementsByName(\"members\")[0].value); return true;}</script>"

							"<div class=\"discuss-new\">"
							"<form accept-charset=\"utf-8\" onsubmit=\"return encodeLedger();\" action=\"/createchat\" method=\"post\">"
							"<div class=\"discuss-new-row\">"
							"<label for=\"members\"></label><input required type=\"text\" name=\"members\" placeholder=\"New Chat Members...\">"
							"<input type=\"submit\" name=\"submit\" value=\"Create\" />"
							"</div>"
							"</form>"
							"</div>"
							"<div class=\"discuss-content\">" + display
							+ "</div></div></div></body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 11, "/createchat") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			// Get stuff from form
			std::string owner = req.cookies["username"];
			std::string decoded_m = decodeURIComponent(req.formData["members"]);
			decoded_m = decodeURIComponent(decoded_m);
			size_t index = 0;
			while (true) {
				index = decoded_m.find("%D", index);
				if (index == std::string::npos)
					break;
				decoded_m.replace(index, 2, "\r");
				index += 2;
			}
			std::string members_raw = owner + "; " + decoded_m;
			std::replace(members_raw.begin(), members_raw.end(), '+', ' ');
			std::replace(members_raw.begin(), members_raw.end(), ',', ';');
			log("MEMBER RAW: " + members_raw);
			std::deque < std::string > members = split(members_raw, ";");
			std::set < std::string > members_polished;
			std::string chathash = generateStringHash(
					req.formData["members"] + std::to_string(time(NULL)));

			// Add the chatbox to owners row
			putKVS(req.cookies["sessionid"], owner, chathash, "");

			for (std::string m : members) {
				std::string member = trim(m);
				if (member.compare("") == 0)
					continue;
				member[0] = std::toupper(member[0]);
				if (!std::isalnum(member[0]))
					continue;

				for (std::size_t i = 1; i < member.length(); ++i) {
					member[i] = std::tolower(member[i]);
					if (!std::isalnum(member[i]))
						continue;
				}
				resp_tuple raw_message = getKVS(req.cookies["sessionid"],
						member, "chats");
				if (kvsResponseStatusCode(raw_message) != 0)
					continue;
				members_polished.insert(member);
				log("MEMBERS POLISHED: " + member);
			}
			std::string members_polished_str;
			for (std::string m : members_polished)
				members_polished_str += m + ", ";
			members_polished_str.pop_back();
			members_polished_str.pop_back();
			for (std::string m : members_polished) {
				std::string member = trim(m);
				std::string cont = members_polished_str + "\t" + owner + "\t"
						+ chathash + "\n";

				log("CHAT MEMBER : " + member);
				std::string my_message = cont;
				std::string old_message = "";
				int resp_code = -1, timeout_count = 0;
				while (resp_code != 0 && (timeout_count++) < 10) {
					resp_tuple raw_message = getKVS(req.cookies["sessionid"],
							member, "chats");
					if (kvsResponseStatusCode(raw_message) != 0)
						continue;
					old_message = kvsResponseMsg(raw_message);
					std::string new_message = old_message + my_message;
					if (old_message.find(my_message) != std::string::npos)
						break;
					resp_tuple ret = cputKVS(req.cookies["sessionid"], member,
							"chats", old_message, new_message);
					resp_code = kvsResponseStatusCode(ret);
				}
				group_to_clients[chathash].insert(member);
			}

			resp.status = "Temporary Redirect";
			resp.status_code = 307;
			resp.headers["Location"] = "/joinchat/" + owner + "/" + chathash;
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 9, "/joinchat") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.status_code = 200;
			resp.status = "OK";
			resp.headers["Content-type"] = "text/html";

			// Get chatroom from KVS
			auto tokens = split(req.filepath, "/");
			std::string chathash = tokens.back();
			tokens.pop_back();
			std::string owner = trim(tokens.back());
			tokens.pop_back();

			// check if user belongs
			resp_tuple check_raw = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "chats");
			if (group_to_clients.find(chathash) != group_to_clients.end()
					&& group_to_clients[chathash].find(req.cookies["username"])
							!= group_to_clients[chathash].end()) {
				log("valid user for group " + chathash);
			} else if (kvsResponseStatusCode(check_raw) != 0
					|| kvsResponseMsg(check_raw).find(chathash)
							== std::string::npos) {
				log("Not valid user for group " + chathash);
				resp.status = "Temporary Redirect";
				resp.status_code = 307;
				resp.headers["Location"] = "/chat";
			}
			resp_tuple chatroom_raw = getKVS(req.cookies["sessionid"], owner,
					chathash);
			if (kvsResponseStatusCode(chatroom_raw) != 0) {
				resp.status = "Temporary Redirect";
				resp.status_code = 307;
				resp.headers["Location"] = "/chat";
				return resp;
			}

			client_to_group[req.cookies["username"]] = chathash;
			std::string chatroom = kvsResponseMsg(chatroom_raw);
			//std::replace(chatroom.begin(), chatroom.end(), '+', ' ');
			//deliverable_messages[req.cookies["username"]][chathash] = chatroom;
			//std::replace(chatroom.begin(), chatroom.end(), '+', ' ');

			/*resp.content =
			 "<head><meta charset=\"UTF-8\"></head>"
			 "<html><body "
			 "style=\"display:flex;flex-direction:column;height:100%;padding:10px;\">"
			 "<div style=\"display:flex; flex-direction: row;\"><form style=\"padding-left:15px; padding-right:15px; margin-bottom:18px;\" action=\"/chat\" method=\"POST\"> <input style=\"line-height:24px;\" type = \"submit\" value=\"Chat\" /></form>"
			 "<form action=\"/leavechat/" + owner + "/"
			 + chathash
			 + "\" method=\"POST\" style=\"margin-bottom:18px;\"> <input style=\"line-height:24px;\" type = \"submit\" value=\"Leave Chat\"/></form>"
			 "</div><ul id=\"message-body\">" + chatroom
			 + "</ul>"
			 "<form action=\"/sendchatmessage/" + owner
			 + "/" + chathash
			 + "\" method=\"POST\" style=\"margin-bottom:18px;\"> <input style=\"line-height:24px;\" type = \"submit\" value=\"Send\"/>"
			 "<label for=\"message\">Type something</label><input required type=\"text\" name=\"message\"/>"
			 "</form><script type=\"text/javascript\" src=\"https://ajax.googleapis.com/ajax/libs/jquery/3.5.1/jquery.min.js\"></script>"
			 "<script type=\"text/javascript\" src=\"https://drive.google.com/uc?export=view&id=1WYBufNkkfQD734AhDtBWZ7sSux-RuXZl\"></script></body></html>";*/

			std::string message =
					"<html><head><meta charset=\"UTF-8\"><link rel=\"stylesheet\" href=\"https://drive.google.com/uc?export=view&id=1PoIbJlTGlxAi24J7s5J8aRViWVW1XHgh\"></head><body><div class=\"total-wrapper\">"
							"<div class=\"nav\">"
							"<div class=\"nav-title\"><form action=\"/dashboard\" method=\"POST\"><button   type = \"submit\" >PennCloud</button></form></div>"
							"<div class=\"nav-right\">"
							"<form action=\"/change-password\" method=\"POST\"><button style=\"line-height: 24px;\" type = \"submit\" >Change Password</button></form>"
							"</body></html>"
							"<form action=\"/logout\" method=\"POST\"><button style=\"line-height: 24px;\"  type = \"submit\" >Logout</button></form>"
							"</div>"
							"</div>"
							"<div class=\"main-content\">";
			message +=
					"<script>if ( window.history.replaceState ) {window.history.replaceState( null, null, window.location.href );}</script>"
							"<script>function encodeMessage() {document.getElementsByName(\"message\")[0].value = encodeURIComponent(document.getElementsByName(\"message\")[0].value); return true;}</script>"
							"<div class=\"discuss-header\">"
							"<form action=\"/chat\" method=\"POST\"> <button type=\"submit\" class=\"btn btn-success\"><div class=\"button-content\"><div class=\"button-text\">PennChat</div></div></button></form>"
							//"<div class=\"forum-name\">"
							/*"<form action=\"/leavechat/"
							 + owner + "/" + chathash
							 + "\" method=\"POST\"> <input type = \"submit\" value=\"Leave Chat\"/></form>""</div>"*/
							"</div>"
							"<div class=\"discuss-content\">"
							//message += displayAllLedgerMessages(ledgerHash);
							//message +=
							"<ul id=\"message-body\">" + chatroom
							+ "</ul>"
									"</div>"
									"<div class=\"discuss-new\">"
									"<form accept-charset=\"utf-8\" onsubmit=\"return encodeMessage();\" action=\"/sendchatmessage/"
							+ owner + "/" + chathash
							+ "\" method=\"post\">"
									"<div class=\"discuss-new-row\">"
									"<label for=\"message\"></label><input required type=\"text\" name=\"message\" placeholder=\"Type a Message...\">"
									"<input type=\"submit\" name=\"submit\" value=\"Send\" />"
									"</div>"
									"</form>"
									"</div>"
									"</div>";
			message +=
					"</div><script type=\"text/javascript\" src=\"https://ajax.googleapis.com/ajax/libs/jquery/3.5.1/jquery.min.js\"></script>"
							"<script type=\"text/javascript\" src=\"https://drive.google.com/uc?export=view&id=1WYBufNkkfQD734AhDtBWZ7sSux-RuXZl\"></script></body></html>";
			resp.content = message;
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 16, "/sendchatmessage") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			// change this to javascript friendly route
			resp.status = "Temporary Redirect";
			resp.status_code = 307;

			// Get chatroom from KVS
			std::string message = req.formData["message"];
			message = decodeURIComponent(message);
			message = decodeURIComponent(message);
			size_t index = 0;
			while (true) {
				index = message.find("%D", index);
				if (index == std::string::npos)
					break;
				message.replace(index, 2, "\r");
				index += 2;
			}
			auto tokens = split(req.filepath, "/");
			std::string chathash = tokens.back();
			tokens.pop_back();
			std::string owner = trim(tokens.back());
			tokens.pop_back();
			// check if user belongs
			resp_tuple check_raw = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "chats");
			if (group_to_clients.find(chathash) != group_to_clients.end()
					&& group_to_clients[chathash].find(req.cookies["username"])
							!= group_to_clients[chathash].end()) {
				log("valid user for group " + chathash);
			} else if (kvsResponseStatusCode(check_raw) != 0
					|| kvsResponseMsg(check_raw).find(chathash)
							== std::string::npos) {
				log("Not valid user for group " + chathash);
				resp.status = "Temporary Redirect";
				resp.status_code = 307;
				resp.headers["Location"] = "/chat";
			}
			resp.headers["Location"] = "/joinchat/" + owner + "/" + chathash;

			chatMulticast(req.cookies["username"], message, chathash, owner);
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 10, "/leavechat") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			auto tokens = split(req.filepath, "/");
			std::string chathash = tokens.back();
			tokens.pop_back();
			std::string owner = trim(tokens.back());
			tokens.pop_back();
			/// check if user belongs
			resp_tuple check_raw = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "chats");
			if (group_to_clients.find(chathash) != group_to_clients.end()
					&& group_to_clients[chathash].find(req.cookies["username"])
							!= group_to_clients[chathash].end()) {
				log("valid user for group " + chathash);
			} else if (kvsResponseStatusCode(check_raw) != 0
					|| kvsResponseMsg(check_raw).find(chathash)
							== std::string::npos) {
				log("Not valid user for group " + chathash);
				resp.status = "Temporary Redirect";
				resp.status_code = 307;
				resp.headers["Location"] = "/chat";
			}

			resp_tuple getResp = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "chats");
			std::string getRespMsg = kvsResponseMsg(getResp);
			std::stringstream ss(getRespMsg);
			std::string chatroom;
			std::string new_chats = "";
			if (getRespMsg != "") {
				while (std::getline(ss, chatroom, '\n')) {
					if (chatroom.find(chathash) != std::string::npos)
						continue;
					new_chats += chatroom;
				}
				putKVS(req.cookies["sessionid"], req.cookies["username"],
						"chats", new_chats);
			}
			group_to_clients[chathash].erase(req.cookies["username"]);
			client_to_group.erase(req.cookies["username"]);
			chatMulticast(req.cookies["username"],
					req.cookies["username"] + "+left+the+group", chathash,
					owner);
			resp.status = "Temporary Redirect";
			resp.status_code = 307;
			resp.headers["Location"] = "/chat";
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else if (req.filepath.compare(0, 18, "/updatechatmessage") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			log("HERE UPDATE CHAT MESSAGE");
			resp.status = "OK";
			resp.status_code = 200;
			resp.content = "";
			std::string user = req.cookies["username"];
			auto tokens = split(req.filepath, "/");
			std::string chathash = tokens.back();
			tokens.pop_back();
			std::string owner = trim(tokens.back());
			tokens.pop_back();
			// check if user belongs
			resp_tuple check_raw = getKVS(req.cookies["sessionid"],
					req.cookies["username"], "chats");
			if (group_to_clients.find(chathash) != group_to_clients.end()
					&& group_to_clients[chathash].find(req.cookies["username"])
							!= group_to_clients[chathash].end()) {
				log("valid user for group " + chathash);
			} else if (kvsResponseStatusCode(check_raw) != 0
					|| kvsResponseMsg(check_raw).find(chathash)
							== std::string::npos) {
				log("Not valid user for group " + chathash);
				resp.status = "Temporary Redirect";
				resp.status_code = 307;
				resp.headers["Location"] = "/chat";
			}

			if (deliverable_messages.find(user) != deliverable_messages.end()
					&& deliverable_messages[user].find(chathash)
							!= deliverable_messages[user].end()) {
				log(
						"Update chat message hit: "
								+ deliverable_messages[user][chathash]);
				resp.content = deliverable_messages[user][chathash];
			} else {
				log("No luck! Getting from KVS now");
				resp_tuple ret = getKVS(req.cookies["sessionid"], owner,
						chathash);
				if (kvsResponseStatusCode(ret) == 0) {
					deliverable_messages[user][chathash] = kvsResponseMsg(ret);
					resp.content = deliverable_messages[user][chathash];
					log("Found: " + deliverable_messages[user][chathash]);
				}
			}
			//std::replace(resp.content.begin(), resp.content.end(), '+', ' ');
			resp.headers["Content-type"] = "text/plain";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/";
		}
	} else {
		log(std::to_string(__LINE__));
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
// log("Sent: " + response);
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
							for (int j = i - 15; j < i - 1; j++) {
								if (tolower(buf[j]) != penncloud[checkIndex])
									validDomain = false;
								checkIndex++;
							}
							if (validDomain) {
								// Successful RCPT command.
								recipient = "";
								for (int j = 9; j < i - 15; j++) {
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
	if (pthread_mutex_init(&access_fifo_seq_num, NULL) != 0)
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

// Hardcoding Paxos server addresses
	paxosServers.push_back("127.0.0.1:10030");
	paxosServers.push_back("127.0.0.1:10032");
	paxosServers.push_back("127.0.0.1:10034");
	paxosServersHeartbeatMap["127.0.0.1:10030"] = "127.0.0.1:10031";
	paxosServersHeartbeatMap["127.0.0.1:10032"] = "127.0.0.1:10033";
	paxosServersHeartbeatMap["127.0.0.1:10034"] = "127.0.0.1:10035";

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
		if (load_balancer)
			this_server_state.http_address = split(trim(std::string(buffer)),
					",").at(0);
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
	} else { // if config file not given, i.e., stand alone frontend
		std::string local_internal_address = "127.0.0.1";
		sockaddr_in internal_server;
		internal_server.sin_family = AF_INET;
		internal_server.sin_port = htons(internal_port_no);
		internal_server.sin_addr.s_addr = inet_addr(
				local_internal_address.data());
		frontend_internal_list.push_back(internal_server);

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
