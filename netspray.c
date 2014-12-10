/*

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
	uint64_t fail; /* nr of failures detected */
	uint64_t count; /* total number of packets processed */
	uint8_t data[4];
};

struct {
	int verbose;
	int fd;
	int nr_allow_loss;
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
			if(conf.verbose) printf("got packet from %u\n", from_addr.sin_addr.s_addr);
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
				ip->fail++;
				printf("fail = %llu\n", ip->fail);
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
					ip->fail = 0;
					printf("fail = %llu\n", ip->fail);
					continue;
				}
				if(cts.tv_sec < ts.tv_sec) {
					ip->fail++;
					printf("fail = %llu\n", ip->fail);
					continue;
				}
				if(cts.tv_usec < ts.tv_usec) {
                                        ip->fail++;
					printf("fail = %llu\n", ip->fail);
					continue;
                                }
				ip->fail = 0;
				printf("fail = %llu\n", ip->fail);
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
	/*
	 */
	int err = 0;
	int port = 12345;
	int rate = 10;
	struct jlhead *ips;
	ips = jl_new();
	conf.nr_allow_loss = 3;

	if(jelopt(argv, 'h', "help", 0, &err)) {
	usage:
		printf("netspray [-v] rx|tx IP [IP]*\n"
		       " -v            be verbose\n"
		       " -r --rate     packets per second\n"
		       " -p --port N   [12345] port number to use\n"
			);
		exit(0);
	}

	while(jelopt(argv, 'v', "verbose", 0, &err)) conf.verbose++;
	while(jelopt_int(argv, 'r', "rate", &rate, &err));
	while(jelopt_int(argv, 'p', "port", &port, &err));
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
			fprintf(stderr, "Address not valid\n");
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
			fprintf(stderr, "socket failed\n");
			exit(2);
		}
	}

	if(!strcmp(argv[1], "rx")) {
		struct sockaddr_in my_addr;
		memset( &my_addr, 0, sizeof(my_addr));
		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons(port);
		bind(conf.fd, (struct sockaddr *)&my_addr, sizeof( struct sockaddr_in) );

		if(!conf.verbose) daemonize();
		receiver(ips);
		exit(1);
	}
	if(!strcmp(argv[1], "tx")) {
		if(!conf.verbose) daemonize();
		sender(port, ips, rate);
		exit(1);
	}
	return 2;
}
