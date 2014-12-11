/*
 * File: netspray.c
 * Implements: network connectivity tester.
 *
 * Copyright: Jens Låås Uppsala University, 2014
 * Copyright license: According to GPL, see file COPYING in this directory.
 *
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "jelist.h"
#include "jelopt.h"

struct ip {
	struct sockaddr_in addr;
	struct timeval ts[100]; /* circular buffer */
	int rate;
	int pos; /* position in ts array */
	int last; /* last position */
	int recoverycounter; /* count down for recovery */
	uint64_t fail; /* nr of failures detected */
	uint64_t count; /* total number of packets processed */
	uint8_t data[4];
};

struct {
	int verbose;
	int fd;
	int nr_allow_loss;
	int recoverycount;
	int foreground;
} conf;

void daemonize(void)
{
	pid_t pid, sid;
	int fd;

	/* already a daemon */
	if ( getppid() == 1 ) return;

	if((pid = fork()) < 0)
		exit(-1);

	if (pid > 0) _exit(0);

	umask(0);

	if((sid = setsid()) < 0)
		exit(-1);

	if ((chdir("/")) < 0)
		exit(-1);

	if((fd = open("/dev/null", O_RDWR, 0)) >= 0) {
		if(fd>2) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
		}
		if(fd) close(fd);
	}
}

int logmsg(struct ip *ip, char *msg)
{
	if(conf.verbose) fprintf(stderr, "%s: %s\n", inet_ntoa(ip->addr.sin_addr), msg);
	return 0;
}

int ip_status_fail(struct ip *ip)
{
	if(ip->fail == 0) logmsg(ip, "connectivity failed");
	if(ip->fail && ((ip->fail % (conf.recoverycount?conf.recoverycount:100)) == 0))
		logmsg(ip, "connectivity still failed");
	ip->fail++;
	ip->recoverycounter = conf.recoverycount;
	if(conf.verbose) fprintf(stderr, "%s: fail = %llu\n", inet_ntoa(ip->addr.sin_addr), ip->fail);
	return ip->fail;
}

int ip_status_ok(struct ip *ip)
{
	if(ip->fail) {
		logmsg(ip, "reconnected");
	}
	ip->fail = 0;
	if(ip->recoverycounter) {
		ip->recoverycounter--;
		if(ip->recoverycounter == 0) {
			if(conf.verbose) fprintf(stderr, "%s: recovered\n", inet_ntoa(ip->addr.sin_addr));
			logmsg(ip, "connectivity recovered");
		}
	}
	if(conf.verbose) fprintf(stderr, "%s: fail = %llu recover=%d\n",
				 inet_ntoa(ip->addr.sin_addr), ip->fail, ip->recoverycounter);
	return ip->fail;
}

void receiver(struct jlhead *ips)
{
	struct pollfd fds[2];
	struct sockaddr_in from_addr;
	struct ip *ip;
	unsigned int fromlen;
	struct timeval ts, checkts;
	int got, rc;
	uint8_t buf[4];

	fromlen = sizeof( from_addr );

	fds[0].fd = conf.fd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	gettimeofday(&checkts, NULL);
	checkts.tv_sec+=5; /* warmup a few seconds */

	while(1) {
		rc = poll(fds, 1, 500); /* timeout 500ms */
		if( (rc > 0) && fds[0].revents) {
			got = recvfrom( conf.fd, buf, sizeof(buf), 0,
					(struct sockaddr *)&from_addr, &fromlen);
			if(got < 0) continue;
			gettimeofday(&ts, NULL);
			if(conf.verbose) printf("got packet from %s port %d\n",
						inet_ntoa(from_addr.sin_addr),
						from_addr.sin_port);
			jl_foreach(ips, ip) {
				if(ip->addr.sin_addr.s_addr == from_addr.sin_addr.s_addr) {
					ip->count++;
					memcpy(&ip->ts[ip->pos], &ts, sizeof(struct timeval));
					if(conf.verbose) printf("timestamped %lu.%lu\n", ip->ts[ip->pos].tv_sec, ip->ts[ip->pos].tv_usec);
					ip->last = ip->pos;
					ip->pos++;
					if(ip->pos >= 100) ip->pos = 0;
					ip->rate = (buf[2] << 8) + buf[3];
					break;
				}
			}
		} else {
			gettimeofday(&ts, NULL);
		}
		
		/* limit check interval */
		if(checkts.tv_sec > ts.tv_sec) {
			continue;
		}
		memcpy(&checkts, &ts, sizeof(checkts));
		checkts.tv_sec++; /* wait one second until next check */
		
		jl_foreach(ips, ip) {
			if(ip->count == 0) {
				ip_status_fail(ip);
				continue;
			}
			
			/* compare ts (now) with time of last packet.
			 *  limit: 1/rate * nr_allow_loss
			 */
			{
				struct timeval cts;
				uint64_t maxtime;
				
				memcpy(&cts, &ip->ts[ip->last], sizeof(cts));
				maxtime = ((uint64_t)1000000/ip->rate)*conf.nr_allow_loss;
				cts.tv_sec += (maxtime/1000000);
				cts.tv_usec += (maxtime%1000000);
				if(cts.tv_usec > 1000000) {
					cts.tv_usec -= 1000000;
					cts.tv_sec++;
				}
				if(conf.verbose) printf("ts %lu.%lu\n", ts.tv_sec, ts.tv_usec);
				if(conf.verbose) printf("cts %lu.%lu\n", cts.tv_sec, cts.tv_usec);
				if(cts.tv_sec > ts.tv_sec) {
					ip_status_ok(ip);
					continue;
				}
				if(cts.tv_sec < ts.tv_sec) {
					ip_status_fail(ip);
					continue;
				}
				if(cts.tv_usec < ts.tv_usec) {
					ip_status_fail(ip);
					continue;
                                }
				ip_status_ok(ip);
			}
		}
	}
}

void sender(int port, struct jlhead *ips, int rate)
{
	struct ip *ip;
        ssize_t rc;
	struct timeval now, next;
	uint64_t step = 1000000/rate;
	
	if(gettimeofday(&now, NULL)) {
		if(conf.verbose) fprintf(stderr, "gettimeofday failed\n");
	}
	
	memcpy(&next, &now, sizeof(now));
	
	while(1) {
		next.tv_usec += step;
		if(next.tv_usec > (uint64_t)1000000) {
			next.tv_sec++;
			next.tv_usec -= (uint64_t)1000000;
		}
		jl_foreach(ips, ip) {
			rc = sendto( conf.fd, ip->data, 4, 0,
				     (struct sockaddr *)&ip->addr, sizeof(struct sockaddr_in));
			if(rc >= 0)
				ip->count++;
			if(rc == -1) {
				if(conf.verbose) fprintf(stderr, "sendto() failed\n");
			}
		}

		if(gettimeofday(&now, NULL)) {
			if(conf.verbose) fprintf(stderr, "gettimeofday failed\n");
		}
		if( (now.tv_sec > next.tv_sec) ||
		    ( (now.tv_sec == next.tv_sec) && (now.tv_usec > next.tv_usec) ) ) {
			if(conf.verbose) fprintf(stderr, "failed to keep up send rate\n");
			memcpy(&next, &now, sizeof(now));
			continue;
		}
		{
			struct timespec req;
			req.tv_sec = next.tv_sec - now.tv_sec;
			if(now.tv_usec > next.tv_usec) {
				req.tv_nsec = ((uint64_t)1000000 - now.tv_usec) + next.tv_usec;
				req.tv_sec--;
			} else {
				req.tv_nsec = next.tv_usec - now.tv_usec;
			}
			
			req.tv_nsec *= (uint64_t)1000;
			if(-1 == nanosleep(&req, NULL)) {
				if(conf.verbose) fprintf(stderr, "nanosleep failed\n");
			}
		}
	}
}

int main(int argc, char **argv)
{
	int err = 0;
	int port = 1234;
	int rate = 10;
	char *bindaddr = NULL;
	struct in_addr srcaddr;
	struct jlhead *ips;

	ips = jl_new();
	conf.nr_allow_loss = 3;
	conf.recoverycount = 200;
	srcaddr.s_addr = htonl(INADDR_ANY);
	
	if(jelopt(argv, 'h', "help", 0, &err)) {
	usage:
		printf("netspray [-v] rx|tx IP [IP]*\n"
		       " -b --bind ADDR  optional bind address\n"
		       " -v              be verbose (also keeps process in foreground)\n"
		       " -r --rate       packets per second [10]\n"
		       " -p --port N     port number to use [1234]\n"
		       " -l --loss N     packet loss trigger level [3]\n"
		       " -R --rcount     recoverycount until recovered [200]\n"
		       " -F              stay in foreground (no daemon)\n"
		       "\n"
		       "Netspray has two modes: 'rx' and 'tx'\n"
		       "\n"
		       "rx mode is the receiver mode.\n"
		       "The receiver will expect packets from the IP-addresses listed.\n"
		       "If sequential packet loss is detected the alarm function will trigger.\n"
		       "\n"
		       "tx mode is the transmitter mode.\n"
		       "The transmitter will send packets to all IP-addresses listed at the given rate.\n"
		       "\n"
		       "It is possible to bind to an adress that is not (yet) configured on the system.\n"
			);
		exit(0);
	}
	
	while(jelopt(argv, 'v', "verbose", 0, &err)) conf.verbose++;
	while(jelopt(argv, 'F', (void*)0, 0, &err)) conf.foreground=1;
	while(jelopt(argv, 'b', "bind", &bindaddr, &err));
	while(jelopt_int(argv, 'r', "rate", &rate, &err));
	while(jelopt_int(argv, 'p', "port", &port, &err));
	while(jelopt_int(argv, 'l', "loss", &conf.nr_allow_loss, &err));
	while(jelopt_int(argv, 'R', "rcount", &conf.recoverycount, &err));
	argc = jelopt_final(argv, &err);
	if(err) {
		fprintf(stderr, "netspray: Syntax error in options.\n");
		exit(2);
	}

	if(argc <= 2) {
		fprintf(stderr, "netspray: unsufficient args.\n");
		exit(2);
	}
	if(strcmp(argv[1], "rx") && strcmp(argv[1], "tx")) {
		fprintf(stderr, "netspray: only tx|rx accepted\n");
		exit(2);
	}

	while(argc > 2) {
		struct in_addr dst;
		struct ip *ip;

		if(conf.verbose) printf("ip %s rate %d pkts/s\n", argv[argc-1], rate);
		if(!inet_aton(argv[argc-1], &dst)) {
			fprintf(stderr, "netspray: Address not valid\n");
			exit(1);
		}
		ip = malloc(sizeof(struct ip));
		memset(ip, 0, sizeof(struct ip));
		ip->addr.sin_family = AF_INET;
		ip->addr.sin_port = htons(port);
		ip->addr.sin_addr.s_addr = dst.s_addr;
		ip->rate = rate;
		ip->data[0] = 'n';
		ip->data[1] = '1';
		ip->data[2] = ip->rate >> 8;
		ip->data[3] = ip->rate & 0xff;
		jl_append(ips, ip);
		argc--;
	}

	{
		conf.fd = socket( PF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if(conf.fd == -1) {
			fprintf(stderr, "netspray: UDP socket creation failed\n");
			exit(2);
		}
	}

	if(bindaddr) {
		int one=1;
		if(!inet_aton(bindaddr, &srcaddr)) {
			fprintf(stderr, "netspray: Bind address not valid\n");
			exit(1);
		}
		setsockopt(conf.fd, IPPROTO_IP, IP_FREEBIND, (char *)&one, sizeof(one));
	}

	if(!strcmp(argv[1], "rx")) {
		struct sockaddr_in my_addr;
		memset( &my_addr, 0, sizeof(my_addr));
		my_addr.sin_family = AF_INET;
		my_addr.sin_addr.s_addr = srcaddr.s_addr;
		my_addr.sin_port = htons(port);
		bind(conf.fd, (struct sockaddr *)&my_addr, sizeof( struct sockaddr_in) );

		if((!conf.verbose) && (!conf.foreground)) daemonize();
		receiver(ips);
		exit(1);
	}
	if(!strcmp(argv[1], "tx")) {
		struct sockaddr_in my_addr;
		memset( &my_addr, 0, sizeof(my_addr));
		my_addr.sin_family = AF_INET;
		my_addr.sin_addr.s_addr = srcaddr.s_addr;
		my_addr.sin_port = 0;
		bind(conf.fd, (struct sockaddr *)&my_addr, sizeof( struct sockaddr_in) );
		
		if((!conf.verbose) && (!conf.foreground)) daemonize();
		sender(port, ips, rate);
		exit(1);
	}
	return 2;
}
