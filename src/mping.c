/*
Multicast Ping (mping) is a network administration utility used to test the
reachability of multiple hosts via multicast traffic.

Copyright (C) 2015 Alexis Green

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <sys/types.h>
#ifdef LINUX
#include <linux/types.h>
#else
#include <sys/types.h>
#endif
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/select.h>
#ifdef ANDROID
#include <linux/signal.h>
#else
#include <sys/signal.h>
#endif
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "uthash.h"

#define DEFAULT_SIZE 64
#define MAX_SIZE 65507
#define UDP_HEADER 28
#define DEFAULT_PORT 4322
#define DEFAULT_TTL 64
#define DEFAULT_MADDR "226.1.1.1"
#define MAX_ADDR_LEN 100

#ifndef SOL_IP
#define SOL_IP IPPROTO_IP
#endif

static struct in_addr local_iface;
static struct sockaddr_in group_sock, server_addr, client_addr;
static int sock_desc, addr_len, recv_bytes;
static int stop_count;
static char recv_buf[MAX_SIZE];
static char mcast_addr[MAX_ADDR_LEN];
static char local_addr[MAX_ADDR_LEN];
static char src_addr[MAX_ADDR_LEN];
static struct timeval delay;
static int send_sequence;
static int udp_port;
static int verbose;
static int send_ttl;
static int data_size;
static int sender;

static int recv_len = sizeof(recv_buf);

struct client_store {
	UT_hash_handle hh;
	unsigned long s_addr;
	int last_seq;
	int num_pongs;
	struct timeval min_rtt;
	struct timeval max_rtt;
	struct timeval total_time;
};

#pragma pack(push, 1)

struct ping_header {
	uint32_t sequence;
	uint32_t send_time_sec;
	uint32_t send_time_usec;
	uint32_t ttl;
};

struct ping_msg {
	struct ping_header hdr;
	char msg[MAX_SIZE];
};

#pragma pack(pop)

static struct ping_msg payload;
static struct ping_header* recv_ping_hdr;

static struct client_store* clients = NULL;

static
void finish(int ignore)
{

	if(sender)
	{
		signal(SIGINT, SIG_IGN);
		signal(SIGTERM, SIG_IGN);
		printf("\n");
		struct client_store * client;
		for (client=clients; client!=NULL; client=client->hh.next)
		{
			struct in_addr blah={.s_addr=client->s_addr};
			printf("%s: rtt min/avg/max: %lu.%03lu/%lu.%03lu/%lu.%03lu ms, loss: %d%% (%d packets)\n", inet_ntoa(blah),
									(client->min_rtt.tv_sec*1000000+client->min_rtt.tv_usec)/1000,
									(client->min_rtt.tv_sec*1000000+client->min_rtt.tv_usec)%1000,
									((client->total_time.tv_sec*1000000+client->total_time.tv_usec)/client->num_pongs)/1000,
									((client->total_time.tv_sec*1000000+client->total_time.tv_usec)/client->num_pongs)%1000,
									(client->max_rtt.tv_sec*1000000+client->max_rtt.tv_usec)/1000,
									(client->max_rtt.tv_sec*1000000+client->max_rtt.tv_usec)%1000,
									((send_sequence-client->num_pongs)*100/send_sequence),
									send_sequence-client->num_pongs);
		}
	}

	fflush(stdout);
	exit(0);
}

static
int run_client()
{
	struct ip_mreq group;
	struct ip_mreq_source group_source;
	struct msghdr msgh;
	struct cmsghdr *cmsg;
	struct iovec iov;
	char cbuf[1000];
	int err;
	int flags = 0;
	memset((char *) &client_addr, 0, sizeof(struct sockaddr_in));

	sock_desc = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock_desc < 0)
	{
		perror("Opening datagram socket for reception error");
		return 1;
	}

 	int reuse = 1;
	if(setsockopt(sock_desc, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
	{
		perror("Setting SO_REUSEADDR error");
		close(sock_desc);
		return 1;
	}
	
	int rec_ttl = 1;
	if (setsockopt(sock_desc, SOL_IP, IP_RECVTTL, (char *)&rec_ttl, sizeof(rec_ttl)) < 0 )
	{
		perror("Setting IP_RECVTTL error");
		close(sock_desc);
		return 1;
	}
	
	memset((char *) &group_sock, 0, sizeof(group_sock));
	group_sock.sin_family = AF_INET;
	group_sock.sin_port = htons(udp_port);
	group_sock.sin_addr.s_addr = INADDR_ANY;
	if(bind(sock_desc, (struct sockaddr*)&group_sock, sizeof(group_sock)))
	{
		perror("Binding datagram socket error");
		close(sock_desc);
		return 1;
	}

	if (strlen(src_addr) != 0) {
		group_source.imr_multiaddr.s_addr = inet_addr(mcast_addr);
		group_source.imr_sourceaddr.s_addr = inet_addr(src_addr);
		group_source.imr_interface.s_addr = inet_addr(local_addr);
		if(setsockopt(sock_desc, IPPROTO_IP, IP_ADD_SOURCE_MEMBERSHIP, (char *)&group_source, sizeof(group_source)) < 0)
		{
			perror("Adding multicast group error");
			close(sock_desc);
			return 1;
		}
	} else {
		group.imr_multiaddr.s_addr = inet_addr(mcast_addr);
		group.imr_interface.s_addr = inet_addr(local_addr);
		if(setsockopt(sock_desc, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char *)&group, sizeof(group)) < 0)
		{
			perror("Adding multicast group error");
			close(sock_desc);
			return 1;
		}
	}

	signal(SIGINT, finish);
	signal(SIGTERM, finish);

	while(1)
	{ 
		recv_len = sizeof(recv_buf);
		addr_len=sizeof(struct sockaddr);
		memset(&payload, 0, sizeof(struct ping_header));
		memset(&msgh, 0, sizeof(msgh));
		iov.iov_base        = recv_buf;
		iov.iov_len         = recv_len;
		msgh.msg_control    = cbuf;
		msgh.msg_controllen = sizeof(cbuf);
		msgh.msg_name       = &client_addr;
		msgh.msg_namelen    = (socklen_t)addr_len;
		msgh.msg_iov        = &iov;
		msgh.msg_iovlen     = 1;
		msgh.msg_flags      = 0;

		recv_bytes = recvmsg(sock_desc, &msgh, flags);
		if ( recv_bytes <= 0 )
		{
			perror("Failed to receive packet from the socket");
			continue;
		}
		for (cmsg = CMSG_FIRSTHDR(&msgh); cmsg != NULL; cmsg = CMSG_NXTHDR(&msgh,cmsg)) {
			if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_TTL) {
				payload.hdr.ttl=htonl(*((int *) CMSG_DATA(cmsg)));
				break;
			}
		}
		if (cmsg == NULL) 
		{
			perror("Failed to get TTL for packet");
			payload.hdr.ttl=htonl(DEFAULT_TTL);
		}
		
		// don't want to reply to 0.0.0.0 or myself
		if ( client_addr.sin_addr.s_addr != INADDR_ANY && client_addr.sin_addr.s_addr!=inet_addr(local_addr) ) 
		{
			recv_ping_hdr = (struct ping_header*) recv_buf;
			
			memset((char *) &server_addr, 0, sizeof(server_addr));
			server_addr.sin_family = AF_INET;
			server_addr.sin_port = htons(udp_port);
			server_addr.sin_addr.s_addr = client_addr.sin_addr.s_addr;
		
			// not doing ntoh and back since stuff gets sent back to network anyway
			payload.hdr.sequence=recv_ping_hdr->sequence;
			payload.hdr.send_time_sec=recv_ping_hdr->send_time_sec;
			payload.hdr.send_time_usec=recv_ping_hdr->send_time_usec;
			
			if (verbose == 1)
				printf("%d bytes from %s: seq=%d ttl=%d\n",
					recv_bytes,
					inet_ntoa(server_addr.sin_addr),
					ntohl(recv_ping_hdr->sequence),
					ntohl(payload.hdr.ttl));
			sendto(sock_desc, &payload, recv_bytes, 0, (struct sockaddr *)&server_addr, sizeof(struct sockaddr));
		}
	}
}

static
int run_server ()
{
	int wait_sequence = 0;
	fd_set master;
	fd_set read_fds;
	FD_ZERO(&master);
	FD_ZERO(&read_fds);
	struct timeval send_time, cur_time, timeout, elapsed, recv_time, payload_timeval, rtt;
	struct client_store* client;
	
	sock_desc = socket(AF_INET, SOCK_DGRAM, 0);
	if(sock_desc < 0)
	{
		perror("Opening datagram socket server error");
		return 1;
	}

	int reuse = 1;
	if(setsockopt(sock_desc, SOL_SOCKET, SO_REUSEADDR, (char *)&reuse, sizeof(reuse)) < 0)
	{
		perror("Setting SO_REUSEADDR error");
		close(sock_desc);
		return 1;
	}

	memset((char *) &group_sock, 0, sizeof(group_sock));
	group_sock.sin_family = AF_INET;
	group_sock.sin_addr.s_addr = inet_addr(mcast_addr);
	group_sock.sin_port = htons(udp_port);
 
	local_iface.s_addr = inet_addr(local_addr);
	if(setsockopt(sock_desc, IPPROTO_IP, IP_MULTICAST_IF, (char *)&local_iface, sizeof(local_iface)) < 0)
	{
		perror("Setting local interface error");
		return 1;
	}

	
	if(setsockopt(sock_desc, IPPROTO_IP, IP_MULTICAST_TTL, &send_ttl, sizeof(send_ttl) ) < 0)
	{
		perror("Setting ttl error");
		return 1;
	}

	memset((char *) &server_addr, 0, sizeof(struct sockaddr_in));
	server_addr.sin_port = htons(udp_port);
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr= htonl(INADDR_ANY);

	if ( bind(sock_desc, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0 )
	{
		perror("Failed to bind server socket");
		close(sock_desc);
		return 1;
	}

	signal(SIGINT, finish);
	signal(SIGTERM, finish);

	send_sequence=0;
	FD_SET(sock_desc, &master);
	addr_len=sizeof(struct sockaddr);

	gettimeofday(&send_time, NULL);

	timeout.tv_sec=0;
	timeout.tv_usec=0;

	printf("MPING %s %d(%d) bytes of data.\n", mcast_addr, data_size, data_size+UDP_HEADER);
		
	int ret;
	while(1)
	{
		read_fds=master;
	
		if ( (ret=select(sock_desc+1, &read_fds, NULL, NULL, &timeout)) > 0 )
		{
				if ((recv_bytes=recvfrom(sock_desc, recv_buf, recv_len, 0, (struct sockaddr *)&client_addr, (socklen_t *)&addr_len)) !=0)
				{
					gettimeofday(&recv_time, NULL);
					recv_ping_hdr = (struct ping_header *)recv_buf;
					unsigned long s_addr = client_addr.sin_addr.s_addr;
					HASH_FIND(hh, clients, &(s_addr) ,sizeof(s_addr) , client);				
					if (!client)
					{
						signal(SIGINT, SIG_IGN);
						signal(SIGTERM, SIG_IGN);
						client = (struct client_store*)calloc(1, sizeof(*client));
						client->s_addr=s_addr;
						client->last_seq=0;
						client->last_seq=0;
						client->min_rtt.tv_sec=10000;
						client->min_rtt.tv_usec=0;
						client->total_time.tv_sec=0;
						client->total_time.tv_usec=0;
						client->max_rtt.tv_sec=0;
						client->max_rtt.tv_usec=0;
						HASH_ADD(hh, clients, s_addr, sizeof(s_addr), client);
						signal(SIGINT, finish);
						signal(SIGTERM, finish);
					}
					
					payload_timeval.tv_sec=ntohl(recv_ping_hdr->send_time_sec);
					payload_timeval.tv_usec=ntohl(recv_ping_hdr->send_time_usec);
					
					timersub(&recv_time, &payload_timeval, &rtt);
					
					if (timercmp(&rtt, &(client->min_rtt), <))
						client->min_rtt=rtt;
					if (timercmp(&rtt, &(client->max_rtt), >))
						client->max_rtt=rtt;
					client->num_pongs=client->num_pongs+1;
					timeradd(&(client->total_time),&rtt, &(client->total_time));
					client->last_seq=(int)(ntohl(recv_ping_hdr->sequence));
					
					if (verbose == 1)
						printf("%d bytes from %s: seq=%d ttl=%d time=%ld.%03ld ms\n",
							   recv_bytes,
							   inet_ntoa(client_addr.sin_addr),
							   ntohl(recv_ping_hdr->sequence),
							   ntohl(recv_ping_hdr->ttl),
							   ((rtt.tv_sec*1000000+rtt.tv_usec)/1000),
							   ((rtt.tv_sec*1000000+rtt.tv_usec)%1000));
				}
		}
		else if (ret == -1)
		{
			perror("Select failure");
			close(sock_desc);
			return 1;
		}
		
		gettimeofday(&cur_time, NULL);
	
		timersub(&cur_time, &send_time, &elapsed);
		if( timercmp(&elapsed, &timeout, >=) )
		{
			if (stop_count !=0 && send_sequence >= stop_count) {
				if (wait_sequence != 0) {
					finish(0);
				} else {
					gettimeofday(&send_time, NULL);
					delay.tv_sec=2;
		                        delay.tv_usec=0;
					timeout=delay;
					wait_sequence=1;
				}
			}

			memset((char *) &payload, 0, sizeof(struct ping_header));
			payload.hdr.sequence=htonl(send_sequence);
			payload.hdr.send_time_sec=htonl(cur_time.tv_sec);
			payload.hdr.send_time_usec=htonl(cur_time.tv_usec);
			payload.hdr.ttl=0;
			
			if(sendto(sock_desc, &payload, data_size, 0, (struct sockaddr*)&group_sock, sizeof(group_sock)) < 0)
				perror("Sending datagram message error");
			
			send_sequence+=1;
			gettimeofday(&send_time, NULL);
			timeout=delay;
		}
		else
			timersub(&timeout, &elapsed, &timeout);
	}
}

static
void usage(int return_code)
{
	// TODO:
	// would be good to have -w
	printf("Usage:\tmping -l -I <interface address> [-p <udp port>] [-g <multicast group[/source]>] [-q]\n");
	printf("\tmping -s -I <interface address> [-p <udp port>] [-g <multicast group>] ");
	printf("[-i <interval in ms>] [-c <count>] [-T TTL ] [-q] [-S payload size]\n");
	printf("\tDefaults: interval - 1 second, multicast group - %s , udp port - %d\n", DEFAULT_MADDR, DEFAULT_PORT );
	exit(return_code);
}

int main (int argc, char *argv[ ])
{
	int c;
	
	char *address=NULL;
	char *group=NULL;
	char *port=NULL;
	char *interval=NULL;
	char *count=NULL;
	char *ttl_string=NULL;
	char *size_string=NULL;
	int listener=0;
	
	extern char *optarg;
	extern int optind, optopt;


	if (argc==1)
		usage(0);
	
	sender=0;
	verbose=1;

        char pnn9seq[511] = {
        0x87, 0xb8, 0x59, 0xb7, 0xa1, 0xcc, 0x24, 0x57, 0x5e, 0x4b, 0x9c, 0xe,
        0xe9, 0xea, 0x50, 0x2a, 0xbe, 0xb4, 0x1b, 0xb6, 0xb0, 0x5d, 0xf1, 0xe6,
        0x9a, 0xe3, 0x45, 0xfd, 0x2c, 0x53, 0x18, 0xc,  0xca, 0xc9, 0xfb, 0x49,
        0x37, 0xe5, 0xa8, 0x51, 0x3b, 0x2f, 0x61, 0xaa, 0x72, 0x18, 0x84, 0x2,
        0x23, 0x23, 0xab, 0x63, 0x89, 0x51, 0xb3, 0xe7, 0x8b, 0x72, 0x90, 0x4c,
        0xe8, 0xfb, 0xc1, 0xff, 0xf,  0x70, 0xb3, 0x6f, 0x43, 0x98, 0x48, 0xae,
        0xbc, 0x97, 0x38, 0x1d, 0xd3, 0xd4, 0xa0, 0x55, 0x7d, 0x68, 0x37, 0x6d,
        0x60, 0xbb, 0xe3, 0xcd, 0x35, 0xc6, 0x6f, 0xcb, 0x50, 0xa2, 0x76, 0x5e,
        0xc3, 0x54, 0xe4, 0x31, 0x8,  0x4,  0x46, 0x47, 0x56, 0xc7, 0x12, 0xa3,
        0x67, 0xcf, 0xde, 0x87, 0x30, 0x91, 0x5d, 0x79, 0x2e, 0x70, 0x3b, 0xa7,
        0xa9, 0x8d, 0x17, 0xf4, 0xb1, 0x4c, 0x60, 0x33, 0x2b, 0x27, 0xed, 0x24,
        0xdf, 0x96, 0xa1, 0x44, 0xec, 0xbd, 0x86, 0xa9, 0xc8, 0x62, 0x10, 0x8,
        0x8c, 0x8e, 0xad, 0x8e, 0x25, 0x46, 0xcf, 0x9e, 0x2d, 0xca, 0x41, 0x33,
        0xa3, 0xef, 0x7,  0xfc, 0x3d, 0xc2, 0xcd, 0xbd, 0xe,  0x61, 0x22, 0xba,
        0xf2, 0x5c, 0xe0, 0x77, 0x4f, 0x52, 0x81, 0x55, 0xf5, 0xa0, 0xdd, 0xb5,
        0x82, 0xef, 0x8f, 0x34, 0xd7, 0x1a, 0x2f, 0xe9, 0x62, 0x98, 0xc0, 0x66,
        0x56, 0x4f, 0xda, 0x49, 0xbf, 0x2d, 0x42, 0x89, 0xd9, 0x7b, 0xd,  0x53,
        0x90, 0xc4, 0x20, 0x11, 0x19, 0x1d, 0x5b, 0x1c, 0x4a, 0x8d, 0x9f, 0x3c,
        0x5b, 0x94, 0x82, 0x67, 0x47, 0xde, 0xf,  0xf8, 0x7b, 0x85, 0x9b, 0x7a,
        0x1c, 0xc2, 0x45, 0x75, 0xe4, 0xb9, 0xc0, 0xee, 0x9e, 0xa5, 0x2,  0xab,
        0xeb, 0x41, 0xbb, 0x6b, 0x5,  0xdf, 0x1e, 0x69, 0xae, 0x34, 0x5f, 0xd2,
        0xc5, 0x31, 0x80, 0xcc, 0xac, 0x9f, 0xb4, 0x93, 0x7e, 0x5a, 0x85, 0x13,
        0xb2, 0xf6, 0x1a, 0xa7, 0x21, 0x88, 0x40, 0x22, 0x32, 0x3a, 0xb6, 0x38,
        0x95, 0x1b, 0x3e, 0x78, 0xb7, 0x29, 0x4,  0xce, 0x8f, 0xbc, 0x1f, 0xf0,
        0xf7, 0xb,  0x36, 0xf4, 0x39, 0x84, 0x8a, 0xeb, 0xc9, 0x73, 0x81, 0xdd,
        0x3d, 0x4a, 0x5,  0x57, 0xd6, 0x83, 0x76, 0xd6, 0xb,  0xbe, 0x3c, 0xd3,
        0x5c, 0x68, 0xbf, 0xa5, 0x8a, 0x63, 0x1,  0x99, 0x59, 0x3f, 0x69, 0x26,
        0xfc, 0xb5, 0xa,  0x27, 0x65, 0xec, 0x35, 0x4e, 0x43, 0x10, 0x80, 0x44,
        0x64, 0x75, 0x6c, 0x71, 0x2a, 0x36, 0x7c, 0xf1, 0x6e, 0x52, 0x9,  0x9d,
        0x1f, 0x78, 0x3f, 0xe1, 0xee, 0x16, 0x6d, 0xe8, 0x73, 0x9,  0x15, 0xd7,
        0x92, 0xe7, 0x3,  0xba, 0x7a, 0x94, 0xa,  0xaf, 0xad, 0x6,  0xed, 0xac,
        0x17, 0x7c, 0x79, 0xa6, 0xb8, 0xd1, 0x7f, 0x4b, 0x14, 0xc6, 0x3,  0x32,
        0xb2, 0x7e, 0xd2, 0x4d, 0xf9, 0x6a, 0x14, 0x4e, 0xcb, 0xd8, 0x6a, 0x9c,
        0x86, 0x21, 0x0,  0x88, 0xc8, 0xea, 0xd8, 0xe2, 0x54, 0x6c, 0xf9, 0xe2,
        0xdc, 0xa4, 0x13, 0x3a, 0x3e, 0xf0, 0x7f, 0xc3, 0xdc, 0x2c, 0xdb, 0xd0,
        0xe6, 0x12, 0x2b, 0xaf, 0x25, 0xce, 0x7,  0x74, 0xf5, 0x28, 0x15, 0x5f,
        0x5a, 0xd,  0xdb, 0x58, 0x2e, 0xf8, 0xf3, 0x4d, 0x71, 0xa2, 0xfe, 0x96,
        0x29, 0x8c, 0x6,  0x65, 0x64, 0xfd, 0xa4, 0x9b, 0xf2, 0xd4, 0x28, 0x9d,
        0x97, 0xb0, 0xd5, 0x39, 0xc,  0x42, 0x1,  0x11, 0x91, 0xd5, 0xb1, 0xc4,
        0xa8, 0xd9, 0xf3, 0xc5, 0xb9, 0x48, 0x26, 0x74, 0x7d, 0xe0, 0xff};

	int copied_bytes = 0;
	while( copied_bytes < MAX_SIZE ) {
		if (copied_bytes + 511 < MAX_SIZE) {
			memcpy(payload.msg + copied_bytes, pnn9seq, 511);
			copied_bytes += 511;
		} else {
			memcpy(payload.msg + copied_bytes, pnn9seq, MAX_SIZE - copied_bytes);
			copied_bytes = MAX_SIZE;
		}
	}

	while (( c = getopt(argc, argv, "c:g:hi:lp:qsI:S:T:V")) !=-1)
	{
		switch(c)
		{
			case 'c':
				count=optarg;
				break;
			case 'I':
				address=optarg;
				break;
			case 'i':
				interval=optarg;
				break;
			case 'g':
				group=optarg;
				break;
			case 'h':
				usage(0);
			case 'p':
				port=optarg;
				break;
			case 'q':
				verbose=0;
				break;
			case 'l':
				listener=1;
				break;
			case 's':
				sender=1;
				break;
			case 'T':
				ttl_string=optarg;
				break;
			case 'S':
				size_string=optarg;
				break;
			case 'V':
				printf("mping version 0.1\n");
				exit(0);
			case '?':
				usage(0);
			default:
				abort();
		}
	}
	
	if (!address)
	{
		fprintf(stderr, "Must specify local interface address.\n");
		usage(1);
	}
	else
	{
		in_addr_t addr;
		snprintf(local_addr, MAX_ADDR_LEN, "%s",address);
		addr=inet_addr(local_addr);
		if (addr == ( in_addr_t)(-1))
		{
			fprintf(stderr, "Invalid address specified for local interface\n");
			usage(1);
		}
		
	}
	
	if (!group)
		snprintf(mcast_addr, MAX_ADDR_LEN, "%s", DEFAULT_MADDR);
	else
	{
		in_addr_t group_addr;
		char *source=NULL;
		char *groupp = group;
	    char *grp;

		grp = strsep(&groupp, "/");
		if (groupp != NULL) {
			source = strsep(&groupp, "/");
			snprintf(src_addr, MAX_ADDR_LEN, "%s",source);
		}
		group = grp;

		snprintf(mcast_addr, MAX_ADDR_LEN, "%s",group);

		group_addr=inet_addr(mcast_addr);
		if ( group_addr == (in_addr_t)(-1) || !IN_MULTICAST(ntohl(group_addr)))
		{
			fprintf(stderr, "Invalid address specified for multicast group - address must be multicast.\n");
			usage(1);
		}
	}
	
	if (!port)
		udp_port=DEFAULT_PORT;
	else
	{
		udp_port=atoi(port);
		if (udp_port < 1024 || udp_port > 65535)
		{
			fprintf(stderr, "Invalide port set - must be between 1024 and 65535.\n");
			usage(1);
		}
		
	}
	
	
	if (listener)
	{
		if (sender)
		{
			fprintf(stderr, "Unable to be sender and receiver at the same time.\n");
			usage(1);
		}
		
		if (interval != NULL)
		{
			fprintf(stderr, "Unable to set interval when in receive mode.\n");
			usage(1);
		}
		if (count != NULL)
		{
			fprintf(stderr, "Unable to set ping count in receiver mode.\n");
			usage(1);
		}
		if (ttl_string != NULL)
		{
			fprintf(stderr, "Unable to set TTL in receiver mode.\n");
			usage(1);
		}
		if (size_string != NULL)
		{
			fprintf(stderr, "Unable to set payload size in receiver mode.\n");
			usage(1);
		}
		return run_client();
	}
	
	if (sender)
	{
		if (!interval)
		{
			delay.tv_sec=1;
			delay.tv_usec=0;
		}
		else
		{
			double secs = strtod(interval, NULL)/1000;		// returns zero on errors
			if (secs > 0.0)
			{
				delay.tv_sec = (long) secs;
				delay.tv_usec = (long) (1e6*(secs - delay.tv_sec));
			}
			else
			{
				fprintf(stderr, "Invalid interval set - must be greater than 0.\n");
				usage(1);
			}
		}

		if(!count)
			stop_count=0;
		else
		{
			stop_count=atoi(count);
			if (stop_count <= 0 || stop_count > 65535)
			{
				fprintf(stderr, "Invalid stop count.\n");
				usage(1);
			}
		}

		if(!ttl_string)
			send_ttl=DEFAULT_TTL;
		else
		{
			send_ttl=atoi(ttl_string);
			if (send_ttl <= 0 || send_ttl > 255)
			{
				fprintf(stderr, "Invalid TTL value - must be between 1 and 255.\n");
				usage(1);
			}
		}
		
		if(!size_string)
			data_size=DEFAULT_SIZE;
		else
		{
			data_size=atoi(size_string);
			if (data_size < sizeof(struct ping_header) || data_size > MAX_SIZE)
			{
				fprintf(stderr, "Invalid payload size value - must be between %d and %d.\n", (int)(sizeof(struct ping_header)), DEFAULT_SIZE);
				usage(1);
			}
		}	
				
		return run_server();
	
	}
	
	fprintf(stderr, "Must select sender or receiver mode.\n");
	usage(1);
	return 0;
}
