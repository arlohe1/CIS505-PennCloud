

if [ $# -eq 0 ]
then
	echo "Missing options!"
	echo "(run $0 -h for help)"
	echo ""
	exit 0
fi

CONFIG=""
BACKEND=""
VERBOSE=0
LOGFILE="log"

while getopts ":c:b:l:vkh?" OPTION; do
	case $OPTION in
	
		c)
			CONFIG=$OPTARG
			;;
		
		b)
			BACKEND=$OPTARG
			;;
		v)
			VERBOSE=1
			;;
		l)
			LOGFILE=$OPTARG
			;;	
		k)
			pkill frontend
			echo "Killed all frontend servers!"
			exit 0
			;;
		h|\:)
			echo "Usage:"
			echo "$0 -c <path to config file> -b <address of backend master node> -l <logfile_prefix>"
			echo ""
			exit 0
			;;
	esac
done

if [ -z "$CONFIG" ];
then
	echo "Need to specify config file (run $0 -h for help"
	exit 0
fi

if [ -z "$BACKEND" ];
then
	echo "Need to specify backend address (run $0 -h for help"
	exit 0
fi

LOAD_BALANCER="false"

echo "Using $CONFIG as config file..."

server_index=0

cat "$CONFIG" | while read -r line
do
	if [ "$LOAD_BALANCER" = "false" ];
	then
		LOAD_BALANCER="true"
		IFS=',' read -r -a addr_arr <<< "$line"
		IFS=':' read -r -a http_addr_arr <<< "${addr_arr[0]}"
		IFS=':' read -r -a internal_addr_arr <<< "${addr_arr[1]}"
		http_port_no=${http_addr_arr[1]}
		internal_port_no=${internal_addr_arr[1]}
		if [ $VERBOSE = 1 ];
		then
			echo "./frontend -v -k $BACKEND -c $CONFIG -l -p $http_port_no -r $internal_port_no &> ${LOGFILE}_loadbalancer &"
			./frontend -v -k $BACKEND -c $CONFIG -l -p $http_port_no -r $internal_port_no &> ${LOGFILE}_loadbalancer &
		else
			echo "./frontend -k $BACKEND -c $CONFIG -l -p $http_port_no -r $internal_port_no &> ${LOGFILE}_loadbalancer &"
			./frontend -k $BACKEND -c $CONFIG -l -p $http_port_no -r $internal_port_no &> ${LOGFILE}_loadbalancer &
		fi
	else
		IFS=',' read -r -a addr_arr <<< "$line"
		IFS=':' read -r -a http_addr_arr <<< "${addr_arr[0]}"
		IFS=':' read -r -a smtp_addr_arr <<< "${addr_arr[1]}"
		IFS=':' read -r -a internal_addr_arr <<< "${addr_arr[2]}"
		http_port_no=${http_addr_arr[1]}
		smtp_port_no=${smtp_addr_arr[1]}
		internal_port_no=${internal_addr_arr[1]}
		if [ $VERBOSE = 1 ];
		then
			echo "./frontend -v -k $BACKEND -c $CONFIG -i $server_index -p $http_port_no -q $smtp_port_no -r $internal_port_no &> ${LOGFILE}_server${server_index} &"
			./frontend -v -k $BACKEND -c $CONFIG -i $server_index -p $http_port_no -q $smtp_port_no -r $internal_port_no &> ${LOGFILE}_server${server_index} &
		else
			echo "./frontend -k $BACKEND -c $CONFIG -i $server_index -p $http_port_no -q $smtp_port_no -r $internal_port_no &> ${LOGFILE}_server${server_index} &"
			./frontend -k $BACKEND -c $CONFIG -i $server_index -p $http_port_no -q $smtp_port_no -r $internal_port_no &> ${LOGFILE}_server${server_index} &
		fi
	fi
	let server_index+=1
done
