#include <algorithm>
#include <arpa/inet.h>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
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

std::string greeting = "+OK Server ready (Author: Prasanna Poudyal / poudyal)\r\n";
std::string goodbye = "+OK Goodbye!\r\n";
std::string error_msg = "-ERR Server shutting down\r\n";
std::string unknown_cmd = "-ERR Unknown command\r\n";
std::string kvs_addr = "";
std::string mail_addr = "";
std::string storage_addr = "";
std::vector<pthread_t> pthread_ids;
pthread_mutex_t fd_mutex;
std::set<int *> fd;
volatile bool verbose = false;
volatile bool shut_down = false;

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
       std::deque<char> file;
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

void log(std::string str) {
       if (verbose)
              std::cerr << str << "\n";
}

void readNBytes(int *client_fd, int n, char *buffer) {
       if (n == 0)
              return;
       int message_read = 0;
       while (message_read < n) {
              int rlen = read(*client_fd, &buffer[message_read], n - message_read);
              message_read += rlen;
       }
}

// TODO: confirm that the kvs server returns "+OK" or "-ERR" (ignoring them for now)
// Returns content from KVS response (anything following length,)
std::string readKVSResponse(int *client_fd) {
       bool lengthUnknown = true;
       long contentLength = 100;
       int message_read = 0;
       char buffer[100] = "";
       char *responseAfterComma = NULL;
       while (lengthUnknown || message_read < contentLength) {
              int rlen = 0;
              if (lengthUnknown) {
                     rlen = read(*client_fd, &buffer[message_read], 100 - message_read);
              } else {
                     rlen = read(*client_fd, responseAfterComma + message_read, contentLength - message_read);
              }
              message_read += rlen;
              char *firstComma = strstr(buffer, ",");
              if (firstComma != NULL) {
                     char *firstSpace = strstr(buffer, " ");
                     lengthUnknown = false;
                     char *numStr = strndup(firstSpace + 1, firstComma - firstSpace);
                     contentLength = strtol(numStr, NULL, 10) + (firstComma - buffer);
                     responseAfterComma = (char *)malloc(sizeof(char) * contentLength);
                     strncpy(responseAfterComma, firstComma + 1, (buffer + message_read) - firstComma);
                     message_read = (buffer + message_read) - firstComma; // what i've read of content so far
              }
       }
       std::string finalResponseToReturn(responseAfterComma);
       free(responseAfterComma);
       // EXCLUDES OK OR ERR! TODO add this in
       log("readKVSResponse: " + finalResponseToReturn);
       return finalResponseToReturn;
}

char *readIncomingCmdKVS(int *client_fd) {
       bool lengthUnknown = true;
       long contentLength = 100;
       int message_read = 0;
       char buffer[100] = "";
       char *finalCmd = NULL;
       while (lengthUnknown || message_read < contentLength) {
              int rlen = 0;
              if (lengthUnknown) {
                     rlen = read(*client_fd, &buffer[message_read], 100 - message_read);
              } else {
                     rlen = read(*client_fd, finalCmd + message_read, contentLength - message_read);
              }
              message_read += rlen;
              char *firstComma = strstr(buffer, ",");
              if (firstComma != NULL) {
                     char *firstSpace = strstr(buffer, " ");
                     lengthUnknown = false;
                     char *numStr = strndup(firstSpace + 1, firstComma - firstSpace);
                     contentLength = strtol(numStr, NULL, 10) + (firstComma - buffer);
                     finalCmd = (char *)malloc(sizeof(char) * contentLength);
                     strncpy(finalCmd, buffer, message_read);
              }
       }
       return finalCmd;
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
       str.erase(str.begin(), std::find_if(str.begin(), str.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
       str.erase(std::find_if(str.rbegin(), str.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(),
                 str.end());
       return str;
}

std::string lower(std::string str) {
       std::string lower = str;
       std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
       return lower;
}

std::string getLineAndDelete(std::string &str) {
       std::string delimiter = "\n";
       size_t pos = str.find(delimiter);
       std::string ret = pos == std::string::npos ? std::string(str) : std::string(str.substr(0, pos));
       if (pos != std::string::npos) {
              str.erase(0, pos + delimiter.length());
       } else {
              str.clear();
       }
       return ret;
}

void removeQuotes(std::string &str) { str.erase(remove(str.begin(), str.end(), '\"'), str.end()); }

int getPortNoFromString(std::string fullServAddr) {
       int port = 0;
       try {
              port = stoi(split(trim(fullServAddr), ":")[1]);
       } catch (const std::invalid_argument &ia) {
              log("Port not found! returning 0");
       }
       return port;
}

std::string getAddrFromString(std::string fullServAddr) { return trim(split(fullServAddr, ":")[0]); }

int connectToServer(std::string fullServAddress) {
       int socketFD = socket(PF_INET, SOCK_STREAM, 0);
       if (socketFD < 0) {
              fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
              exit(1);
       }
       struct sockaddr_in servaddr;
       bzero(&servaddr, sizeof(servaddr));
       servaddr.sin_family = AF_INET;
       int serverPortNo = getPortNoFromString(fullServAddress);
       servaddr.sin_port = htons(serverPortNo);
       std::string servAddress = getAddrFromString(fullServAddress);
       inet_pton(AF_INET, servAddress.c_str(), &(servaddr.sin_addr));
       connect(socketFD, (struct sockaddr *)&servaddr, sizeof(servaddr));
       return socketFD;
}

/*********************** KVS Util function ***********************************/

std::string putKVS(std::string row, std::string column, std::string value) {
       int kv_server = connectToServer(kvs_addr);
       int cmdLength = row.size() + column.size() + value.size() + 2; // +2 for commas between row,col,val
       std::string message = "PUT " + std::to_string(cmdLength) + "," + row + "," + column + "," + value;
       writeNBytes(&kv_server, message.size(), message.data());
       // Response
       std::string response = readKVSResponse(&kv_server);
       close(kv_server);
       return response;
}

std::string getKVS(std::string row, std::string column) {
       int kv_server = connectToServer(kvs_addr);

       // Request
       int cmdLength = row.size() + column.size() + 1; // +1 for comma between row and col
       std::string request = "GET " + std::to_string(cmdLength) + "," + row + "," + column;
       writeNBytes(&kv_server, request.size(), request.data());

       // Response
       std::string response = readKVSResponse(&kv_server);
       /*
       std::string value = "";
       if (response.find("+OK") != std::string::npos) {
              value = trim(split(response, " ")[2]);
       }
       return value;
       */
       close(kv_server);
       return response;
}

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

       while (trim((segment = content.substr(0, content.find(boundary)))).compare("") != 0) {
              content.erase(0, content.find(boundary) + boundary.length());
              std::string line = "", fieldname = "", filename = "";
              bool segment_is_file = false;
              while (trim((line = getLineAndDelete(segment))).compare("") != 0) {
                     if (line.find("application/octet-stream") != std::string::npos)
                            segment_is_file = true;
                     if (lower(line).find("content-disposition") != std::string::npos) {
                            std::deque<std::string> data = split((split(line, ":")[1]), ";");
                            for (std::string d : data) {
                                   if (d.find("filename") != std::string::npos) {
                                          filename = trim(split(d, "=")[1]);
                                          removeQuotes(filename);
                                   } else if (d.find("name") != std::string::npos) {
                                          fieldname = trim(split(d, "=")[1]);
                                          removeQuotes(fieldname);
                                   }
                            }
                     }
              }
              segment = (segment.find("--") == std::string::npos) ? segment : segment.substr(0, segment.find("--"));
              if (segment_is_file) {
                     for (char c : segment) {
                            req.file.push_back(c);
                     }
                     req.formData["filename"] = filename;
              } else if (fieldname.compare("") != 0) {
                     req.formData[fieldname] = trim(segment);
              }
       }

       log("Results of multi-part processing: ");
       log("File: " + std::string(req.file.begin(), req.file.end()));
       log("Form data: ");
       for (std::map<std::string, std::string>::iterator it = req.formData.begin(); it != req.formData.end(); it++) {
              log("Key : " + it->first + " value : " + it->second);
       }
}

void processForm(struct http_request &req) {
       std::deque<std::string> queryPairs = split(req.content, "&");
       for (std::string pair : queryPairs) {
              size_t pos = pair.find("=");
              std::string key = (pos == std::string::npos) ? pair : pair.substr(0, pos);
              std::string value =
                  (pos == std::string::npos) ? "" : ((pos + 1 < pair.length()) ? pair.substr(pos + 1) : "");

              req.formData[key] = value;
       }

       log("Form data: ");
       for (std::map<std::string, std::string>::iterator it = req.formData.begin(); it != req.formData.end(); it++) {
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
              std::string key = (pos == std::string::npos) ? cookie : trim(split(cookie, "=").at(0));
              std::string value = (pos == std::string::npos) ? "" : trim(split(cookie, "=").at(1));
              req.cookies[trim(key)] = trim(value);
       }
       req.headers.erase("cookie");
}

std::string readLines(int *client_fd) {
       struct timeval timeout;
       timeout.tv_sec = 10;
       timeout.tv_usec = 0;
       if (setsockopt(*client_fd, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) < 0)
              log("setsockopt failed\n");

       char buffer[1001];
       bzero(buffer, 1000);
       int message_size = 0;

       /* Read until carriage return */
       while (message_size < 1000 && strstr(buffer, "\n") == NULL) {
              int curr_bytes = read(*client_fd, &buffer[message_size], 1000 - message_size);
              if (curr_bytes <= 0) {
                     return "";
              }
              message_size += curr_bytes;
       }

       buffer[message_size] = 0;
       return std::string(buffer);
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
                     content_length = stoi(req.headers["content-length"]) - req.content.size();
                     log("Trying to read content of length " + req.headers["content-length"]);
                     char buffer[content_length + 1];
                     readNBytes(client_fd, content_length, buffer);
                     buffer[content_length] = 0;
                     req.content += std::string(buffer);
              } catch (const std::invalid_argument &ia) {
                     log("Invalid number: " + req.headers["content-length"]);
                     return req;
              }
       }

       log("Contents: \n" + req.content);
       log("Actual content size: " + std::to_string(req.content.size()));

       // Process form or file if necessary
       if (req.headers.find("content-type") != req.headers.end()) {
              if (req.headers["content-type"].find("multipart/form-data") != std::string::npos)
                     processMultiPart(req);
              if (req.headers["content-type"].find("application/x-www-form-urlencoded") != std::string::npos)
                     processForm(req);
       }
       return req;
}

struct http_response processRequest(struct http_request &req) {
       struct http_response resp;
       for (std::map<std::string, std::string>::iterator it = req.cookies.begin(); it != req.cookies.end(); it++) {
              resp.cookies[it->first] = it->second;
       }

       if (req.filepath.compare("/") == 0) {
              resp.status_code = 200;
              resp.status = "OK";
              resp.headers["Content-type"] = "text/html";
              if (req.cookies.find("error") != req.cookies.end()) {
                     printf("here\n");
              }
              resp.content =
                  "<html><body "
                  "style=\"display:flex;flex-direction:column;height:100%;align-items:center;justify-content:"
                  "center;\">"
                  "<form id=\"login\" action=\"/login\" enctype=\"multipart/form-data\" method=\"POST\""
                  "<label for =\"username\">Username:</label><br/><input name=\"username\" type=\"text\"/><br/>"
                  "<label for=\"password\">Password:</label><br/><input name=\"password\" "
                  "type=\"password\"/><br/>"
                  "<br/><input type=\"submit\" name=\"submit\" value=\"Log In\"><br/>"
                  "</form>"
                  "<form id=\"signup\" style=\"display:none;\" action=\"/signup\" "
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
              resp.headers["Content-length"] = std::to_string(resp.content.size());
       } else if (req.filepath.compare("/login") == 0) {
              resp.status_code = 307;
              resp.status = "Temporary Redirect";
              resp.headers["Location"] = "/";
              resp.cookies["error"] = "1";
              // check KVS. redirect to dashboard if successful
              // set cookies
              // if not successful, report error and redirect to login page
       } else if (req.filepath.compare("/signup") == 0) {
              // try to add to KVS. if successful, login and redirect to dashboard
              // set cookies
              // if not successful, report error and redirect to signup page
       } else {
              resp.status_code = 404;
              resp.status = "Not Found";
       }
       return resp;
}

void sendResponseToClient(struct http_response &resp, int *client_fd) {
       std::string response;
       response = "HTTP/1.0 " + std::to_string(resp.status_code) + " " + resp.status + "\r\n";
       response += "Connection: close\r\n";
       for (std::map<std::string, std::string>::iterator it = resp.headers.begin(); it != resp.headers.end(); it++) {
              response += it->first + ":" + it->second + "\r\n";
       }
       if (resp.cookies.size() > 0) {
              response += "Set-cookie: ";
              for (std::map<std::string, std::string>::iterator it = resp.cookies.begin(); it != resp.cookies.end();
                   it++) {
                     response += it->first + "=" + it->second + ";";
              }
       }
       response += "\r\n";
       if (resp.content.compare("") != 0) {
              response += "\r\n" + resp.content;
       }
       writeNBytes(client_fd, response.size(), response.data());
       log("Sent: " + response);
}

/***************************** End http util functions ************************/

/***************************** Start storage service functions ************************/

int connectToKVSServer() { return connectToServer(kvs_addr); }

void uploadFile(struct http_request req) {
       std::string username = req.formData["username"];
       std::string filename = req.formData["filename"];
       std::string filepath = "ss0_/"; // TODO filepath of file in storage service (no dirs for now)
       std::string fileData(req.file.begin(), req.file.end());

       // Construct filepath of new file
       std::string kvsCol = "ss1_" + filepath.substr(4, 1) + filename;
       // Reading in response to GET --> list of files at filepath
       std::string getCmdResponse = getKVS(username, filepath);
       // Adding new file to file list
       std::string fileList(getCmdResponse);
       fileList += "," + kvsCol;
       // PUT length,row,col,value for MODIFIED FILE LIST
       std::string putCmdResponse = putKVS(username, filepath, fileList);
       // PUT username,kvsCol,filedata
       putCmdResponse = putKVS(username, kvsCol, fileData);
}

/***************************** End storage service functions ************************/

void *handleClient(void *arg) {
       /* Initialize buffer and client fd */
       int *client_fd = (int *)arg;
       log("Handling client " + std::to_string(*client_fd));

       /* Parse request from client */
       struct http_request req = parseRequest(client_fd);
       if (!req.valid) {
              close(*client_fd);
              return NULL;
       }
       if (req.file.size() > 0) {
              // File present to upload
              uploadFile(req);
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
              if (strstr(argv[i], "-v") != NULL && strcmp(strstr(argv[i], "-v"), "-v") == 0) {
                     verbose = true;
              } else if (strstr(argv[i], "-p") != NULL && strcmp(strstr(argv[i], "-p"), "-p") == 0) {
                     if (i + 1 < argc) {
                            port_no = atoi(argv[++i]);
                            if (port_no == 0) {
                                   std::cerr << "Port number is 0 or '-n' is followed by non integer! Using default\n";
                                   port_no = 10000;
                            }
                     } else {
                            std::cerr << "'-p' should be followed by a number! Using port 10000\n";
                     }
              } else if (strstr(argv[i], "-k") != NULL && strcmp(strstr(argv[i], "-k"), "-k") == 0) {
                     if (i + 1 < argc) {
                            kvs_addr = trim(std::string(argv[++i]));
                     } else {
                            std::cerr << "'-k' should be followed by an address!\n";
                     }
              } else if (strstr(argv[i], "-m") != NULL && strcmp(strstr(argv[i], "-m"), "-m") == 0) {
                     if (i + 1 < argc) {
                            mail_addr = trim(std::string(argv[++i]));
                     } else {
                            std::cerr << "'-m' should be followed by an address!\n";
                     }
              } else if (strstr(argv[i], "-s") != NULL && strcmp(strstr(argv[i], "-s"), "-s") == 0) {
                     if (i + 1 < argc) {
                            storage_addr = trim(std::string(argv[++i]));
                     } else {
                            std::cerr << "'-s' should be followed by an address!\n";
                     }
              } else if (strstr(argv[i], "-a") != NULL && strcmp(strstr(argv[i], "-a"), "-a") == 0) {
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
       if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &true_opt, sizeof(int)) < 0) {
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
       if (bind(socket_fd, (struct sockaddr *)&servaddr, sizeof(servaddr)) != 0) {
              std::cerr << "Socket couldn't bind to " << port_no << "\n";
              exit(-1);
       } else if (verbose) {
              std::cerr << "Successfully binded to " << port_no << "\n";
       }

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
              int *client_fd = (int *)malloc(sizeof(int));
              *client_fd = accept(socket_fd, (struct sockaddr *)&client_addr, &clientaddrlen);
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
