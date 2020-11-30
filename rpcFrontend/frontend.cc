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
#include <rpc/client.h>
#include <rpc/rpc_error.h>

std::string greeting =
		"+OK Server ready (Author: Prasanna Poudyal / poudyal)\r\n";
std::string goodbye = "+OK Goodbye!\r\n";
std::string error_msg = "-ERR Server shutting down\r\n";
std::string unknown_cmd = "-ERR Unknown command\r\n";
std::string kvs_addr = "";
std::string mail_addr = "";
std::string storage_addr = "";
std::vector<pthread_t> pthread_ids;
pthread_mutex_t fd_mutex;
std::set<int*> fd;
volatile bool verbose = false;
volatile bool shut_down = false;

using resp_tuple = std::tuple<int, std::string>;

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
void computeDigest(char *data, int dataLengthBytes, unsigned char *digestBuffer) {
    /* The digest will be written to digestBuffer, which must be at least MD5_DIGEST_LENGTH bytes long */
    MD5_CTX c;
    MD5_Init(&c);
    MD5_Update(&c, data, dataLengthBytes);
    MD5_Final(digestBuffer, &c);
}

std::string generateStringHash(std::string strToHash) {
        unsigned char *digestBuff = (unsigned char *)malloc(MD5_DIGEST_LENGTH * sizeof(unsigned char) + 1);
        char* strToHashCStr = strdup(strToHash.c_str());
        computeDigest(strToHashCStr, strToHash.length()+1, digestBuff);
        free(strToHashCStr);
        digestBuff[MD5_DIGEST_LENGTH] = '\0';
        char *stringHash = (char *)malloc((32+1) * sizeof(char));
        for (int i = 0; i < 16; i++) {
            stringHash[2 * i] = "0123456789ABCDEF"[digestBuff[i] / 16];
            stringHash[2 * i + 1] = "0123456789ABCDEF"[digestBuff[i] % 16];
        }
        free(digestBuff);
        stringHash[32] = '\0';
        return std::string(stringHash);

}

void log(std::string str) {
	if (verbose)
		std::cerr << str << "\n";
}

int readNBytes(int *client_fd, int n, char *buffer) {
	if (n == 0)
		return 0;
	int message_read = 0;
	while (message_read < n) {
		int rlen = read(*client_fd, &buffer[message_read], n - message_read);
		if (rlen > 0) message_read += rlen;
		if(rlen <= 0) return message_read;
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
	std::deque<std::string> ret;

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

resp_tuple putKVS(std::string row, std::string column, std::string value) {
	int serverPortNo = getPortNoFromString(kvs_addr);
	std::string servAddress = getAddrFromString(kvs_addr);
    rpc::client kvsRPCClient(servAddress, serverPortNo);
    resp_tuple resp;
    try {
        log("KVS PUT: "+row+", "+column+", "+value);
        resp = kvsRPCClient.call("put", row, column, value).as<resp_tuple>();
        log("putKVS Response Status: "+ std::to_string(std::get<0>(resp)));
        log("putKVS Response Value: "+ std::get<1>(resp));
    } catch (rpc::rpc_error &e) {
        /*
        std::cout << std::endl << e.what() << std::endl;
        std::cout << "in function " << e.get_function_name() << ": ";
        using err_t = std::tuple<std::string, std::string>;
        auto err = e.get_error().as<err_t>();
        */
        log("UNHANDLED ERROR IN putKVS TRY CATCH"); // TODO
    }
    return resp;
}

resp_tuple getKVS(std::string row, std::string column) {
	int serverPortNo = getPortNoFromString(kvs_addr);
	std::string servAddress = getAddrFromString(kvs_addr);
    rpc::client kvsRPCClient(servAddress, serverPortNo);
    using resp_tuple = std::tuple<int, std::string>;
    resp_tuple resp;
    try {
        log("KVS GET: "+row+", "+column);
        resp = kvsRPCClient.call("get", row, column).as<resp_tuple>();
        log("getKVS Response Status: "+ std::to_string(std::get<0>(resp)));
        log("getKVS Response Value: "+ std::get<1>(resp));
    } catch (rpc::rpc_error &e) {
        /*
        std::cout << std::endl << e.what() << std::endl;
        std::cout << "in function " << e.get_function_name() << ": ";
        using err_t = std::tuple<std::string, std::string>;
        auto err = e.get_error().as<err_t>();
        */
        log("UNHANDLED ERROR IN getKVS TRY CATCH"); // TODO
    }
    return resp;
}

resp_tuple deleteKVS(std::string row, std::string column) {
	int serverPortNo = getPortNoFromString(kvs_addr);
	std::string servAddress = getAddrFromString(kvs_addr);
    rpc::client kvsRPCClient(servAddress, serverPortNo);
    using resp_tuple = std::tuple<int, std::string>;
    resp_tuple resp;
    try {
        log("KVS DELETE: "+row+", "+column);
        resp = kvsRPCClient.call("del", row, column).as<resp_tuple>();
        log("deleteKVS Response Status: "+ std::to_string(std::get<0>(resp)));
        log("deleteKVS Response Value: "+ std::get<1>(resp));
    } catch (rpc::rpc_error &e) {
        /*
        std::cout << std::endl << e.what() << std::endl;
        std::cout << "in function " << e.get_function_name() << ": ";
        using err_t = std::tuple<std::string, std::string>;
        auto err = e.get_error().as<err_t>();
        */
        log("UNHANDLED ERROR IN deleteKVS TRY CATCH"); // TODO
    }
    return resp;
}

int kvsResponseStatusCode(resp_tuple resp) {
        return std::get<0>(resp);
}

std::string kvsResponseMsg(resp_tuple resp) {
        return std::get<1>(resp);
}

/***************************** Start storage service functions ************************/

void uploadFile(struct http_request req, std::string filepath) {
	std::string username = req.cookies["username"]; 
	username = "amit"; // TODO change hardcoding
	std::string filename = req.formData["filename"];
	std::string fileData = req.formData["file"];

	// Construct filepath of new file
    std::string filenameHash = generateStringHash(username+filepath+filename);
	std::string kvsCol = "ss1_" + filenameHash;
	// Reading in response to GET --> list of files at filepath
	resp_tuple getCmdResponse = getKVS(username, filepath);
    resp_tuple putCmdResponse;
    if(kvsResponseStatusCode(getCmdResponse) == 0) {
        std::string fileList = kvsResponseMsg(getCmdResponse);
        // Adding new file to existing file list (IF new file!)
        std::string newEntry = filename+","+kvsCol+"\n";
        if(fileList.find(newEntry) == std::string::npos) {
            fileList += newEntry;
            // PUT length,row,col,value for MODIFIED FILE LIST
            putCmdResponse = putKVS(username, filepath, fileList);
        }
        // PUT username,kvsCol,filedata
        putCmdResponse = putKVS(username, kvsCol, fileData);
    }
}

void deleteFile(struct http_request req, std::string containingDir, std::string itemToDeleteHash) {
	std::string username = req.cookies["username"]; 
	username = "amit"; // TODO change hardcoding

	// Reading in response to GET --> list of files at filepath
	resp_tuple getCmdResponse = getKVS(username, containingDir);
    resp_tuple putCmdResponse;
    if(kvsResponseStatusCode(getCmdResponse) == 0) {
        std::string fileList = kvsResponseMsg(getCmdResponse);
        // Removing itemToDelete hash from  existing file list
        size_t hashPos = fileList.find(itemToDeleteHash);
        size_t startLine = fileList.substr(0, hashPos).find_last_of("\n");
        size_t endLine = fileList.find("\n", hashPos);
        if(startLine != std::string::npos) {
            fileList = fileList.replace(startLine+1, endLine-startLine, "");
        }
        // PUT length,row,col,value for MODIFIED FILE LIST
        putCmdResponse = putKVS(username, containingDir, fileList);
    }
    // DELETE username,itemToDeleteHash
    putCmdResponse = deleteKVS(username, itemToDeleteHash);
}

void deleteDirectory(struct http_request req, std::string containingDir, std::string itemToDeleteHash) {
	std::string username = req.cookies["username"]; 
	username = "amit"; // TODO change hardcoding

	// Reading in response to GET --> list of files at filepath
	resp_tuple getCmdResponse = getKVS(username, containingDir);
    resp_tuple putCmdResponse;
    if(kvsResponseStatusCode(getCmdResponse) == 0) {
        std::string fileList = kvsResponseMsg(getCmdResponse);
        // Removing itemToDelete hash from  existing file list
        size_t hashPos = fileList.find(itemToDeleteHash);
        size_t startLine = fileList.substr(0, hashPos).find_last_of("\n");
        size_t endLine = fileList.find("\n", hashPos);
        if(startLine != std::string::npos) {
            fileList = fileList.replace(startLine+1, endLine-startLine, "");
        }
        // PUT length,row,col,value for MODIFIED FILE LIST
        putCmdResponse = putKVS(username, containingDir, fileList);
    }
	resp_tuple recursiveDeleteResp = getKVS(username, itemToDeleteHash);
    int respStatus = kvsResponseStatusCode(recursiveDeleteResp);
    std::string respValue = kvsResponseMsg(recursiveDeleteResp);
    if(respStatus == 0) {
        // DELETE username,itemToDeleteHash
        putCmdResponse = deleteKVS(username, itemToDeleteHash);
        std::deque<std::string> splt = split(respValue, "\n");
        int lineNum = 0;
        for (std::string line : splt) {
            if(line.length() > 0) {
                std::deque<std::string> lineSplt = split(line, ",");
                if(lineNum != 0) {
                    // Delete Child Files or Directories
                    if(lineSplt[1].at(2) == '1') {
                        deleteFile(req, itemToDeleteHash, lineSplt[1]);
                    } else if(lineSplt[1].at(2) == '0') {
                        deleteDirectory(req, itemToDeleteHash, lineSplt[1]);
                    }
                }
                lineNum++;
            }
        }
    }
}

std::string getParentDirLink(std::string fileHash) {
    std::string link="<li>Go back<a href=/files/"+fileHash+">Link</a>";
    return link;
}

std::string getFileLink(std::string fileName, std::string fileHash, std::string containingDirectory) {
    std::string link="<li>"+fileName+"<a download=\""+fileName+"\" href=/files/"+fileHash+">Download</a>";
    link += "<form action=\"/ss_delete\" method=\"post\">"
        "<input type=\"hidden\" name=\"containingDirectory\" value=\""+containingDirectory+"\" />"
        "<input type=\"hidden\" name=\"itemToDelete\" value=\""+fileHash+"\" />"
        "<input type=\"submit\" name=\"submit\" value=\"Delete\" />"
        "</form>";
    return link;
}

std::string getFileList(struct http_request req, std::string filepath) {
	std::string username = req.cookies["username"]; 
	username = "amit"; // TODO change hardcoding
    resp_tuple filesResp = getKVS("amit", filepath);
    int respStatus = kvsResponseStatusCode(filesResp);
    std::string respValue = kvsResponseMsg(filesResp);
    int lineNum = 0;
    if(respStatus == 0) {
        if(respValue.length() == 0) {
            return "This directory is empty.";
        }
        std::string result = "<ul>";
        std::deque<std::string> splt = split(respValue, "\n");
        for (std::string line : splt) {
            if(line.length() > 0) {
                std::deque<std::string> lineSplt = split(line, ",");
                if(lineNum == 0) {
                    // Parent Directory Line
                    if(!(lineSplt[0].compare("ROOT") == 0 && lineSplt[1].compare("ROOT") == 0)) {
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
        if(lineNum <= 1) {
            result+= "<p>This directory is empty</p>";
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

void createDirectory(struct http_request req, std::string filepath, std::string dirName) {
	std::string username = req.cookies["username"]; //TODO change hardcoding
	username = "amit";

	// Construct filepath of new directory
    std::string dirNameHash = generateStringHash(username+filepath+dirName);
	std::string kvsCol = "ss0_" + dirNameHash;
	// Reading in response to GET --> list of files at filepath
	resp_tuple getCmdResponse = getKVS(username, filepath);
    resp_tuple putCmdResponse;
    if(kvsResponseStatusCode(getCmdResponse) == 0) {
        std::string fileList = kvsResponseMsg(getCmdResponse);
        // Adding new directory to existing file list (IF new dir!)
        std::string newEntry = dirName+","+kvsCol+"\n";
        // Adding new file to existing file list
        // disallow creation of duplicate named directories
        if(fileList.find(newEntry) == std::string::npos) {
            fileList += newEntry;
            // PUT length,row,col,value for MODIFIED FILE LIST
            putCmdResponse = putKVS(username, filepath, fileList);
            // PUT new column for new directory
            putCmdResponse = putKVS(username, kvsCol, "PARENT_DIR,"+filepath+"\n");
        }
    }
}

void createRootDirForNewUser(struct http_request req) {
	std::string username = req.cookies["username"];
	username = "amit"; //TODO change hardcoding
    std::string dirNameHash = generateStringHash(username + "/");
    // PUT new column for root directory
	putKVS(username, "ss0_" + dirNameHash, "ROOT,ROOT\n");
}

/***************************** End storage service functions ************************/

/*********************** Http Util function **********************************/
std::string getBoundary(std::string &type) {
	std::deque<std::string> splt = split(type, ";");
	for (std::string potent : splt) {
		if (potent.find("boundary") != std::string::npos) {
			std::string boundary = trim(potent.substr(potent.find("=") + 1));
			return boundary + "\r\n";
		}
	}
	return "";
}

void processMultiPart(struct http_request &req) {
	log("Processing multipart");
	std::string boundary = getBoundary(req.headers["content-type"]);
	std::string content(req.content);
	std::string segment = "";

	while (trim((segment = content.substr(0, content.find(boundary)))).compare(
			"") != 0) {
		content.erase(0, content.find(boundary) + boundary.length());
		std::string line = "", fieldname = "", filename = "";
		bool segment_is_file = false;
		while (trim((line = getLineAndDelete(segment))).compare("") != 0) {
			if (line.find("filename") != std::string::npos)
				segment_is_file = true;
			if (lower(line).find("content-disposition") != std::string::npos) {
				std::deque<std::string> data = split((split(line, ":")[1]),
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
		segment =
				(segment.find("--") == std::string::npos) ?
						segment : segment.substr(0, segment.find("--"));
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
		log("Key : " + it->first + " value : " + it->second);
	}
}

void processForm(struct http_request &req) {
	std::deque<std::string> queryPairs = split(req.content, "&");
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
	std::deque<std::string> cookies = split(req.headers["cookie"], ";");
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
	std::deque<std::string> headr = split(trim(first_line), " ");
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
			char * buffer;
			while ((buffer = (char *) malloc(sizeof(char) * (content_length + 1))) == NULL);
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

struct http_response processRequest(struct http_request &req) {
	struct http_response resp;
	for (std::map<std::string, std::string>::iterator it = req.cookies.begin();
			it != req.cookies.end(); it++) {
		resp.cookies[it->first] = it->second;
	}

	if (req.formData["dir_name"].size() > 0) {
            // File present to upload
            if(req.filepath.substr(0,7).compare("/files/") == 0 && req.filepath.length() > 7) {
                std::string filepath = req.filepath.substr(7);
                createDirectory(req, filepath, req.formData["dir_name"]);
            }
	}

	if (req.formData["file"].size() > 0) {
            // File present to upload
            if(req.filepath.substr(0,7).compare("/files/") == 0 && req.filepath.length() > 7) {
                std::string filepath = req.filepath.substr(7);
                uploadFile(req, filepath);
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
					"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
							"center;\">" + test
							+ "<form id=\"login\" style=\"display:"
							+ (signuperr ? "none" : "block")
							+ ";\" action=\"/login\" enctype=\"multipart/form-data\" method=\"POST\""
									"<label for =\"username\">Username:</label><br/><input name=\"username\" type=\"text\"/><br/>"
									"<label for=\"password\">Password:</label><br/><input name=\"password\" "
									"type=\"password\"/><br/>"
									"<br/><input type=\"submit\" name=\"submit\" value=\"Log In\"><br/>"
									"</form>"
									"<form id=\"signup\" style=\"display:"
							+ (signuperr ? "block" : "none")
							+ ";\" action=\"/signup\" "
									"enctype=\"multipart/form-data\" "
									"method=\"POST\""
									"<label for =\"username\">Username:</label><br/><input name=\"username\" type=\"text\"/><br/>"
									"<label for=\"password\">Password:</label><br/><input name=\"password\" "
									"type=\"password\"/><br/>"
									"<label for=\"confirm_password\">Confirm Password:</label><br/><input "
									"name=\"confirm_password\" "
									"type=\"password\"/><br/>"
									"<br/><input type=\"submit\" name=\"submit\" value=\"Sign Up\"><br/>"
									"</form>"
									"<br/><button id=\"switchButton\" type=\"button\">Don't have an account? Sign up!</button>"
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
			/* resp.content =
			 "<html><body>"
			 "<form action=\"/submitdummy\" enctype=\"multipart/form-data\" method=\"POST\""
			 "<label for =\"username\">Username</label><br/><input name=\"username\" type=\"text\"/><br/>"
			 "<label for=\"password\">Password:</label><br/><input name=\"password\"
			 type=\"password\"/><br/>"
			 "<label for=\"file\">File</label><br/><input type=\"file\" name=\"file\"/><br/>"
			 "<label for=\"submit\">Submit</label><br/><input type=\"submit\" name=\"submit\"><br/>"
			 "</form></body></html>";*/
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/dashboard";
		}
	} else if (req.filepath.compare("/login") == 0) {
		if (req.cookies.find("username") == req.cookies.end()) {
            resp_tuple getResp = getKVS(req.formData["username"], "password");
            std::string getRespMsg = kvsResponseMsg(getResp);
			if (req.formData["username"] == ""
					|| getRespMsg == "") {
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
				resp.headers["Location"] = "/dashboard";
				resp.cookies["username"] = req.formData["username"];
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
            resp_tuple getResp = getKVS(req.formData["username"], "password");
            std::string getRespMsg = kvsResponseMsg(getResp);
            int getRespStatusCode = kvsResponseStatusCode(getResp);
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
			} else if (getRespStatusCode == 0) {
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/";
				resp.cookies["error"] = "User already exists.";
				resp.cookies["signuperr"] = "1";
			} else {
				putKVS(req.formData["username"], "password",
						req.formData["password"]);
				resp.status_code = 307;
				resp.status = "Temporary Redirect";
				resp.headers["Location"] = "/dashboard";
				resp.cookies["username"] = req.formData["username"];
                createRootDirForNewUser(req);
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
            std::string username = "amit"; // TODO change hardcoding
            std::string userRootDir = "ss0_" + generateStringHash(username + "/");
			resp.content =
					"<html><body "
							"style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
							"center;\">"
							"<form action=\"/mail\" method=\"POST\"> <input type = \"submit\" value=\"My Mailbox\" /></form>"
							"<form action=\"/compose\" method=\"POST\"> <input type = \"submit\" value=\"Compose Email\" /></form>"
							"<form action=\"/files/"+userRootDir+"\" method=\"POST\"> <input type = \"submit\" value=\"Storage Service\" /></form>"
							"<form action=\"/logout\" method=\"POST\"><input type = \"submit\" value=\"Logout\" /></form>"
							"</body></html>";
			resp.headers["Content-length"] = std::to_string(
					resp.content.size());
		} else {
			resp.status_code = 307;
			resp.status = "Temporary Redirect";
			resp.headers["Location"] = "/login";
		}
	} else if (req.filepath.compare(0,7,"/files/") == 0) {
            if(req.filepath.length() > 7) {
                    std::string filepath = req.filepath.substr(7);
                    resp_tuple getFileResp = getKVS("amit", filepath); // TODO change hardcoded username
                    if(kvsResponseStatusCode(getFileResp) == 0) {
                            // display list of files if route = directory. else, display file contents
                            if(isFileRouteDirectory(filepath)) {
                                    resp.status_code = 200;
                                    resp.status = "OK";
                                    resp.headers["Content-type"] = "text/html";
                                    std::string fileList = getFileList(req, filepath);
                                    resp.content =
                                    "<html><body>"
                                    ""+fileList+"<br/>"
                                    "<form action=\"/files/"+filepath+"\" enctype=\"multipart/form-data\" method=\"POST\""
                                    "<label for=\"file\">Upload a new File</label><br/><input type=\"file\" name=\"file\"/><br/>"
                                    "<input type=\"submit\" name=\"submit\" value=\"Upload\"><br/>"
                                    "</form>"
                                    "<form action=\"/files/"+filepath+"\" enctype=\"multipart/form-data\" method=\"POST\""
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
                            resp.content =
                            "<html><body>"
                            "Requested file not found!"
                            "</body></html>";
                    }
            } else {
                    resp.status_code = 404;
                    resp.status = "Not found";
                    resp.headers["Content-type"] = "text/html";
                    resp.content =
                    "<html><body>"
                    "Requested file not found!"
                    "</body></html>";
            }
	} else if (req.filepath.compare(0,11,"/ss_delete") == 0) {
        std::string containingDirectory = req.formData["containingDirectory"];
        std::string itemToDelete= req.formData["itemToDelete"];
        if(containingDirectory.length() > 0 && itemToDelete.length() > 0) {
            if(itemToDelete.at(2) == '1') {
                // itemToDelete is a FILE
                deleteFile(req, containingDirectory, itemToDelete);
            } else if(itemToDelete.at(2) == '0') {
                // itemToDelete is a DIRECTORY
                // Recursively delete all subdirectories and files
                deleteDirectory(req, containingDirectory, itemToDelete);
            }
            resp.status_code = 307;
            resp.status = "Temporary Redirect";
            resp.headers["Location"] = "/files/"+containingDirectory;
        }
	} else if (req.filepath.compare("/logout") == 0) {
		if (req.cookies.find("username") != req.cookies.end()) {
			resp.cookies.erase("username");
		}
		resp.status_code = 307;
		resp.status = "Temporary Redirect";
		resp.headers["Location"] = "/";
	} else {
		resp.status_code = 404;
		resp.status = "Not Found";
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
	if (resp.content.compare("") != 0) {
		response += "\r\n" + resp.content;
	}
	writeNBytes(client_fd, response.size(), response.data());
	log("Sent: " + response);
}

/***************************** End http util functions ************************/

void* handleClient(void *arg) {
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
	return NULL;
}

int main(int argc, char *argv[]) {
	/* Set signal handler */
	signal(SIGINT, sigint_handler);

	/* Initialize mutex to access fd set */
	if (pthread_mutex_init(&fd_mutex, NULL) != 0)
		log("Couldn't initialize mutex for fd set");

	/* Parse command line args */
	int port_no = 10000;
	for (int i = 0; i < argc; i++) {
		if (strstr(argv[i], "-v") != NULL
				&& strcmp(strstr(argv[i], "-v"), "-v") == 0) {
			verbose = true;
		} else if (strstr(argv[i], "-p") != NULL
				&& strcmp(strstr(argv[i], "-p"), "-p") == 0) {
			if (i + 1 < argc) {
				port_no = atoi(argv[++i]);
				if (port_no == 0) {
					std::cerr
							<< "Port number is 0 or '-n' is followed by non integer! Using default\n";
					port_no = 10000;
				}
			} else {
				std::cerr
						<< "'-p' should be followed by a number! Using port 10000\n";
			}
		} else if (strstr(argv[i], "-k") != NULL
				&& strcmp(strstr(argv[i], "-k"), "-k") == 0) {
			if (i + 1 < argc) {
				kvs_addr = trim(std::string(argv[++i]));
			} else {
				std::cerr << "'-k' should be followed by an address!\n";
			}
		} else if (strstr(argv[i], "-m") != NULL
				&& strcmp(strstr(argv[i], "-m"), "-m") == 0) {
			if (i + 1 < argc) {
				mail_addr = trim(std::string(argv[++i]));
			} else {
				std::cerr << "'-m' should be followed by an address!\n";
			}
		} else if (strstr(argv[i], "-s") != NULL
				&& strcmp(strstr(argv[i], "-s"), "-s") == 0) {
			if (i + 1 < argc) {
				storage_addr = trim(std::string(argv[++i]));
			} else {
				std::cerr << "'-s' should be followed by an address!\n";
			}
		} else if (strstr(argv[i], "-a") != NULL
				&& strcmp(strstr(argv[i], "-a"), "-a") == 0) {
			std::cerr << "Full name: Prasanna Poudyal\nSEAS login: poudyal\n";
			exit(0);
		}
	}

	/* Initialize socket */
	int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (socket_fd < 0) {
		std::cerr << "Socket failed to initialize\n";
		exit(-1);
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
		std::cerr << "Socket couldn't bind to " << port_no << "\n";
		exit(-1);
	} else if (verbose) {
		std::cerr << "Successfully binded to " << port_no << "\n";
	}

    // connectToRPCServer(kvs_addr);

	/* Start listening */
	if (listen(socket_fd, 20) != 0) {
		std::cerr << "Listening failed!\n";
		exit(-1);
	} else if (verbose) {
		std::cerr << "Successfully started listening!\n";
	}

	while (!shut_down) {
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
			break;
		} else {
			pthread_t pthread_id;
			pthread_create(&pthread_id, NULL, handleClient, client_fd);
			if (shut_down) {
				write(*client_fd, error_msg.data(), error_msg.size());
				close(*client_fd);
				free(client_fd);
			} else {
				if (verbose)
					std::cerr << "[" << *client_fd << "] New connection\n";
				pthread_ids.push_back(pthread_id);
				pthread_mutex_lock(&fd_mutex);
				fd.insert(client_fd);
				pthread_mutex_unlock(&fd_mutex);
			}
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
