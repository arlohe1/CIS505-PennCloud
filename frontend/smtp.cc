#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <map>
#include <vector>
#include <fstream>
#include <dirent.h>
#include <sys/file.h>
#include <time.h>
#include <errno.h>
#include <sstream>

volatile bool sigcaught = false;
std::map<pthread_t, int> threads;
pthread_mutex_t threadsMutex;
int listenFD;
int vflag;
std::string directory;
std::map<std::string, pthread_mutex_t> mailboxes;
std::string kvsAddress;
int kvsPort;

/************************************************************************************/

void log(std::string str) {
	if (vflag)
		fprintf(stderr, "%s\n", str.c_str());
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
	std::string response("");
	while (lengthUnknown || message_read < contentLength) {
		int rlen = 0;
		if (lengthUnknown) {
			rlen = read(*client_fd, &buffer[message_read], 100 - message_read);
		} else {
			char temp[1001];
			int nBytes =
					1000 < (contentLength - message_read) ?
							1000 : (contentLength - message_read);
			rlen = read(*client_fd, temp, nBytes);
			temp[rlen] = '\0';
			response += std::string(temp);
		}
		message_read += rlen;
		if (lengthUnknown) {
			char *firstComma = strstr(buffer, ",");
			if (firstComma != NULL) {
				char *firstSpace = strstr(buffer, " ");
				lengthUnknown = false;
				char *numStr = strndup(firstSpace + 1,
						firstComma - firstSpace - 1);
				contentLength = strtol(numStr, NULL, 10);
				response = std::string(buffer);
			}
			contentLength += firstComma - buffer + 1;
		}
	}
	log("Response From Server: " + response);
	std::string finalResponse = response;
	if (response.find(",") != std::string::npos) {
		finalResponse = response.substr(response.find(",") + 1);
	}
	// EXCLUDES OK OR ERR! TODO add this in
	log("Parsed Response from readKVSResponse(): " + finalResponse);
	return finalResponse;
}

char* readIncomingCmdKVS(int *client_fd) {
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
			rlen = read(*client_fd, finalCmd + message_read,
					contentLength - message_read);
		}
		message_read += rlen;
		char *firstComma = strstr(buffer, ",");
		if (firstComma != NULL) {
			char *firstSpace = strstr(buffer, " ");
			lengthUnknown = false;
			char *numStr = strndup(firstSpace + 1, firstComma - firstSpace);
			contentLength = strtol(numStr, NULL, 10) + (firstComma - buffer);
			finalCmd = (char*) malloc(sizeof(char) * contentLength);
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

int connectToServer() {
	int socketFD = socket(PF_INET, SOCK_STREAM, 0);
	if (socketFD < 0) {
		fprintf(stderr, "Cannot open socket (%s)\n", strerror(errno));
		exit(1);
	}
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(kvsPort);
	inet_pton(AF_INET, kvsAddress.c_str(), &(servaddr.sin_addr));
	connect(socketFD, (struct sockaddr*) &servaddr, sizeof(servaddr));
	return socketFD;
}

/*********************** KVS Util function ***********************************/

std::string putKVS(std::string row, std::string column, std::string value) {
	int kv_server = connectToServer();
	int cmdLength = row.size() + column.size() + value.size() + 2; // +2 for commas between row,col,val
	std::string message = "PUT " + std::to_string(cmdLength) + "," + row + ","
			+ column + "," + value;
	writeNBytes(&kv_server, message.size(), message.data());
	// Response
	std::string response = readKVSResponse(&kv_server);
	close(kv_server);
	return response;
}

std::string getKVS(std::string row, std::string column) {
	int kv_server = connectToServer();

	// Request
	int cmdLength = row.size() + column.size() + 1; // +1 for comma between row and col
	std::string request = "GET " + std::to_string(cmdLength) + "," + row + ","
			+ column;
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

/************************************************************************************/

// Handler for SIGINT (executed in main thread as all worker threads block SIGINT).
// Acquires mutex for threads map, sends SIGUSR1 signal to all active threads, and joins threads.
static void sigint_handler(int signum) {
	sigcaught = true;
	pthread_mutex_lock(&threadsMutex);
	std::map<pthread_t, int>::iterator it;
	for (it = threads.begin(); it != threads.end(); ++it) {
		pthread_t thread = it->first;
		pthread_kill(thread, SIGUSR1);
	}
	for (it = threads.begin(); it != threads.end(); ++it) {
		pthread_t thread = it->first;
		pthread_join(thread, NULL);
	}
	pthread_mutex_unlock(&threadsMutex);
	close(listenFD);
	exit(0);
}

// Writes specified length from given buffer to given file descriptor. Returns false on error.
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

// Handler for SIGUSR1 (executed in active worker threads when Ctrl+C is received and SIGUSR1 is sent to threads).
static void sigusr1_handler(int signum) {
	char shutDown[] = "421 localhost Service not available\r\n";
	std::map<pthread_t, int>::iterator it = threads.find(pthread_self());
	if (it != threads.end()) {
		int fd = it->second;
		do_write(fd, shutDown, sizeof(shutDown) - 1);
		if (vflag)
			fprintf(stderr, "[%d] S: %.*s", fd, (int) sizeof(shutDown) - 1,
					shutDown);
		close(fd);
		if (vflag)
			fprintf(stderr, "[%d] Connection closed\n", fd);
	}
	pthread_exit(NULL);
}

// Worker thread function for handling communication with one client.
void* worker(void *arg) {
	sigset_t newmask;
	sigemptyset(&newmask);
	sigaddset(&newmask, SIGINT);
	pthread_sigmask(SIG_BLOCK, &newmask, NULL);
	int comm_fd = *(int*) arg;
	if (!sigcaught) {
		pthread_mutex_lock(&threadsMutex);
		threads.insert(std::pair<pthread_t, int>(pthread_self(), comm_fd));
		pthread_mutex_unlock(&threadsMutex);
	}
	free(arg);
	char buf[1000];
	std::string sender;
	std::string recipient;
	std::vector<std::string> recipients;
	std::string dataLine;
	std::vector<std::string> data;
	int len = 0;
	int rcvd;
	int start;
	int index;
	int checkIndex;
	bool checked = false;
	bool alreadySet;
	int state = 0;
	char helo[] = "250 localhost\r\n";
	char stateError[] = "503 Bad sequence of commands\r\n";
	char paramError[] = "501 Syntax error in parameters or arguments\r\n";
	char commandError[] = "500 Syntax error, command unrecognized\r\n";
	char quit[] = "221 localhost Service closing transmission channel\r\n";
	char ok[] = "250 OK\r\n";
	char localhost[] = "@localhost";
	char notLocalError[] = "551 User not local\r\n";
	char noUserError[] = "550 No such user here\r\n";
	char intermediateReply[] =
			"354 Start mail input; end with <CRLF>.<CRLF>\r\n";
	char shutDown[] = "421 localhost Service not available\r\n";
	while (!sigcaught) {
		alreadySet = false;
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
								if (tolower(buf[j]) != localhost[checkIndex])
									validDomain = false;
								checkIndex++;
							}
							if (validDomain) {
								// Successful RCPT command.
								recipient = "";
								for (int j = 9; j < i - 11; j++) {
									recipient = recipient + buf[j];
								}
								if (getKVS(recipient, "password") != "") {
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
							// Acquire mutex for mailbox.
							if (mailboxes.find(recipients[a])
									== mailboxes.end()) {
								pthread_mutex_init(&mailboxes[recipients[a]],
								NULL);
							}
							pthread_mutex_lock(&mailboxes[recipients[a]]);
							std::string current = getKVS(recipients[a],
									"mailbox");
							current += temp;
							for (std::size_t b = 0; b < data.size(); b++) {
								current += data[b];
							}
							putKVS(recipients[a], "mailbox", current);
							printf("Current contents of the mailbox:\n%s",
									getKVS(recipients[a], "mailbox").c_str());
							// Release mutex for mailbox.
							pthread_mutex_unlock(&mailboxes[recipients[a]]);
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
					pthread_mutex_lock(&threadsMutex);
					do_write(comm_fd, quit, sizeof(quit) - 1);
					if (vflag)
						fprintf(stderr, "[%d] S: %.*s", comm_fd,
								(int) sizeof(quit) - 1, quit);
					close(comm_fd);
					if (vflag)
						fprintf(stderr, "[%d] Connection closed\n", comm_fd);
					threads.erase(pthread_self());
					pthread_detach(pthread_self());
					pthread_mutex_unlock(&threadsMutex);
					pthread_exit(NULL);
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
	pthread_mutex_lock(&threadsMutex);
	do_write(comm_fd, shutDown, sizeof(shutDown) - 1);
	close(comm_fd);
	if (vflag)
		fprintf(stderr, "[%d] Connection closed\n", comm_fd);
	threads.erase(pthread_self());
	pthread_detach(pthread_self());
	pthread_mutex_unlock(&threadsMutex);
	pthread_exit(NULL);
}

// Processes command line arguments to program.
std::string processArguments(int argc, char *argv[], int *p, int *aflag,
		int *vflag) {
	char *pstr = NULL;
	char *kstr = NULL;
	int c;
	int colons;
	opterr = 0;

	while ((c = getopt(argc, argv, "p:avk:")) != -1) {
		switch (c) {
		case 'a':
			*aflag = 1;
			break;
		case 'v':
			*vflag = 1;
			break;
		case 'p':
			pstr = optarg;
			for (char *i = pstr; *i != '\0'; i++) {
				if (!isdigit(*i)) {
					fprintf(stderr, "Invalid argument to option `-p'.\n");
					exit(1);
				}
			}
			*p = atoi(pstr);
			break;
		case 'k':
			kstr = optarg;
			colons = 0;
			for (char *i = kstr; *i != '\0'; i++) {
				if (*i == ':')
					colons++;
			}
			if (colons != 1) {
				fprintf(stderr, "Invalid argument to option `-k'.\n");
				exit(1);
			}
			break;
		case '?':
			if (optopt == 'p')
				fprintf(stderr, "Option `-%c' requires an argument.\n", optopt);
			else if (isprint(optopt))
				fprintf(stderr, "Unknown option `-%c'.\n", optopt);
			else
				fprintf(stderr, "Unknown option character `\\x%x'.\n", optopt);
			exit(1);
		default:
			fprintf(stderr, "Error processing command line arguments.\n");
			exit(1);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "Too many command line arguments.\n");
		exit(1);
	}

	return std::string(kstr);
}

// Main function for opening TCP port, accepting connections, and spawning worker threads.
int main(int argc, char *argv[]) {
	struct sigaction newact;
	newact.sa_handler = sigint_handler;
	sigemptyset(&newact.sa_mask);
	newact.sa_flags = 0;
	sigaction(SIGINT, &newact, NULL);

	newact.sa_handler = sigusr1_handler;
	sigaction(SIGUSR1, &newact, NULL);

	int p = 2500;
	int aflag = 0;
	vflag = 0;
	std::string kvsFullAddress = processArguments(argc, argv, &p, &aflag,
			&vflag);
	std::stringstream ss(kvsFullAddress);
	std::string item;
	bool isAddr = true;
	while (std::getline(ss, item, ':')) {
		if (isAddr) {
			kvsAddress = item;
			isAddr = false;
		} else {
			kvsPort = stoi(item);
		}
	}

	if (aflag) {
		fprintf(stderr, "Bharath Jaladi (bjaladi).\n");
		return 1;
	}

	listenFD = socket(PF_INET, SOCK_STREAM, 0);
	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(p);
	if (bind(listenFD, (struct sockaddr*) &servaddr, sizeof(servaddr)) == -1) {
		if (p != 2500)
			fprintf(stderr,
					"Error calling bind. (This could be due to the specified port number.)\n");
		else
			fprintf(stderr, "Error calling bind.\n");
		close(listenFD);
		return 1;
	}
	if (listen(listenFD, 100) == -1) {
		fprintf(stderr, "Error calling listen.\n");
		close(listenFD);
		return 1;
	}
	char greeting[] = "220 localhost Service ready\r\n";
	while (true) {
		struct sockaddr_in clientaddr;
		socklen_t clientaddrlen = sizeof(clientaddr);
		int fdTemp = accept(listenFD, (struct sockaddr*) &clientaddr,
				&clientaddrlen);
		int *fd = (int*) malloc(sizeof(int));
		*fd = fdTemp;
		if (vflag)
			fprintf(stderr, "[%d] New connection\n", *fd);
		do_write(*fd, greeting, sizeof(greeting) - 1);
		if (vflag)
			fprintf(stderr, "[%d] S: %.*s", *fd, (int) sizeof(greeting) - 1,
					greeting);
		pthread_t thread;
		pthread_create(&thread, NULL, worker, fd);
	}
}
