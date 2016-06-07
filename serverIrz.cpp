#include <map>
#include <time.h>
#include <string.h>
#include <stdarg.h>
#include <postgresql/libpq-fe.h>
#include "local.h"
#include "dbconnectionsettings.h"

//usual
using namespace std;

//for service auth connection
int serviceAuthConnection = 0;
int serviceSockFd = -1;

FILE * logFd = NULL;
int inDebugFoo = 0;

//for lists
typedef map<int, hash> MapType;
typedef map<long, int> MapType2;

//hashes
MapType clients_map; 	//sock fd <-> hash item
MapType2 clients_map2;  //imei    <-> sock fd

//for db
PGconn*    connection_DB;
PGresult*  result_DB_transaction;

//foos
int handlePoll(int client);
void setNonBlocking(int sock);
void closeClient(int client);
int getArrIndexByHeader(char * t);
void initMeterages(meterage_t * m);

//foos wiht db access
int connectDb(void);
void resetPrevStates();
int checkImei(char * imei);
int saveSyncResult(int secs, char * imei);
//int getConfigSize (long imei);
int getConfigTo (long imei, char ** config);
int sendConfigTo (int client, char * config, int totalLen, int * pointer, int * progress);
int saveDataToDb(char * typeMeterage, char * timeStamp, meterage_t meterage, char * counterId);
int setOnlineState(char * imei, int online);
int saveEventToDb(char * imei, int ev_type, char * ev_time, char * ev_text);
int saveConnected(char * imei);
int saveDisconnected(char * imei);
void saveActionToStatusTable(char * imei, char * counterId);
void saveSocketToStatusTable(char * imei, int socketId);
void Log (const char * str, ...);

//
//
//
void Log (const char * str, ...) 
{
    va_list arg;
    char out[DEBUG_OUT_SIZE];

    if (inDebugFoo == 1){
        while (inDebugFoo == 0) {
        };
	}
	
    inDebugFoo = 1;

    va_start(arg, str);
    memset(out, 0, DEBUG_OUT_SIZE);
    vsprintf(out, str, arg);
    va_end(arg);

    if (logFd == NULL) {
		char fileName[1024];
		
		time_t ltime;
		struct tm *Tm;
		ltime=time(NULL);
		Tm=localtime(&ltime);
 
		memset(fileName, 0 ,1024);
		sprintf(fileName, "/var/log/serverIrz/log_%04d-%02d-%02d_%02d.%02d.%02d",
            Tm->tm_year+1900,
			Tm->tm_mon+1,
			Tm->tm_mday,
            Tm->tm_hour,
            Tm->tm_min,
            Tm->tm_sec);
		logFd = fopen(fileName, "w");
    }
	
	if (logFd != NULL) {
		fprintf(logFd, "%s", out);
		fflush(logFd);
		printf ("%s", out);
	}
	
    inDebugFoo = 0;
}

//
//
//
int sendFoo(int client, char * buf, int size, int zero){
	int crcCalc = 0;
	int result = -1;
	char len[32];
	char crc[32];
	
	//pre setup
	memset(len, 0, 32);
	memset(crc, 0, 32);
	
	//copy len
	sprintf(len, "%d", size);
	
	//calculate crc
	for (int index =0; index < size; index++){
		char symb = *(buf + index ); 
		crcCalc = (crcCalc + (int)symb) % 256;
	}
	sprintf(crc, "%d", crcCalc);
	
	// send start
	result = send (client, PACKET_START_RUN, strlen(PACKET_START_RUN), 0);
	if (result < 0) return result;
	//send len
	result = send (client, len, strlen(len), 0);
	if (result < 0) return result;
	//close start
	result = send (client, PACKET_START_END, strlen(PACKET_START_END), 0);
	if (result < 0) return result;
	
	//send data
	result = send (client, buf, size, 0);
	if (result < 0) return result;
	
	// send end
	result = send (client, PACKET_STOP_RUN, strlen(PACKET_STOP_RUN), 0);
	if (result < 0) return result;
	//send crc
	result = send (client, crc, strlen(crc), 0);
	if (result < 0) return result;
	//close end
	result = send (client, PACKET_STOP_END, strlen(PACKET_STOP_END), 0);
	if (result < 0) return result;
	
	//send rn
	result = send (client, CRLF, strlen(CRLF), 0);
	if (result < 0) return result;
	
	return result;
}

//
//
//
int main(int argc, char *argv[])
{
	int epfd;
	int listener;
	int client, res, epoll_events_count;
	//char buf[BUF_SIZE];
	static struct epoll_event ev, events[EPOLL_SIZE];
	
	struct sockaddr_in addr, their_addr;
	addr.sin_family = PF_INET;
	addr.sin_port = htons(SERVER_PORT);
	addr.sin_addr.s_addr = inet_addr(SERVER_HOST);

	socklen_t socklen;
	socklen = sizeof(struct sockaddr_in);

	// configure epoll events
	ev.events = EPOLLIN | EPOLLET;
	
	// setup server listener
	listener = socket(PF_INET, SOCK_STREAM, 0);
	if (listener < 0){
		Log("ERROR: failed listener creation\n");
		exit(1);
	}
	Log("Main listener(fd=%d) created! \n", listener);

	//set reuse address
	int on = 1;
	if(setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0){
		Log("ERROR: failed set socket options on listener, quit\n");
		exit(1);
	}
	Log("Listener: option [reuse Addres] Ok\r\n");
	
	// setup nonblocking socket
	setNonBlocking(listener); 
	Log("Listener: option [non Blocking] Ok\r\n");
	
	// bind listener to address(addr)
	if(bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0){
		Log("ERROR: bind failed, quit\n");
		exit(1);
	}
	Log("Listener: binded [%s] Ok\n", SERVER_HOST);

	// start to listen connections
	if(listen(listener, 1) < 0){
		Log("ERROR: listen failed, quit\n");
		exit(1);
	}
	Log("Start to listen port: %d!\n", SERVER_PORT);

	// setup epoll
	epfd = epoll_create(EPOLL_SIZE);
	if (epfd < 0){
		Log("ERROR: failed creating epoll fd\n");
		exit(1);
	}
	Log("Epoll(fd=%d) created!\n", epfd);

	// set listener to event template
	ev.data.fd = listener;

	// add listener to epoll
	if(epoll_ctl(epfd, EPOLL_CTL_ADD, listener, &ev) < 0){
		Log("ERROR: failed epoll_ctl \n");
		exit(1);
	}
	Log("Main listener(%d) added to epoll!\n", epfd);

	if ( connectDb() == -1 )
	{
		Log("ERROR: can't connect to database, quit\n");
		exit(1);
	}

	// main cycle(epoll_wait)
	int mainLoop = 1;
	while(1)
	{
		//wait epoll events
		epoll_events_count = epoll_wait(epfd, events, EPOLL_SIZE, EPOLL_RUN_TIMEOUT);
		if (epoll_events_count < 0){
			Log("ERROR: epoll que crushed, quit\n");
			break;
		}
		
		//process events in loop
		for(int i = 0; i < epoll_events_count ; i++)
		{
			// EPOLLIN event for listener(new client connection)
			if(events[i].data.fd == listener)
			{
				client = accept(listener, (struct sockaddr *) &their_addr, &socklen);
				if (client < 0){
					continue;
					
				} else {

					// setup nonblocking socket
					setNonBlocking(client);

					// set new client to event template
					ev.data.fd = client;

					// add new client to epoll
					if(epoll_ctl(epfd, EPOLL_CTL_ADD, client, &ev) < 0){
						Log("ERROR: epoll ctr failed\n");
						mainLoop = 0;
						break;
					} else {
					
						// save new descriptor to further use
						hash clientHash;
						memset(&clientHash, 0, sizeof(clientHash));
						clientHash.lastPackTime = time(NULL);
						clientHash.socketBuffer = NULL;
						clients_map.insert(MapType::value_type(client, clientHash));

						//DEBUG
						Log ("\r\n\r\n!!! NEW CONNECTION\r\nsend HAU?\n");

						// send initial get imei to client
						res = sendFoo(client, (char *)WHO_ARE_YOU, strlen(WHO_ARE_YOU), 0);
						if (res < 0){
							closeClient(client);
						}
					}
				}
			}
			else 
			{   
				// EPOLLIN event for others(new incoming message from client)
				res = handlePoll(events[i].data.fd);
				if (res < 0){
					closeClient(events[i].data.fd);
				}
			}
		}

		//check extra time sockets
		//Log ("TIME CHECKER: check timimngs\n");
		for (MapType::iterator iter=clients_map.begin(); iter!=clients_map.end(); ++iter){

			//check we wait some input still
			if (iter->second.socketBufferSize > 0){

				//fix current time
				time_t nowTime = time(NULL);

				//view about last pack time
				double difference = difftime(nowTime, iter->second.lastPackTime);
				if (difference > MAX_PACKET_TIMEOUT){
					Log ("%s INPUT TIMEOUT (%d secs), ...client will be closed\n", iter->second.imei, difference);
					closeClient(iter->first);
					break;
				}
		   }      	
	   }
	   //Log ("TIME CHECKER: check timimngs done\n");
	}

	close(listener);
	close(epfd);

	return 0;
}

//
//
//
int handlePoll(int client)
{
    int len;
    //char * ptr = NULL;
    char * ptrA = NULL;
    char * ptrB = NULL;
    char answ[ANSW_SIZE];
	
	//input reader buffer
	char inputBuffer[INPUT_SIZE];
	
	//processing buffer
	char * buf = NULL;
	
    //find sicket id in hash
    MapType::iterator iter = clients_map.begin();
    iter = clients_map.find(client);
	if (iter == clients_map.end()){
		Log ("ERROR: ambigous iter pointer found, quit\n");
		exit(0);
	}
	
    //read socket to all buffer
	memset(inputBuffer, 0, INPUT_SIZE);
	
	len = read(client, inputBuffer, INPUT_SIZE);
	
	//check read result
	if (len > 0)
	{
		//DEBUG
		//Log ("\nRX[%d] %s\n",len, inputBuffer);

		//stub ?!
		//len = strlen(inputBuffer);
		
		//glue socket buffer with input
		iter->second.socketBuffer = (char *)realloc(iter->second.socketBuffer,
												    iter->second.socketBufferSize + len);
		//check memory allocating result
		if (iter->second.socketBuffer == NULL){
			//debug
			Log("ERROR: MEMORY ALLOC FAILED, quit application\n");
			exit (0);
			
		} else {
			//debug
			//Log ("allocated %d bytes for input buffer, will copy with offset %d\n", len, iter->second.socketBufferSize);
			
			//copy data to input
			memcpy(&iter->second.socketBuffer[iter->second.socketBufferSize], inputBuffer, len);
			iter->second.socketBufferSize+=len;
		}
		
		//try to parse start tag
		ptrA = &iter->second.socketBuffer[0];
		ptrB = NULL;

		//try to find '\r\n' at the end of the packet
		while( 1 )
		{
			//char * a;
			char  b[1024];
			char * tempEndPointer = NULL;
			char * tempEndPointer2 = NULL;
			char * startPacket = NULL;
			char * stopPacket = NULL;
			int packetLen = 0;
			int packetCrc = 0;
			int calcCrc = 0;
			int calcLen = 0;
			int sizeToCopy =0;

			//check line input
			if (ptrA == NULL){
				//debug
				//Log ("parser ends\n");
				
				break;
			}
			
			//check new portion in buffers
			ptrB=strstr(ptrA, "\r\n");
			if (ptrB == NULL){
				//debug
				//Log ("end of input reached\n");
				
				break;
			}
			
			int ab = 0;
			ab = ptrB-ptrA;
			if (ab > 0)
			{
				goto pack_try_parse;
			} 
			else 
			{
				//DEBUG
				Log("ERROR: NOTHING TO COPY BY RN\n");
				goto pack_check_time;
			}

			//parse packet start
				
			//00000000001111111111222
			//01234567890123456789012
			//<GISUR PACK LEN 95><>RN
pack_try_parse:
			startPacket = strstr(ptrA, PACKET_START_RUN);
			if (startPacket != NULL)
			{
				tempEndPointer = strstr(startPacket+1, PACKET_START_END);
				if (tempEndPointer != NULL)
				{
					sizeToCopy = (tempEndPointer - (startPacket+16));
					if (sizeToCopy > 0) 
					{
						memset(b, 0, 1024);
						memcpy(b, startPacket+16, sizeToCopy);
						packetLen = atoi(b);
						
					} 
					else 
					{
						//DEBUG
						Log ("NO PACKET LEN PRESENT, QUIT\n");
						goto pack_check_time;
					}		
			
				}
				else 
				{
					//DEBUG
					Log ("NO PACKET START (END) REACHED, QUIT\n");
					goto pack_check_time;
				}
				
			} 
			else 
			{
				//DEBUG
				Log ("NO PACKET START (START) REACHED, QUIT\n");
				goto pack_check_time;
			}
			
			// parse packet end

			//00000000001111111111
			//01234567890123456789
			//</GISUR PACK CRC 20>

			stopPacket = strstr(ptrA, PACKET_STOP_RUN);
			if (stopPacket != NULL)
			{
				tempEndPointer2 = strstr(stopPacket+1, PACKET_STOP_END);
				if (tempEndPointer2 != NULL)
				{
						sizeToCopy = (tempEndPointer2 - (stopPacket+17));
						if (sizeToCopy > 0)
						{
							memset(b, 0, 1024);
							memcpy(b, stopPacket+17, sizeToCopy);
							packetCrc = atoi(b);

						} 
						else 
						{
							//DEBUG
							Log ("NO PACKET CRC PRESENT, QUIT\n");
							goto pack_check_time;
						}

				} 
				else 
				{
					//DEBUG
					Log ("NO PACKET STOP (END) REACHED, QUIT\n");
					goto pack_check_time;
				}

			} 
			else 
			{
				//DEBUG
				Log ("NO PACKET STOP (START) REACHED, QUIT\n");
				goto pack_check_time;
			}

			//parse payload
								   
			//00000000001111111111222222222233333333334
			//01234567890123456789012345678901234567890
			//<GISUR PACK LEN 1>s</GISUR PACK CRC 20>RN

			//check len
			calcLen = (stopPacket - tempEndPointer) - 1;
			if (packetLen != calcLen)
			{
				//DEBUG
				Log ("PACKET LEN ERROR: pl[%d], cl[%d]\n", packetLen, calcLen);
				goto pack_mark_bad;
			}	

			//check crc
			for (int i=0; i<calcLen; i++)
			{
				char symb = *(tempEndPointer + i + 1); 
				calcCrc = (calcCrc + (int)symb) % 256;
			}

			if (packetCrc != calcCrc)
			{
				//DEBUG
				Log ("PACKET CRC ERROR: pc[%d], cc[%d]\n", packetCrc, calcCrc);
				goto pack_mark_bad;
			}
			
			//
			//if all is OK - process 
			//copy payload to buffer to process
			//
			
			//preempty buf
			if (buf != NULL){
				//debug
				//Log ("preempty local used buf ptr\n");
				
				free(buf);
			}
			
			//allocate new mem
			buf = (char *)malloc(calcLen+1);
			if (buf == NULL){
				//debug
				Log("ERROR: MEMORY ALLOCAION ERROR, quit application\n");
			
				exit (0);
			
			} else {
				//debug
				//Log ("allocated %d bytes for buf at %08x\n",calcLen+1, buf);
				
				//copy data to input
				memset(buf, 0, calcLen+1);
				memcpy(buf, tempEndPointer+1, calcLen);
			}

			//free input for next packet
			int pSizeAll = (tempEndPointer2 - startPacket)+1+2; //plus rn + shift
			if (iter->second.socketBufferSize == pSizeAll){
				//simply free socket buffer
				//Log ("fully free input buffer\n");
				
				free(iter->second.socketBuffer);
				iter->second.socketBuffer = NULL;
				iter->second.socketBufferSize = 0;
				ptrA = NULL;
				ptrB = NULL;
				
			} else {
				//shft buffer content to process more
				//Log ("move part input buffer\n");
				
				int moveSize = iter->second.socketBufferSize - pSizeAll;
				memmove(iter->second.socketBuffer, (tempEndPointer2+3), moveSize);
				iter->second.socketBuffer = (char *)realloc(iter->second.socketBuffer, moveSize);
				iter->second.socketBufferSize = moveSize;
				ptrA = &iter->second.socketBuffer[0];
				ptrB = NULL;
				
			}
			
			//get len
			len = strlen(buf);

			if (strlen(iter->second.imei) > 0){
				Log("\n%s PARSE: %s\n", iter->second.imei, buf);
			
			} else {
				Log("\nno imei get yet PARSE: %s\n", buf);
				
			}
			
			
			//if read len > 0 go to parser
			if(len > 0)
			{
				//switch state
				switch (iter->second.state)
				{
					//
					// RECIEVE DATA AS FIRST AFTER CONNECT
					//
					case STATE_AUTORISE:
					{
						int debug = 0;

						//parse debug information from modem
						if (strstr(buf, DEBUG_HEADER)!=NULL)
						{
							debug = 1;

							//DEBUG
							Log ("%s\n", buf);
						}

						//DEBUG
						//Log ("Someone want to authorize with imei %s len %ld\n", buf, strlen(buf));
						
						if ((len >= 10) && (len <= 24))
						{
							int pingtimes = -1;
							int serviceAuth = 0;
							
							//check imei for service number and get ping times from DB for connection
							if (strstr(buf, SERVICE_IMEI) != NULL)
							{
								if (serviceAuthConnection == 0)
								{
									pingtimes = 0;
									serviceAuth = 1;
								}
								else
								{
									CHK(sendFoo(client, (char *)BUZY, strlen(BUZY), 0));
								}
							}
							else
							{
								pingtimes = checkImei( buf ); 
							}
							
							//DEBUG
							//Log ("check in database return times %d, auth %d\n", pingtimes, serviceAuth);
							
							if (pingtimes == -1)
							{
								if (debug == 0){
									//closeClient(client);
									goto pack_mark_bad;
								}
							} 
							else
							{	
								//set second map
								long imei = -1;
								imei = atol(buf);
								clients_map2.insert(MapType2::value_type(imei, client));
								
								//send ping times
								memset (answ, 0, ANSW_SIZE);
								sprintf(answ, "%d", pingtimes);
								CHK(sendFoo(client, answ, strlen(answ), 0));
								
								//setup hash
								iter->second.state = STATE_CONNECTED;
								iter->second.pings = pingtimes;
								sprintf(iter->second.imei, "%ld", imei);
								iter->second.lastTime = time(NULL);
								iter->second.waitFlags |= PING_TIMES;
								iter->second.serviceAuth = serviceAuth;
								
								//turn on keep alive
								int optval = 1;
								if(setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, &optval, sizeof(socklen_t)) < 0) 
								{
									//DEBUG
									Log ("%s ERROR SET socket options\n", iter->second.imei);
									//closeClient(client);
									goto pack_mark_bad;
								}
							        
								//set online state in db
								if (serviceAuth == 0)
								{
									setOnlineState(iter->second.imei, 1);
									
									saveSocketToStatusTable(iter->second.imei, client);
								}
	
								//DEBUG
								if (serviceAuth == 0)
									Log ("%s STATE [CONNECTED]\n", iter->second.imei);
							}
						} 
						else
						{
							if (debug == 0){
								//closeClient(client);
								goto pack_mark_bad;
							}
						}
					}
					break;
					
					//
					// RECIEVE SOME DATA AFTER AUTOTIZE
					//
					case STATE_CONNECTED:
					{
						int debug = 0;

						//parse debug information from modem
						//iif (strstr(buf, DEBUG_HEADER)!=NULL)
						//{
						//	debug = 1;
						//
						//	//DEBUG
						//	Log ("INPUT BUFFER: %s\n", buf);
						//}

						//parse incoming command
						if ((strstr(buf, PING_TIMES_OK) != NULL) && (debug == 0)) 
						{
							//1. process recieved incomnig answer for ping times -> OK, wait
							if ((iter->second.waitFlags & PING_TIMES) == PING_TIMES)
							{
								iter->second.lastTime = time(NULL);
								iter->second.waitFlags-=PING_TIMES;
								
								//DEBUG
								if (iter->second.serviceAuth == 0)
									Log ("%s STATE [PING_TIMES_OK]\n", iter->second.imei);
							}
							else
							{	
								if (debug == 0){
									//closeClient (client);
									goto pack_mark_bad;
								}
								
								//DEBUG
								if (iter->second.serviceAuth == 0)
									Log ("%s ERROR waiting PING_TIMES_OK\n", iter->second.imei);
							}
						} 
						else if ((strstr(buf, LIVE) != NULL) && (debug == 0))
						{
							//2. process recieved incomnig live -> send UTC
							memset (answ, 0, ANSW_SIZE);
							sprintf(answ,"time;%ld;", time(NULL));
							CHK(sendFoo(client, answ, strlen(answ), 0));
							iter->second.lastTime = time(NULL);
							iter->second.waitFlags |= SYNC_SECS;
							
							//DEBUG
							if (iter->second.serviceAuth == 0)
								Log ("%s STATE [LIVE]\n", iter->second.imei);
						} 
						else if ((strstr(buf, SYNC_ANSWER) != NULL) && (debug == 0))
						{
							//2. process recieved incomnig ping result -> OK, wait
							if ((iter->second.waitFlags & SYNC_SECS) == SYNC_SECS)
							{
								char * comma = NULL;
								int syncTime = 0;
								comma = strstr(buf, ";");
								if (comma != NULL)
								{
									comma++;
									syncTime = atoi(comma);
									saveSyncResult(syncTime, iter->second.imei);
								}
								
								iter->second.lastTime = time(NULL);
								iter->second.waitFlags-=SYNC_SECS;
								
								//DEBUG
								if (iter->second.serviceAuth == 0)
									Log ("%s STATE [SYNC_ANSWER] [sync time %d]\n", iter->second.imei, syncTime);
							}
							else
							{
								if (debug == 0){
									//closeClient (client);
									goto pack_mark_bad;
								}
								
								//DEBUG
								if (iter->second.serviceAuth == 0)
									Log ("%s ERROR waiting SYNC_ANSWER\n", iter->second.imei);
							}
						}
						
						//
						// PARSE INCOMING DATA
						//
						else if ((strstr(buf, DATA) != NULL) && (debug == 0)) 
						{
							//3. process recieved incoming data
							char * run = buf;
							char * end = buf;
							
							char counterId[16];
							char typeMeterage[32];
							char timeStamp[32];
							char headers[HEADER_SIZE * VALUES_COUNT];
							char values[VALUES_SIZE * VALUES_COUNT];

							int result = 0;					

							int field = 0;
							meterage_t meterage;
							
							memset(counterId, 0 , 16);
							memset(typeMeterage, 0, 32);
							memset(timeStamp, 0, 32);
							memset(headers, 0, HEADER_SIZE * VALUES_COUNT);
							memset(values, 0, VALUES_SIZE * VALUES_COUNT);
							
							//initialize  metergaes array
							initMeterages(&meterage);
							
							//data;id;type;stamp;headers(using ',');values(using ',');
							
							//DEBUG
							//Log ("DATA IS\n %s\n", buf);

							while (field < 6)
							{
								run = end + 1;
								end = strstr(run, ";");
								if (end != NULL)
								{
									if ((end-run) > 0)
									{
										switch (field)
										{
											case 0:
												//skip, nothing needed to do with tag
												break;
												
											case 1: 
												//copy counterId
												memcpy(counterId, run, (end-run));
												
												//DEBUG
												//Log ("parse counterId %s\n", counterId);
												break;
												
											case 2:
												//copy typeMeterage
												memcpy(typeMeterage, run, (end-run));
												
												//DEBUG
												//Log ("parse typeMeterage %s\n", typeMeterage);
												break;
												
											case 3:
												//copy timeStamp
												memcpy(timeStamp, run, (end-run));
												
												//DEBUG
												//Log ("parse timeStamp %s\n", timeStamp);
												break;
												
											case 4:
												//copy headers
												memcpy(headers, run, (end-run));
												
												//DEBUG
												//Log ("parse headers %s\n", headers);
												break;
												
											case 5:
												//copy values
												memcpy(values, run, (end-run));
												result = 1;

												//DEBUG
												//Log ("parse values %s\n", values);
												break;
												
											default:
												// DEBUG
												result = 0;
												Log ("%s ERROR DATA parse filed %d\n", iter->second.imei, field);
												break;
										}
										field++;
									}
									else
									{
										// DEBUG
										result = 0;
										Log ("%s ERROR DATA copy filed %d with len = 0\n", iter->second.imei, field);
										break;
									}
								} 
								else 
								{
									// DEBUG
									result = 0;
									Log ("%s ERROR DATA no more bytes to parse field %d\n", iter->second.imei, field);
									break;
								}
							}
							
							//investigate headers and parse values
							if ((strlen(headers) > 0) && (result == 1))
							{
								char * ptrHrun = headers;
								char * ptrHend = headers;
								char * ptrVrun = values;
								char * ptrVend = values;
								char * totalHend = headers + strlen(headers)-1;
								char * totalVend = values + strlen(values)-1;
							
								switch (atoi(typeMeterage))
								{
									case 1: //current meterage
									case 3: //hourly meterage
									case 4: //daily meterage
									{
										while (true)
										{
											if ((ptrHrun < totalHend) && (ptrVrun < totalVend))
											{
												ptrHend = strstr(ptrHrun, ",");
												ptrVend = strstr(ptrVrun, ",");
												if ((ptrHend != NULL) && (ptrVend != NULL))
												{
													if (((ptrHend-ptrHrun) > 0) && ((ptrVend-ptrVrun) > 0))
													{
														char valueT[HEADER_SIZE];
														char valueV[VALUES_SIZE];
														
														memset(valueT, 0, HEADER_SIZE);
														memset(valueV, 0, VALUES_SIZE);
														
														memcpy(valueT, ptrHrun, (ptrHend-ptrHrun));
														memcpy(valueV, ptrVrun, (ptrVend-ptrVrun));
														
														int arrIndex = getArrIndexByHeader(valueT);
														if (arrIndex != -1)
														{
															memset(meterage.values[arrIndex], 0, VALUES_SIZE);
															memcpy(meterage.values[arrIndex], valueV, strlen(valueV));
														}
														else
														{
															//DEBUG
															result = 0;
															Log ("%s ERROR DATA header of value is unknown [%s]\n", iter->second.imei, valueT);
														}
													}
													else
													{
														//DEBUG
														result = 0;
														Log ("%s ERROR DATA header of value is len=0\n", iter->second.imei);
													}
												}
												else
												{
													//DEBUG
													result = 0;
													Log ("%s ERROR DATA N of headers and N of values is not euqal\n", iter->second.imei);
												}
											}
											else
											{
												break;
											}
											
											ptrHrun = ptrHend + 1;
											ptrVrun = ptrVend + 1;
										}
									}
									break;
								}
							}
							else
							{
								//DEBUG
								result = 0;
								Log ("%s ERROR DATA no headers to parse values\n", iter->second.imei);
							}
							
							if (result == 1)
							{
								int saveRes = saveDataToDb(typeMeterage, timeStamp, meterage, counterId);
								if (saveRes != -1)
								{
									//send OK
									CHK(sendFoo(client, (char *)DATA_OK, strlen(DATA_OK), 0));
									iter->second.lastTime = time(NULL);
								}
								else
								{
									//DEBUG
									Log ("%s ERROR DATA save values to DB\n", iter->second.imei);
								}
								
								saveActionToStatusTable( iter->second.imei, counterId);
								
								//DEBUG
								Log ("%s STATE [DATA] [Cid %s, db save ok]\n", iter->second.imei, counterId);
							} else {

								//DEBUG
								Log ("%s STATE [DATA] [parse error]\n", iter->second.imei);
								
								//send OK by way (please del in future)
								CHK(sendFoo(client, (char *)DATA_OK, strlen(DATA_OK), 0));
								iter->second.lastTime = time(NULL);
							}
						} 
						
						//
						// PARSE INCOMING EVENT
						//
						else if ((strstr(buf, EVENT) != NULL) && (debug == 0))
						{
							int ttype = -1;
							char ddate[128];
							char eevent[1024];
							char * prev;
							char * evComma = strstr(buf, ";");
							memset(ddate, 0, 128);
                            memset(eevent, 0, 1024);

							if (evComma != NULL){
								//parse data after info tag
								prev = evComma;
								evComma = strstr(evComma+1, ";");
								if (evComma != NULL){
									//parse type
									char tmp[5];
									memset(tmp, 0, 5);
									memcpy(tmp, prev+1, (evComma-(prev+1)));
									ttype = atoi(tmp);
									prev = evComma;
									evComma = strstr(evComma+1, ";");
									if (evComma != NULL){
										//parse date
										memcpy(ddate, prev+1, (evComma-(prev+1)));
										prev = evComma;
										evComma = strstr(evComma+1, ";"); 
										if (evComma != NULL){
											//parse event text
											memcpy(eevent, prev+1, (evComma-(prev+1)));

											//debug
											Log ("%s STATE [EVENT] [type %d date %s text: %s]\n", iter->second.imei, ttype, ddate, eevent);
											if (strstr(ddate, "2000-01-01 00:00:00")!=NULL)
												memset(ddate, 0, 128);
											
											saveEventToDb(iter->second.imei, ttype, ddate, eevent);
											CHK(sendFoo(client, (char *)EVENT_OK, strlen(EVENT_OK), 0));				
										}
									}
								}
							}
						}

						//
						// PARSE ANSWERS FROM MODEM INITIATED BY SERVICE AUTH SCRIPT
						//
						else if ((strstr(buf, REBOOT_ANSWER) != NULL) && (debug == 0))
						{
							if((iter->second.waitFlags & CMD_REBOOT) == CMD_REBOOT)
							{
								//DEBUG
								Log ("%s STATUS [REBOOT_ANSWER]\n", iter->second.imei);
	
								//store answer
								memset(iter->second.status.rebootAnswer, 0, REBOOT_BUF_SIZE);
								memcpy(iter->second.status.rebootAnswer, buf, strlen(buf));
								iter->second.status.rebootStatus = READY;

								iter->second.lastTime = time(NULL);
								iter->second.waitFlags-=CMD_REBOOT;
							}
							else
							{
								//debug
								Log("%s ERROR waiting REBOOT_ANSWER\n", iter->second.imei);
							}
						}
						
						else if ((strstr(buf, REMOTE_PING_ANSWER) != NULL) && (debug == 0))
						{
							//B. process recieved incoming data about plc info
							if ((iter->second.waitFlags & CMD_REMOTE_PING) == CMD_REMOTE_PING) 
							{
								//DEBUG
								Log ("%s STATUS [REMOTE_PING_ANSWER]\n", iter->second.imei);
								
								//store answer
								memset(iter->second.status.remotePingAnswer, 0, REMOTE_PING_BUF_SIZE);
								memcpy(iter->second.status.remotePingAnswer, buf, strlen(buf));
								iter->second.status.remotePingStatus = READY;
								
								iter->second.lastTime = time(NULL);
								iter->second.waitFlags-=CMD_REMOTE_PING;
							}
							else
							{
								//DEBUG
								Log ("%s ERROR waiting REMOTE_PING_ANSWER\n", iter->second.imei);
							}
						}
						
						else if ((strstr(buf, MODEM_ANSWER) != NULL) && (debug == 0))
						{
							//A. process recieved incoming data about modem info
							if ((iter->second.waitFlags & CMD_MODEM) == CMD_MODEM)
							{
								//DEBUG
								Log ("%s STATUS [MODEM_ANSWER]\n", iter->second.imei);
								
								//store answer
								memset(iter->second.status.modemAnswer, 0, MODEM_BUF_SIZE);
								memcpy(iter->second.status.modemAnswer, buf, strlen(buf));
								iter->second.status.modemStatus = READY;
								
								iter->second.lastTime = time(NULL);
								iter->second.waitFlags-=CMD_MODEM;
							}
							else
							{
								//DEBUG
								Log ("%s ERROR waiting MODEM_ANSWER\n", iter->second.imei);
							}
						}
						else if ((strstr(buf, PLC_ANSWER) != NULL) && (debug == 0))
						{
							//B. process recieved incoming data about plc info
							if ((iter->second.waitFlags & CMD_PLC) == CMD_PLC) 
							{
								//DEBUG
								Log ("%s STATUS [PLC_ANSWER]\n", iter->second.imei);
								
								//store answer
								int allocSize = strlen(buf) + 1;
								
								if (iter->second.status.plcAnswer){
									free (iter->second.status.plcAnswer);
								}
								
								iter->second.status.plcAnswer = (char *)malloc(allocSize);	
								if (iter->second.status.plcAnswer){
									memset(iter->second.status.plcAnswer, 0, allocSize);
									memcpy(iter->second.status.plcAnswer, buf, allocSize-1);
									
									iter->second.status.plcAnswerSize = allocSize-1;
									iter->second.status.plcStatus = READY;
								
									iter->second.lastTime = time(NULL);
									iter->second.waitFlags-=CMD_PLC;
								} else {
									Log("ERROR: failed allocating mem\n");
									exit(1);
								}
							}
							else
							{
								//DEBUG
								Log ("%s ERROR waiting PLC_ANSWER\n", iter->second.imei);
							}
						}
						else if ((strstr(buf, POWER_TURN_ANSWER) != NULL) && (debug == 0))
						{
							//C. process recieved incoming data about ok recieve of power tuirn command
							if ((iter->second.waitFlags & CMD_POWER_TURN) == CMD_POWER_TURN) 
							{
								//DEBUG
								Log ("%s STATUS [POWER_TURN_ANSWER]\n", iter->second.imei);
								
								//store answer
								memset(iter->second.status.powerTurnAnswer, 0, POWER_TURN_SIZE);
								memcpy(iter->second.status.powerTurnAnswer, buf, strlen(buf));
								iter->second.status.powerTurnStatus = READY;
								
								iter->second.lastTime = time(NULL);
								iter->second.waitFlags-=CMD_POWER_TURN;
							}
							else
							{
								//DEBUG
								Log ("%s ERROR waiting PLC_ANSWER\n", iter->second.imei);
							}
						}
						else if ((strstr(buf, CONFIG_ANSWER) != NULL) && (debug == 0)) 
						{
							//C. process recieved incoming data about writing config
							if ((iter->second.waitFlags & CMD_CONFIG) == CMD_CONFIG)
							{
								//DEBUG
								Log ("%s STATUS [CONFIG_ANSWER]\n", iter->second.imei);
								//Log ("recv cfg answer as %s\n", buf);
								
								//send next parts to modem
								if (*(iter->second.status.configProgress) != 100)
								{
									//DEBUG
									//Log ("SEND NEXT PART OF CONFIG\n");
										
									if (sendConfigTo(client, iter->second.status.configPtr, strlen(iter->second.status.configPtr), iter->second.status.configPointer, iter->second.status.configProgress) < 0){
										goto pack_mark_bad;
									}
									iter->second.lastTime = time(NULL);
									iter->second.waitFlags|=CMD_CONFIG;
									iter->second.status.configStatus = PROGRESS;
								}
								else
								{
									//store answer
									memset(iter->second.status.configAnswer, 0, CONFIG_BUF_SIZE);
									memcpy(iter->second.status.configAnswer, buf, strlen(buf));
									iter->second.status.configStatus = READY;
									
									iter->second.lastTime = time(NULL);
									iter->second.waitFlags-=CMD_CONFIG;
								}
							}
							else
							{
								//DEBUG
								Log ("%s ERROR waiting CONFIG_ANSWER\n", iter->second.imei);
							}
						} 
						
						//
						// SERVER CONNECTION COMMANDS
						//
						else if ((strstr(buf, ASK_REBOOT) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							//Log ("STATUS [ASK_REBOOT]\n");

							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;

							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++;
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
							}

							//DEBUG
							//Log ("parsed imei %ld, find socket id %d\n", imei, sockFd);
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									int status = iter3->second.status.rebootStatus;

									//DEBUG
									//Log ("status %d\n", status);
									if (status == FREE)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask REBOOT on %ld\n", imei);

										//send command to modem
										CHK(sendFoo(sockFd, (char *)ASK_REBOOT, strlen(ASK_REBOOT), 0));

										iter3->second.lastTime = time(NULL);
										iter3->second.waitFlags|=CMD_REBOOT;
										iter3->second.status.rebootStatus = PROGRESS;

										//send answer to serv auth
										CHK(sendFoo(client, (char *)STARTED, strlen(STARTED), 0));
									}
									else if (status == PROGRESS)
									{
										//send answer to serv auth - in work
										CHK( sendFoo(client, (char *)IN_WORK, strlen(IN_WORK), 0));
									}
									else if (status == READY)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask REBOOT answer READY\n");

										//send answer to serv auth - done + answer
										int sizePtr = strlen(iter3->second.status.rebootAnswer) + strlen(STOPED)+1+1;
										char * ptrToSend = (char *)malloc (sizePtr);
										memset (ptrToSend, 0, sizePtr);
										sprintf (ptrToSend, "%s;%s", STOPED, iter3->second.status.rebootAnswer);
										CHK(sendFoo(client, ptrToSend, strlen(ptrToSend), 0));
										iter3->second.status.rebootStatus = FREE;
										free (ptrToSend);
										ptrToSend = NULL;
									}
									else
									{
										//send answer to serv auth - error case
										CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
									}
								}
							}
							else
							{
								//DEBUG
								Log ("SERVER AUTH SCRIPT ASK REBOOT\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR MODEM\n");

								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
						}
						
						
						else if ((strstr(buf, ASK_MODEM) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							//Log ("STATUS [ASK_MODEM]\n");
							
							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;
							
							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++;
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
							}
							
							//DEBUG
							//Log ("parsed imei %ld, find socket id %d\n", imei, sockFd);
							
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									int status = iter3->second.status.modemStatus;
									
									//DEBUG
									//Log ("status %d\n", status);
									
									if (status == FREE)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask MODEM on %ld\n", imei);

										//send command to modem
										CHK(sendFoo(sockFd, (char *)ASK_MODEM, strlen(ASK_MODEM), 0));
										
										iter3->second.lastTime = time(NULL);
										iter3->second.waitFlags|=CMD_MODEM;
										iter3->second.status.modemStatus = PROGRESS;
										
										//send answer to serv auth
										CHK(sendFoo(client, (char *)STARTED, strlen(STARTED), 0));
									}
									else if (status == PROGRESS)
									{
										//send answer to serv auth - in work
										CHK( sendFoo(client, (char *)IN_WORK, strlen(IN_WORK), 0));
									}
									else if (status == READY)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask MODEM answer READY\n");

										//send answer to serv auth - done + answer
										int sizePtr = strlen(iter3->second.status.modemAnswer) + strlen(STOPED)+1+1;
										char * ptrToSend = (char *)malloc (sizePtr);
										memset (ptrToSend, 0, sizePtr);
										sprintf (ptrToSend, "%s;%s", STOPED, iter3->second.status.modemAnswer);
										CHK(sendFoo(client, ptrToSend, strlen(ptrToSend), 0));
										iter3->second.status.modemStatus = FREE;
										free (ptrToSend);
										ptrToSend = NULL;
									}
									else
									{
										//send answer to serv auth - error case
										CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
									}
								}
							}
							else
							{
								//DEBUG
								Log ("SERVER AUTH SCRIPT ASK MODEM\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR MODEM\n");
								
								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
								
						}
						
						else if ((strstr(buf, ASK_PLC) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							//Log ("SERVICE AUTH SCRIPT [ASK_PLC]\n");
							
							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;
							
							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++;
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
							}
							
							//DEBUG
							//Log ("SERVICE AUTH SCRIPT [ASK_PLC imei %ld, socket id %d]\n", imei, sockFd);
							
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									int status = iter3->second.status.plcStatus;
									
									//DEBUG
									//Log ("SERVICE AUTH SCRIPT [ASK_PLC status %d]\n", status);
									
									if (status == FREE)
									{
										//DEBUG
										Log("SERVER AUTH SCRIPT ask PLC on %ld\n", imei);

										//send command to modem
										CHK(sendFoo(sockFd, (char *)ASK_PLC, strlen(ASK_PLC), 0));
										
										iter3->second.lastTime = time(NULL);
										iter3->second.waitFlags|=CMD_PLC;
										iter3->second.status.plcStatus = PROGRESS;
										
										//send answer to serv auth
										CHK(sendFoo(client, (char *)STARTED, strlen(STARTED), 0));
									}
									else if (status == PROGRESS)
									{
										//send answer to serv auth - in work
										CHK(sendFoo(client, (char *)IN_WORK, strlen(IN_WORK), 0));
									}
									else if (status == READY)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask PLC answer READY\n");

										//send answer to serv auth - done 
										int sizePtr = iter3->second.status.plcAnswerSize + strlen(STOPED)+1+1;
										char * ptrToSend = (char *)malloc (sizePtr);
										memset (ptrToSend, 0, sizePtr);
										sprintf (ptrToSend, "%s;%s", STOPED, iter3->second.status.plcAnswer);
										CHK(sendFoo(client, ptrToSend, strlen(ptrToSend), 0));
										iter3->second.status.plcStatus = FREE;
										free(ptrToSend);
										free (iter3->second.status.plcAnswer);
										ptrToSend = NULL;
										iter3->second.status.plcAnswer = NULL;
										iter3->second.status.plcAnswerSize = 0;
									}
									else
									{
										//send answer to serv auth - error case
										CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
									}
								}
							}
							else
							{
								//DEBUG
								Log ("SERVER AUTH SCRIPT ASK PLC\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR MODEM\n");
								
								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
						}
						
						else if ((strstr(buf, ASK_CONFIG) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							Log ("STATUS [ASK_CONFIG]\n");
							
							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;
							
							//int cfgSize = 0;
							
							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++;
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
							}
							
							//DEBUG
							Log ("parsed imei %ld, find socket id %d\n", imei, sockFd);
							
							//get config
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									int status = iter3->second.status.configStatus;
									
									//DEBUG
									Log ("status %d\n", status);
									
									if (status == FREE)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask CONFIG  on %ld\n", imei);

										iter3->second.status.configProgress = (int *)malloc (sizeof(int));
										iter3->second.status.configPointer = (int *)malloc (sizeof(int));
										
										*(iter3->second.status.configProgress) = 0;
										*(iter3->second.status.configPointer) = 0;

										//allocate memory for config needs
										iter3->second.status.configPtr = NULL;
										
										//get config contents
										Log ("Ask config from database\n");
										getConfigTo(imei, &(iter3->second.status.configPtr));

										//DEBUG
                                                                                if (iter3->second.status.configPtr == NULL){
                                                                                    Log ("GET SIZE: configSize = NULL\n" );
                                                                                    Log ("SEND NO CONFIG\n");
                                                                                    
                                                                                    //send answer to serv auth - error case
                                                                                    CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
                                                                                    
                                                                                } else {
                                                                                    Log ("GET SIZE: configSize = %d\n", strlen(iter3->second.status.configPtr));
                                                                                    Log ("SEND 1t PART OF CONFIG\n");
                                                                                    
                                                                                    //send first part to modem
                                                                                    if (sendConfigTo(sockFd, iter3->second.status.configPtr, strlen(iter3->second.status.configPtr), iter3->second.status.configPointer, iter3->second.status.configProgress) < 0)
                                                                                    {
                                                                                        goto pack_mark_bad;
                                                                                    }

                                                                                    iter3->second.lastTime = time(NULL);
                                                                                    iter3->second.waitFlags|=CMD_CONFIG;
                                                                                    iter3->second.status.configStatus = PROGRESS;

                                                                                    //send answer to serv auth
                                                                                    CHK(sendFoo(client, (char *)STARTED, strlen(STARTED), 0));
                                                                                }
										
                                                                                    
									}
									else if (status == PROGRESS)
									{
										//send answer to serv auth - in work
										memset(answ, 0, ANSW_SIZE);
										sprintf(answ, "%s;%d;", IN_WORK, *(iter3->second.status.configProgress));
										CHK(sendFoo(client, answ, strlen(answ), 0));
									}
									else if (status == READY)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask CONFIG answer READY\n");

										//send answer to serv auth - done + answer
										int sizePtr = strlen(iter3->second.status.configAnswer) + strlen(STOPED)+1+1;
										char * ptrToSend = (char *)malloc (sizePtr);
										memset (ptrToSend, 0, sizePtr);
										sprintf (ptrToSend, "%s;%s", STOPED, iter3->second.status.configAnswer);
										CHK(sendFoo(client, ptrToSend, strlen(ptrToSend), 0));
										iter3->second.status.configStatus = FREE;
										free(ptrToSend);
										ptrToSend = NULL;
										
										free(iter3->second.status.configPtr);
										free(iter3->second.status.configProgress);
										free(iter3->second.status.configPointer);
										iter3->second.status.configPtr = NULL;
										iter3->second.status.configProgress = NULL;
										iter3->second.status.configPointer = NULL;
									}
									else
									{
										//send answer to serv auth - error case
										CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
									}
								}
							} else {
								//DEBUG
								Log ("SERVER AUTH SCRIPT ASK CONFIG\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR MODEM\n");

								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
						}
						
						else if ((strstr(buf, ASK_REMOTE_PING) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							//Log ("STATUS [ASK_REMOTE_PING]\n");
						 	//Log ("buf is: %s\n", buf);
	
							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;
							char remoteId[32];
							int rsID = 0;
							
							//set remote Id to zero
							memset(remoteId, 0, 32);
							
							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++;
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
									
								//000000000011111111...
								//012345678901234567...
								//wtf remote id XX;1...
								memcpy(remoteId, buf, (comma-buf)-1);
								if (remoteId[strlen(remoteId)-1]=='\n')
									remoteId[strlen(remoteId)-1] = 0;
								if (remoteId[strlen(remoteId)-1]=='\r')
                                    remoteId[strlen(remoteId)-1] = 0;
								rsID = atoi(&remoteId[14]);

								//Log ("remoteId = %s\n", remoteId);
								//Log ("parse rs = %d\n", rsID);
							}
							
							//DEBUG
							//Log ("%ld is touched with [%d]\n", imei, rsID);
							
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									int status = iter3->second.status.remotePingStatus;
									
									//DEBUG
									//Log ("status %d\n", status);
									
									if ((status == FREE) && (rsID > 0))
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask REMOTE_PING on %ld\n", imei);

										//send command to modem
										memset(remoteId, 0, 32);
										sprintf(remoteId, "wtf remote id %d", rsID);
										CHK(sendFoo(sockFd, remoteId, strlen(remoteId), 0));
										
										iter3->second.lastTime = time(NULL);
										iter3->second.waitFlags|=CMD_REMOTE_PING;
										iter3->second.status.remotePingStatus = PROGRESS;
										
										//send answer to serv auth
										CHK(sendFoo(client, (char *)STARTED, strlen(STARTED), 0));
									}
									else if (status == PROGRESS)
									{
										//send answer to serv auth - in work
										CHK(sendFoo(client, (char *)IN_WORK, strlen(IN_WORK), 0));
									}
									else if (status == READY)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask REMOTE_PING answer READY\n");

										//send answer to serv auth - done + answer
										int sizePtr = strlen(iter3->second.status.remotePingAnswer) + strlen(STOPED)+1+1;
										char * ptrToSend = (char *)malloc (sizePtr);
										memset (ptrToSend, 0, sizePtr);
										sprintf (ptrToSend, "%s;%s", STOPED, iter3->second.status.remotePingAnswer);
										CHK(sendFoo(client, ptrToSend, strlen(ptrToSend), 0));
										iter3->second.status.remotePingStatus = FREE;
										free (ptrToSend);
										ptrToSend = NULL;
									}
									else
									{
										//send answer to serv auth - error case
										CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
									}
								}
							}
							else
							{
								//DEBUG
								Log ("SERVER AUTH SCRIPT ASK REMOTE_PING\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR REMOTE_PING\n");
								
								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
								
						}
						
						//
						// ASK POWER TURN
						//
						else if ((strstr(buf, ASK_POWER_TURN) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							//Log ("SERVICE AUTH SCRIPT [ASK_PLC]\n");
							
							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;
							long csn = 0;
							long tmode = 0;
							
							//
							//wtf power turn;imei;csn;mode
							                 
							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++; 
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
							}
							
							//parse counter sn
							comma = strstr(comma, ";");
							if (comma != NULL)
							{
								comma++; 
								csn = atol(comma);
							}
							
							//parse counter mode
							comma = strstr(comma, ";");
							if (comma != NULL)
							{
								comma++; 
								tmode = atol(comma);
							}
							
							//DEBUG
							//Log ("SERVICE AUTH SCRIPT [ASK_PLC imei %ld, socket id %d]\n", imei, sockFd);
							
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									int status = iter3->second.status.powerTurnStatus;
									
									//DEBUG
									//Log ("SERVICE AUTH SCRIPT [ASK_PLC status %d]\n", status);
									
									if (status == FREE)
									{
										//DEBUG
										Log("SERVER AUTH SCRIPT ask POWER TURN on %ld for counter %ld, mode %ld\n", imei, csn, tmode);

										//send command to modem
										//to do power turn send rule to irz modem
										char powerRule[256];
										memset(powerRule, 0, 256);
										sprintf (powerRule,"%s;%ld;%ld;", ASK_POWER_TURN, csn, tmode);
										
										//send
										CHK(sendFoo(sockFd, powerRule, strlen(powerRule), 0));
										
										iter3->second.lastTime = time(NULL);
										iter3->second.waitFlags|=CMD_POWER_TURN;
										iter3->second.status.powerTurnStatus = PROGRESS;
										
										//send answer to serv auth
										CHK(sendFoo(client, (char *)STARTED, strlen(STARTED), 0));
									}
									else if (status == PROGRESS)
									{
										//send answer to serv auth - in work
										CHK(sendFoo(client, (char *)STOPED, strlen(STOPED), 0));
									}
									else if (status == READY)
									{
										//DEBUG
										Log ("SERVER AUTH SCRIPT ask POWER TURN answer READY\n");

										//send answer to serv auth - done 
										if(strstr(iter3->second.status.powerTurnAnswer, POWER_TURN_ANSWER) !=NULL){
											CHK(sendFoo(client, (char *)IN_WORK, strlen(IN_WORK), 0));
										} else {
											CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
										}
										
										iter3->second.status.powerTurnStatus = FREE;
										
									}
									else
									{
										//send answer to serv auth - error case
										CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
									}
								}
							}
							else
							{
								//DEBUG
								Log ("SERVER AUTH SCRIPT ASK POWER TURN\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR MODEM\n");
								
								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
						}
						
						//
						// ASK CHANCEL ALL PREVIOUS COMMANDS
						//
						else if ((strstr(buf, ASK_CANCEL) != NULL) && (iter->second.serviceAuth == 1))
						{
							//DEBUG
							//Log ("STATUS [ASK_CANCEL]\n");
							
							//find sock Fd
							int sockFd = -1;
							char * comma = NULL;
							long imei = 0;
							
							//parse imei
							comma = strstr(buf, ";");
							if (comma != NULL)
							{
								comma++;
								imei = atol(comma);
								MapType2::iterator iter2 = clients_map2.begin();
								iter2 = clients_map2.find(imei);
								if (iter2 != clients_map2.end())
									sockFd = iter2->second;
							}
							
							//DEBUG
							//Log ("parsed imei %ld, find socket id %d\n", imei, sockFd);
							
							//get config
							if (sockFd > 0)
							{
								MapType::iterator iter3 = clients_map.begin();
								iter3 = clients_map.find(sockFd);
								if (iter3 != clients_map.end())
								{
									if (iter3->second.status.configStatus != FREE){
										iter3->second.status.configStatus = FREE;
										iter3->second.waitFlags-=CMD_CONFIG;
									}
									
									if (iter3->second.status.plcStatus != FREE){
										iter3->second.status.plcStatus = FREE;
										iter3->second.waitFlags-=CMD_PLC;
									}
									
									if (iter3->second.status.modemStatus != FREE){
										iter3->second.status.modemStatus = FREE;
										iter3->second.waitFlags-=CMD_MODEM;
									}
									
									if (iter3->second.status.powerTurnStatus != FREE){
										iter3->second.status.powerTurnStatus = FREE;
										iter3->second.waitFlags-=CMD_POWER_TURN;
									}
									
									//send answer to serv auth - error case
									CHK(sendFoo(client, (char *)STOPED, strlen(STOPED), 0));
								}
							}
							else
							{
								//DEBUG
								Log ("SERVER AUTH SCRIPT CANCEL\n");
								Log ("SERVER AUTH SCRIPT ERROR: SOCKET CLOSED FOR MODEM\n");
								
								//send answer to serv auth - error case
								CHK(sendFoo(client, (char *)ERROR, strlen(ERROR), 0));
							}
						}
						
						//
						// ANOTHER
						//
						else
						{
							if (debug == 0)
							{
								if (iter->second.serviceAuth == 0)
									Log ("SERVER ERROR:: unknown to handle %s\n", buf);
								//closeClient (client);
								goto pack_mark_bad;
							}
						}
					}
					break;
					
					default:
					{
						if (iter->second.serviceAuth == 0)
							Log ("SERVEVR CLOSE CASE 1\n");
						//closeClient (client);
						goto pack_mark_bad;
					}
					break;
					
				}
			}

			/* Get next token: */
			//ptr = strtok(NULL, "\r\n");
		}

	} 
	else 
	{
		//Log ("SERVEVR CLOSE CASE 2 (client side terminate connection)\n");
		//closeClient (client);
		goto pack_mark_bad;
	}

	
pack_check_time:
	//setup processing time on socket
	iter->second.lastPackTime = time(NULL);
	
	//go to exit of foo
	goto pack_proc_exit;
	
	
pack_mark_bad:	
	//so close sock and delete it from map
	//closeClient (client);
	len = -1;
	
pack_proc_exit:
	//free allocated char buffer
	if (buf != NULL){
		//debug
		//Log("free buf on exit\n");
		
		free(buf);
		buf = NULL;
	}
	
	//return len
    return len;
}

//
//
//
void setNonBlocking(int sock)
{
	/* Set the option NON BLOCKING active */
	int opts = fcntl(sock,F_GETFL);
	if (opts < 0) {
		perror("fcntl(F_GETFL)");
		exit(EXIT_FAILURE);
	}
	
	opts = (opts | O_NONBLOCK);
	if (fcntl(sock,F_SETFL,opts) < 0) {
		perror("fcntl(F_SETFL)");
		exit(EXIT_FAILURE);
	}
	
	/* Set the option KEEP ALIVE active */
	int optval = 1;
	int optlen = sizeof(optval);
	if(setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &optval, optlen) < 0) {
		perror("setsockopt(SO_KEEPALIVE)");
		exit(EXIT_FAILURE);
	}
	return;
}

//
//
//
void closeClient(int client)
{
	long imei = -1;
	MapType::iterator iter = clients_map.begin();
	MapType2::iterator iter2 = clients_map2.begin();
		
	//
	iter = clients_map.find(client);
	if (iter != clients_map.end()){
		imei = atol(iter->second.imei);
		//free buffers
		if (iter->second.socketBuffer != NULL){
			free (iter->second.socketBuffer);
		}
	}
	
	//if (iter->second.serviceAuth == 0)
	//	Log ("%ld CONNECTION CLOSED\n\n", imei);
	
	if (imei != -1)	
	{
		iter2 = clients_map2.find(imei);
		if (imei == (long)SERVICE_IMEI)
			serviceAuthConnection = 0;
		
		setOnlineState(iter->second.imei, 0);
	}
		
	close(client);
	
	if (iter2 != clients_map2.end())
		clients_map2.erase(iter2);
	
	if (iter != clients_map.end())
		clients_map.erase(iter);
}

//
//
//
int connectDb(void)
{
	connection_DB = PQconnectdb(DB_CONNECTION_SETTINGS);

	/* connecting to postgres data base */
	if ( PQstatus(connection_DB)  == CONNECTION_BAD)
	{
		Log("DB: Connection error\n");
		return -1;
	}
	else
	{
		Log("DB: Connection ok\n");
		resetPrevStates();
		return 0;
	}
}

//
//
//
int checkImei(char * imei)
{
	int resValue = -1;
	char SQL_request[256];
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select check_connected_modem('%s')", imei);
	result_DB_transaction  = PQexec(connection_DB, SQL_request);
	if ( PQresultStatus( result_DB_transaction ) == PGRES_TUPLES_OK )
	{
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );

		if( nrows == 1 || nfields == 1)
		{
			resValue = atoi( PQgetvalue( result_DB_transaction, 0, 0 ) );
		}
	}
	
	PQclear(result_DB_transaction);
	return resValue;
}

//
//
//
void resetPrevStates()
{
	char SQL_request[256];
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select * from reset_modem_states();");
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	PQclear(result_DB_transaction);
}

//
//
//
void saveActionToStatusTable(char * imei, char * counterId){
	char SQL_request[256];
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select * from set_modem_state_action('%s', '%s');", imei, counterId);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	PQclear(result_DB_transaction);
}

//
//
//
void saveSocketToStatusTable(char * imei, int socketId){
	char SQL_request[256];
	
	memset( SQL_request, 0, sizeof( SQL_request ) );
	sprintf(SQL_request, "select * from set_modem_state_socket('%s', %d);", imei, socketId);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	PQclear(result_DB_transaction);
}

//
//
//
int setOnlineState(char * imei, int online)
{
	int resValue = -1;
	char SQL_request[256];

	memset(SQL_request, 0, sizeof(SQL_request));
	sprintf(SQL_request, "select * from set_modem_online('%s','%d')", imei, online);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK)
	{
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );

		if( nrows == 1 || nfields == 1)
		{
			resValue = atoi(PQgetvalue(result_DB_transaction, 0, 0)) - 1;
		}
	}	
	PQclear(result_DB_transaction);
	
	if(online == 0)
		return saveDisconnected(imei);	
	else
		return saveConnected(imei);
}

//
//
//
int saveDisconnected(char * imei)
{
	int res = 1;
	char event[256];
	memset(event, 0, 256);
	sprintf(event, "disconnected from server");
	res = saveEventToDb(imei, 10, NULL, event);
	return res;
}


//
//
//
int saveConnected(char * imei)
{
	int res = 1;
	char event[256];
	memset(event, 0, 256);
	sprintf(event, "connected to server");
	res = saveEventToDb(imei, 9, NULL, event);
	return res;
}

//
//
//
int saveSyncResult(int secs, char * imei)
{
	int res = 1;
	char event[256];
	memset(event, 0, 256);
	
	if (secs > 0) 
	{	
		sprintf(event, "sync time for %d secs", secs);
		res = saveEventToDb(imei, 3, NULL, event);
	}

	return res;
}



//
//
//
int saveEventToDb(char * imei, int ev_type, char * ev_time, char * ev_text)
{
	int resValue = -1;
    char SQL_request[1024];

	int i = 0;
	int len = strlen(ev_text);
	while (i<len){
		if (ev_text[i] == '\''){
			ev_text[i] = ' ';
		}
		i++;
	}

    memset(SQL_request, 0, 1024);
	if (ev_time == NULL)
	{
	        sprintf(SQL_request, "select * from set_modem_event('%s','%d', NULL, '%s')", imei, ev_type, ev_text);
	} 
	else
	{		
		//move event type 1 up to 3 hours
		//timestamp '2012-01-01 23:00:00' + interval '3 hour'
		if ((ev_type != 9) && (ev_type != 8))
		{
			sprintf(SQL_request, "select * from set_modem_event('%s','%d', timestamp '%s', '%s')", imei, ev_type, ev_time, ev_text);
			//sprintf(SQL_request, "select * from set_modem_event('%s','%d', timestamp '%s' + interval '4 hours', '%s')", imei, ev_type, ev_time, ev_text);
		} 
		else
		{
			sprintf(SQL_request, "select * from set_modem_event('%s','%d', timestamp '%s', '%s')", imei, ev_type, ev_time, ev_text);
			//sprintf(SQL_request, "select * from set_modem_event('%s','%d', timestamp '%s' + interval '1 hour', '%s')", imei, ev_type, ev_time, ev_text);
		}
    }
	
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK)
	{
		int nrows = PQntuples( result_DB_transaction );
		int nfields = PQnfields( result_DB_transaction );

		if( nrows == 1 || nfields == 1)
		{
			resValue = atoi(PQgetvalue(result_DB_transaction, 0, 0));
		}
	}

	PQclear(result_DB_transaction);
	return resValue;	
}

//
//
//
int  getConfigTo (long imei, char ** config)
{
	int retValue = 0;
	char SQL_request[256];
	
	memset(SQL_request, 0, sizeof(SQL_request));
	sprintf(SQL_request, "select gcom.*, c.pulsar_chanel, c.pulsar_impulses, c.pulsar_address from get_counters_of_modem('%ld') gcom, \"COUNTER\" c where c.id = gcom.id", imei);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK)
	{
		int nrows = PQntuples(result_DB_transaction);
		int nfields = PQnfields(result_DB_transaction);
	
		//DEBUG
		//Log ("config get from db: nrows %d, nfields %d\n", nrows, nfields);
		
		if((nrows > 0) && (nfields > 0))
		{
			int configSize = 0;
			//int currentPos = 0;
			int portionSize = 0;
			char configPart[CONFIG_ROW_SIZE_MAX];
			
			//DEBUG
			//Log ("fill config\n");
		
			for (int row = 0; row < nrows; row++)
			{
				memset (configPart, 0, CONFIG_ROW_SIZE_MAX);

				int irzCounterType = 0;
				irzCounterType = atoi(PQgetvalue(result_DB_transaction, row, 1));
				
				if (irzCounterType!=14){
					sprintf (configPart, "%ld;%d;%s;%s;%s;%s;%s;%s;%s;\n", 
						atol(PQgetvalue(result_DB_transaction, row, 0)), 
						irzCounterType, 
						PQgetvalue(result_DB_transaction, row, 2), 
						PQgetvalue(result_DB_transaction, row, 3), 
						PQgetvalue(result_DB_transaction, row, 4), 
						PQgetvalue(result_DB_transaction, row, 5), 
						PQgetvalue(result_DB_transaction, row, 6), 
						PQgetvalue(result_DB_transaction, row, 7), 
						PQgetvalue(result_DB_transaction, row, 8));
						
				} else {
					sprintf (configPart, "%ld;%d;%s;%s;%s;%s;%s;%s;%s;%s;%s;%s;\n", 
						atol(PQgetvalue(result_DB_transaction, row, 0)), 
						irzCounterType, 
						PQgetvalue(result_DB_transaction, row, 2), 
						PQgetvalue(result_DB_transaction, row, 3), 
						PQgetvalue(result_DB_transaction, row, 4), 
						PQgetvalue(result_DB_transaction, row, 5), 
						PQgetvalue(result_DB_transaction, row, 6), 
						PQgetvalue(result_DB_transaction, row, 7), 
						PQgetvalue(result_DB_transaction, row, 8),
						PQgetvalue(result_DB_transaction, row, 9),
						PQgetvalue(result_DB_transaction, row, 10),
						PQgetvalue(result_DB_transaction, row, 11));
				}
					
				portionSize = strlen(configPart);
				(*config) = (char *)realloc ((*config), (portionSize + configSize) * sizeof(char));
				if ((*config)){
					memcpy(&(*config)[configSize], configPart, portionSize);
					configSize+=portionSize;
				}
			}
			
			retValue = configSize;
			
			//DEBUG
			//Log ("fill log\n");
			
			//DEBUG			
			//FILE * fp = fopen("log_config", "w");
			//if (fp!=NULL){
			//	fLog(fp, "%s", config);
			//	fclose(fp);
			//}
			
			//DEBUG
			//Log ("quit get\n");

		}
	}
	
	PQclear(result_DB_transaction);
	return retValue;
}

//
//
//
int  getConfigSize (long imei)
{
	char * testPtr;
	int retValue = 0;
	char SQL_request[256];
	
	memset(SQL_request, 0, sizeof(SQL_request));
	sprintf(SQL_request, "select * from get_counters_of_modem('%ld')", imei);
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK)
	{
		int nrows = PQntuples(result_DB_transaction);
		int nfields = PQnfields(result_DB_transaction);
	
		//DEBUG
		//Log ("config get from db: nrows %d, nfields %d\n", nrows, nfields);
		
		if((nrows > 0) && (nfields > 0))
		{
			int currentPos = 0;
			int currentPortion = 0;
			int totalSize = nrows * (CONFIG_ROW_SIZE_MAX);
			char configPart[CONFIG_ROW_SIZE_MAX];
			
			//DEBUG
			//Log ("try to alolocate mem [%d] bytes\n", totalSize);
			
			testPtr = (char *) malloc (totalSize);
			
			//DEBUG
			//Log ("allocate mem ok\n");
			
			if (testPtr != NULL)
			{
				memset (testPtr, 0, totalSize);
				
				//DEBUG
				//Log ("run calculation\n");
			
				for (int row = 0; row < nrows; row++)
				{
					memset (configPart, 0, CONFIG_ROW_SIZE_MAX);
										
					sprintf (configPart, "%ld;%d;%s;%s;%s;%s;%s;%s;%s;\n", 
						atol(PQgetvalue(result_DB_transaction, row, 0)), 
						atoi(PQgetvalue(result_DB_transaction, row, 1)), 
						PQgetvalue(result_DB_transaction, row, 2), 
						PQgetvalue(result_DB_transaction, row, 3), 
						PQgetvalue(result_DB_transaction, row, 4), 
						PQgetvalue(result_DB_transaction, row, 5), 
						PQgetvalue(result_DB_transaction, row, 6), 
						PQgetvalue(result_DB_transaction, row, 7), 
						PQgetvalue(result_DB_transaction, row, 8));
						
					currentPortion = strlen(configPart);
					memcpy(&testPtr[currentPos], configPart, currentPortion);
					currentPos+=currentPortion;
				}
				
				retValue = strlen(testPtr);
				free(testPtr);
			}
		}
	}
	
	PQclear(result_DB_transaction);
	return retValue;
}

//
//
//
void getConfigPart(char * configPart, PGresult*  result_DB_transaction, int row){
	sprintf (configPart, "%ld;%d;%s;%s;%s;%s;%s;%s;%s;\n", 
		atol(PQgetvalue(result_DB_transaction, row, 0)), 
		atoi(PQgetvalue(result_DB_transaction, row, 1)), 
		PQgetvalue(result_DB_transaction, row, 2), 
		PQgetvalue(result_DB_transaction, row, 3), 
		PQgetvalue(result_DB_transaction, row, 4), 
		PQgetvalue(result_DB_transaction, row, 5), 
		PQgetvalue(result_DB_transaction, row, 6), 
		PQgetvalue(result_DB_transaction, row, 7), 
		PQgetvalue(result_DB_transaction, row, 8));
}

//
//
//
int sendConfigTo (int client, char * config, int totalLen, int * pointer, int * progress)
{
	//char cmd[INPUT_SIZE];
	
	char * toSend = NULL;
	int toSendSize = 0;

	int notHead = 1;

	if ((config == NULL) || (totalLen == 0)) {
		Log ("EMPTY CONFIG\n");
		return -1;
	}

	if (((*pointer) == 0) && ((* progress) == 0)){
		notHead = 0;
	}

	toSend = &config[(*pointer)];
	toSendSize = strlen(toSend);
	if (toSendSize > MAX_CONFIG_SIZE_IN_ONE)
		toSendSize = MAX_CONFIG_SIZE_IN_ONE;
	
	(*pointer)+=toSendSize;
	(*progress)=(((*pointer) * 100)/ totalLen);
	if ((*pointer) == totalLen)
		(*progress)=100;
	
	//DEBUG
	Log ("\tCONFIG upload: %d/%d [%d] notHead %d\n", *pointer, totalLen, (*progress), notHead);
	
	int sizePtr = toSendSize + strlen(ASK_CONFIG)+6+1+1+30;
	char * ptrToSend = (char *)malloc (sizePtr);
	char * payload = (char *)malloc (toSendSize+1);
	memset (ptrToSend, 0, sizePtr);
	memset (payload, 0, toSendSize+1);
	memcpy (payload, toSend, toSendSize);
	
	sprintf (ptrToSend, "%s;%d;%d;%d;@%s@", ASK_CONFIG, *pointer, totalLen, notHead, payload);
	int res = sendFoo(client, ptrToSend, strlen(ptrToSend), 0);
	if (res < 0){
		toSendSize = -1;
	}
	free (ptrToSend);
	free (payload);
	ptrToSend = NULL;
	payload = NULL;
	
	return toSendSize;
}

//
//
//
int saveDataToDb(char * typeMeterage, char * timeStamp, meterage_t meterage, char * counterId)
{
	int retValue = -1;
	//char cmd[BUF_SIZE];
	char SQL_request[VALUES_SIZE*VALUES_COUNT];

	memset(SQL_request, 0, sizeof(SQL_request));
	sprintf(SQL_request, "select * from set_counter_values('%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s', '%s')",
															counterId, typeMeterage, timeStamp,
															meterage.values[0], meterage.values[1], meterage.values[2], meterage.values[3],
															meterage.values[4], meterage.values[5], meterage.values[6], meterage.values[7],
															meterage.values[8], meterage.values[9], meterage.values[10], meterage.values[11],
															meterage.values[12], meterage.values[13], meterage.values[14], meterage.values[15]);
	
	result_DB_transaction = PQexec(connection_DB, SQL_request);
	if (PQresultStatus(result_DB_transaction) == PGRES_TUPLES_OK)
	{
		int nrows = PQntuples(result_DB_transaction);
		int nfields = PQnfields(result_DB_transaction);
		
		if((nrows == 1) || (nfields == 1))
		{
			retValue = atoi(PQgetvalue(result_DB_transaction, 0, 0));
		}
	}
	PQclear(result_DB_transaction);
	
	return retValue;
}

//
//
//
void initMeterages(meterage_t * m)
{
	int initialValue = -1;
	memset(m, 0, sizeof(meterage_t));
	for (int i = 0; i < VALUES_COUNT; i++)
	{
		sprintf(m->values[i], "%d", initialValue);
	}
}

//
//
//
int getArrIndexByHeader(char * t)
{
	     if (strstr(t, "t1a+") != NULL) return 0;
	else if (strstr(t, "t2a+") != NULL) return 1;
	else if (strstr(t, "t3a+") != NULL) return 2;
	else if (strstr(t, "t4a+") != NULL) return 3;
	
	else if (strstr(t, "t1a-") != NULL) return 4;
	else if (strstr(t, "t2a-") != NULL) return 5;
	else if (strstr(t, "t3a-") != NULL) return 6;
	else if (strstr(t, "t24-") != NULL) return 7;
	
	else if (strstr(t, "t1r+") != NULL) return 8;
	else if (strstr(t, "t2r+") != NULL) return 9;
	else if (strstr(t, "t3r+") != NULL) return 10;
	else if (strstr(t, "t4r+") != NULL) return 11;
	
	else if (strstr(t, "t1r-") != NULL) return 12;
	else if (strstr(t, "t2r-") != NULL) return 13;
	else if (strstr(t, "t3r-") != NULL) return 14;
	else if (strstr(t, "t4r-") != NULL) return 15;
	
	else return -1;
}
