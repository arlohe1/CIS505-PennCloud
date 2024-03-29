Full names:  Amit Lohe, Bharath Jaladi, Liana Patel, Prasanna Poudyal
SEAS logins: alohe, bjaladi, lianap, poudyal

Which features did you implement? 
  (list features, or write 'Entire assignment')
  - Frontend HTTP servers and load balancer. Our load balancer redirects incoming connections to available frontend HTTP
    servers in a round-robin approach. If a frontend node crashes, users connected to that server can connect to the
    load balancer and be redirected to other frontend servers with no data loss.
  - User accounts with the ability to sign up, login, and change one's password. Usernames are unique, and descripting
    error messages are displayed to the client in the event that a user does not exist, already exists, or enters an
    incorrect password. Every user is assigned an email corresponding to their username and access to the storage
    service, as well as the other services available on PennCloud.
  - Storage service with support for file upload, directories, moving and renaming items, and deleting items (PennDrive).
  - Email service with support for composing, replying, forwarding, and deleting emails. Emails can be sent internally
    to existing users and externally to @seas accounts. SMTP support is also included and can be used to accept external
    emails via telnet in accordance with the SMTP protocol (PennMail).
  - Admin console with support for starting and stopping backend and frontend servers. Additional info can be viewed
    about these servers, such as the number of connected clients for frontend servers, or row-col-value data for backend
    servers.
  - Fault-tolerant backend key-value store similar to Google Bigtable. Our KV-store has support for up to four clusters,
    with each cluster containing 3 nodes. The RPCLib library is used for RPC communication between backend servers and
    between the frontend and backend servers. Each cluster has a primary node, with the rest adopting the role of secondary
    nodes. Our KV-store maintains a sequential consistency model via a remote writes approach. We maintain a cache of
    recently accessed data in memory, and evict this cache (and clear our log file) after a certain number of write
    commands, or if our in-memory cache becomes too large. If a primary node crashes, the master node (which we assume will not
    crash), assigns a new primary for the cluster. When a crashed node comes back online, it requests all new data from
    the other nodes in the cluster and is brought up to speed to ensure consistent checkpointing and logging and
    minimize the risk of data loss.

Did you complete any extra-credit tasks? If so, which ones?
  (list extra-credit tasks)
  - PennDiscuss. PennDiscuss is a discussion forum application in which users can create forums and discuss topics with
    other users. All users on PennCloud have access to all forums made on the application. On the backend, PennDiscuss
    uses the Paxos consensus algorithm to maintain a consistent append-only ledger of agreed-upon operations.
    PennDiscuss relies on a separate cluster of paxosServers to provide this service.
  - PennChat. PennChat is a persistent instant messaging service where users can create chatrooms with one or more members and chat
    with the users in rooms. Users see messages sent to the room by other members of the room in FIFO ordering and can 
    respond in real time. On the server side, PennChat uses multicast within front end servers for speed and efficiency, but uses
    the KV store to save messages for fault tolerance.
  - Deployment on EC2. We use Amazon AWS's EC2 infrastructure to deploy PennCloud. We have multiple EC2 instances that communicate
    with each other and run both the front end and back end servers with load balancer and master node. We used t2.micro instances
    to deploy our server for scalabilty at a reasonable price. Since our servers are address-agnostic PennCloud is easily deployable
    to multiple machines.

Did you personally write _all_ the code you are submitting
(other than code from the course web page)?
  [] Yes
  [X] No
  Please see below for attributions.

Did you copy any code from the Internet, or from classmates?
  [X] Yes
  [] No
  Please see below for attributions.

Did you collaborate with anyone on this assignment?
  [ ] Yes
  [X] No


Attributions for external code use (with permission of Profesor):

  - The RPCLib library was used for communication between the frontend servers and backend servers, and for
    communication between backend servers.

  - For two functions, decodeURIComponent and encodeURIComponent, we want to attribute arthurafarias on
    Github: https://gist.github.com/arthurafarias/56fec2cd49a32f374c02d1df2b6c350f.

  - For external mail, we used as sources: https://stackoverflow.com/questions/1688432/querying-mx-record-in-c-linux
    and https://stackoverflow.com/questions/52727565/client-in-c-use-gethostbyname-or-getaddrinfo


Project setup instructions:

  1) Set up RPCLib on your machine by following the instructions in the included rpclib_setup.txt file
  2) Run make in T20/rpcFrontend and in T20/rpcBackend/
  3) Run make serverfiles in T20/rpcBackend. This command clears all data for all backend servers and creates new
  directories for 12 backend kvServers (up to 4 clusters) and 3 paxosServers.
  4) In T20/rpcBackend, start the masterNode for kvServers via the following command:
     $ ./masterNode -v <config file>

     The -v option is used to enable logs. The given configFiles (configFile_1cluster, configFile_2clusters, etc.) are
     used to indicate the addresses of the masterNode (1st line) and the addresses of the kvServers (subsequent lines,
     with two ports per server - one for the "heartbeat thread" and one for the "kvServer" thread).

  5) In T20/rpcBackend, start the masterNode for paxosServers via the following command:
     $ ./masterNode -v <paxos config file>

     The -v option is used to enable logs. The given paxosConfig file is used to indicate the addresses of the masterNode (1st
     line) and the addresses of the paxosServers (subsequent lines, with two ports per server - one for the "heartbeat
     thread" and one for the "paxosServer" thread). Every 3 lines (after the first line) indicates a new cluster. Our
     implementation supports at most 4 clusters. 
     NOTE: At least one node per cluster (indicated by the configFile) must be functional for the system to work
     properly (allow access to all data).

  6) In T20/rpcBackend, start kvServers (at least one per cluster ideally) via the following command:
     $ ./kvServer -v <config file> <index>

     The -v option is used to enable logs. The given config files are used to indicate the addresses of the masterNode and
     addresses of the kvServers. The index argument should be a number that indicates the address that this particular
     kvServer should use (minimum: 1, maximum: # of kvServer entries in the configFile).


  7) In T20/rpcFrontend, run the includeds script ./deploy_frontend.sh as written below:
     $ ./deploy_frontend.sh -v -c config_frontend -b 127.0.0.1:8000

     This script will start 4 frontend servers and a load balancer as specified in the included config file
     config_frontend. The -v option is used to enable logs. -c is used to indicate the name of a config file. -b is used
     to indicate the address of the backend masterNode.

  8) In your browser, go to http://localhost:12001 (or the port listed in your frontend config file). You should be
  redirected to a different frontend server. You should now be able to create an account and interact with all elements
  available on PennCloud.

  9) To login as the admin, login with the username "admin" and the password "password" and you should be directed to
  the admin console at the route /admin. Backend and frontend servers can be stopped via the admin console or via CTRL-C
  in the terminal.

  10) To kill frontend servers, you can run the script listed in step (7) with the -k option.

  11) Please note: in order for the system to function properly, the different elements of the system must be started in
  the correct order. Two masterNodes for the kvServers and the paxosServers should be started first. After that,
  kvServers, paxosServers, and the frontend servers can all be launched.



