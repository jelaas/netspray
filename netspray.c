/*
 * File: netspray.c
 * Implements: network connectivity tester.
 *
 * Copyright: Jens Låås, Uppsala University, 2014
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
#include <syslog.h>
#include <signal.h>

#include "jelist.h"
#include "jelopt.h"

struct ip {
	struct sockaddr_in addr;
	struct timeval ts[100]; /* circular buffer */
	int rate;
	int pos; /* position in ts array */
	int last; /* last position */
	time_t lastreport; /* latest "still failed" sent at this time */
	uint64_t fail; /* nr of failures detected */
	uint64_t count; /* total number of packets processed */
	struct timeval gracets; /* point in time when grace period expired */
	struct timeval recoveryts; /* point in time for recovery event */
	struct timeval gracetime; /* time until grace period has expired*/
	uint8_t data[4];
};

struct {
	int verbose;
	int fd;
	int nr_allow_loss;
	int recoverytime; /* time after reconnect for recovery */
	int foreground;
	int facility;
	char *exec;
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

int logmsg(struct ip *ip, char *msg, struct timeval *ts)
{
	pid_t pid;
	char ats[64];
	struct tm tm;
	
	gmtime_r(&ts->tv_sec, &tm);
	snprintf(ats, sizeof(ats), "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ts->tv_usec/1000);
	
	if(conf.verbose) fprintf(stderr, "%s: %s at %s\n", inet_ntoa(ip->addr.sin_addr), msg, ats);
	
	pid = fork();
	if(pid == 0) {
		close(conf.fd);
		syslog(LOG_ERR, "%s %s at %s", inet_ntoa(ip->addr.sin_addr), msg, ats);
		_exit(0);
	}
	return 0;
}

int event(struct ip *ip, char *msg, struct timeval *ts)
{
	pid_t pid;
	char ats[64];
	struct tm tm;
	
	gmtime_r(&ts->tv_sec, &tm);
	snprintf(ats, sizeof(ats), "%02d:%02d:%02d.%03ld", tm.tm_hour, tm.tm_min, tm.tm_sec, ts->tv_usec/1000);
	
	pid = fork();
        if(pid == 0) {
		char *argv[5];
                close(conf.fd);
		argv[0] = conf.exec;
		argv[1] = inet_ntoa(ip->addr.sin_addr);
		argv[2] = msg;
		argv[4] = ats;
		argv[3] = (void*)0;
		execv(conf.exec, argv);
                _exit(0);
	}
        return 0;
}

int ip_status_fail(struct ip *ip, struct timeval *ts)
{
	if(ip->fail == 0) {
		logmsg(ip, "connectivity failed", ts);
		if(conf.exec) event(ip, "FAIL", ts);
	}
	if(ip->fail &&
	   (ts->tv_sec > ip->lastreport) &&
	   ((ts->tv_sec % (conf.recoverytime?conf.recoverytime:60)) == 0)) {
		ip->lastreport = ts->tv_sec;
		logmsg(ip, "connectivity still failed", ts);
	}
	ip->fail++;
	if(conf.verbose) fprintf(stderr, "%s: fail = %llu\n", inet_ntoa(ip->addr.sin_addr), ip->fail);
	return ip->fail;
}

int ip_status_ok(struct ip *ip, struct timeval *ts)
{
	if(ip->fail) {
		memcpy(&ip->recoveryts, ts, sizeof(struct timeval));
		ip->recoveryts.tv_sec += conf.recoverytime;
		logmsg(ip, "reconnected", ts);
		if(conf.exec) event(ip, "RECONNECT", ts);
	}
	ip->fail = 0;
	if(ip->recoveryts.tv_sec) {
		if(ip->recoveryts.tv_sec < ts->tv_sec) {
			memset(&ip->recoveryts, 0, sizeof(struct timeval));
			if(conf.verbose) fprintf(stderr, "%s: recovered\n", inet_ntoa(ip->addr.sin_addr));
			logmsg(ip, "connectivity recovered", ts);
			if(conf.exec) event(ip, "RECOVERED", ts);
		}
	}
	if(conf.verbose) fprintf(stderr, "%s: fail = %llu recover=%lu\n",
				 inet_ntoa(ip->addr.sin_addr), ip->fail, ip->recoveryts.tv_sec);
	return ip->fail;
}

void receiver(struct jlhead *ips)
{
	struct pollfd fds[2];
	struct sockaddr_in from_addr;
	struct ip *ip;
	unsigned int fromlen;
	struct timeval ts;
	int got, rc;
	int polltimeout = 500;		
	uint8_t buf[4];

	fromlen = sizeof( from_addr );

	fds[0].fd = conf.fd;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	gettimeofday(&ts, NULL);

	while(1) {
		polltimeout = 500;
		/* calculate time left of grace period: set gracetime */
		jl_foreach(ips, ip) {
			uint64_t gracetime;

			/* only consider non-failed ips */
			if(ip->fail) continue;

			/* gracets - now */
			ip->gracetime.tv_sec = ip->gracets.tv_sec;
			ip->gracetime.tv_usec = ip->gracets.tv_usec;
			ip->gracetime.tv_sec -= ts.tv_sec;
			if(ip->gracetime.tv_usec > ts.tv_usec) {
				ip->gracetime.tv_usec -= ts.tv_usec;
			} else {
				ip->gracetime.tv_sec--;
				ip->gracetime.tv_usec += (uint64_t)1000000;
				ip->gracetime.tv_usec -= ts.tv_usec;
			}
			gracetime = ip->gracetime.tv_sec * 1000;
			gracetime += (ip->gracetime.tv_usec / 1000);
			
			/* set the lowest polltimeout */
			if((gracetime >= 0) && (gracetime < polltimeout))
				polltimeout = gracetime;
		}

		rc = poll(fds, 1, polltimeout);
		if( (rc > 0) && fds[0].revents) {
			got = recvfrom( conf.fd, buf, sizeof(buf), 0,
					(struct sockaddr *)&from_addr, &fromlen);
			if(got < 4) continue;
			gettimeofday(&ts, NULL);
			if(conf.verbose) printf("got packet from %s port %d\n",
						inet_ntoa(from_addr.sin_addr),
						from_addr.sin_port);
			jl_foreach(ips, ip) {
				if(ip->addr.sin_addr.s_addr == from_addr.sin_addr.s_addr) {
					ip->count++;
					memcpy(&ip->ts[ip->pos], &ts, sizeof(struct timeval));
					if(conf.verbose) printf("timestamped %lu.%lu\n", ip->ts[ip->pos].tv_sec, ip->ts[ip->pos].tv_usec);
					if(buf[2]||buf[3])
						ip->rate = (buf[2] << 8) + buf[3];
					{
						uint64_t maxtime;
						memcpy(&ip->gracets, &ip->ts[ip->last], sizeof(struct timeval));
						maxtime = ((uint64_t)1000000/ip->rate)*conf.nr_allow_loss;
						ip->gracets.tv_sec += (maxtime/1000000);
						ip->gracets.tv_usec += (maxtime%1000000);
						if(ip->gracets.tv_usec > 1000000) {
							ip->gracets.tv_usec -= 1000000;
							ip->gracets.tv_sec++;
						}
					}
					ip->last = ip->pos;
					ip->pos++;
					if(ip->pos >= 100) ip->pos = 0;
					break;
				}
			}
		} else {
			gettimeofday(&ts, NULL);
		}
		
		jl_foreach(ips, ip) {
			if(ip->count == 0) {
				ip_status_fail(ip, &ts);
				continue;
			}
			
			/* 
			 * have we reach gracets?
			 */
			if(ip->gracets.tv_sec > ts.tv_sec) {
				ip_status_ok(ip, &ts);
				continue;
			}
			if(ip->gracets.tv_sec < ts.tv_sec) {
				ip_status_fail(ip, &ts);
				continue;
				}
			if(ip->gracets.tv_usec < ts.tv_usec) {
				ip_status_fail(ip, &ts);
				continue;
			}
			ip_status_ok(ip, &ts);
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

void opensyslog()
{
	char buf[64];
	snprintf(buf, sizeof(buf)-1, "netspray[%d]", getpid());
	openlog(strdup(buf), 0, conf.facility);
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
	conf.facility = LOG_DAEMON;
	conf.nr_allow_loss = 3;
	conf.recoverytime = 200;
	srcaddr.s_addr = htonl(INADDR_ANY);
	
	if(jelopt(argv, 'h', "help", 0, &err)) {
	usage:
		printf("netspray [-v] rx|tx IP [IP]*\n"
		       " -b --bind ADDR  optional bind address\n"
		       " -v              be verbose (also keeps process in foreground)\n"
		       " -r --rate       packets per second [10]\n"
		       " -p --port N     port number to use [1234]\n"
		       " -l --loss N     packet loss trigger level [3]\n"
		       " -R --rtime N    recoverytime in seconds after reconnect until recovered [200]\n"
		       " -F              stay in foreground (no daemon)\n"
		       " -e --exec PRG   run this program to handle events\n"
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
		       "\n"
		       "Exec program:\n"
		       "The program/script given to the '-e' switch receives event information in argv.\n"
		       " $1 = IP\n"
		       " $2 = FAIL|RECONNECT|RECOVER\n"
		       " $3 = HH:MM:SS.ms\n"
			);
		exit(0);
	}
	
	while(jelopt(argv, 'v', "verbose", 0, &err)) conf.verbose++;
	while(jelopt(argv, 'F', (void*)0, 0, &err)) conf.foreground=1;
	while(jelopt(argv, 'b', "bind", &bindaddr, &err));
	while(jelopt(argv, 'e', "exec", &conf.exec, &err));
	while(jelopt_int(argv, 'r', "rate", &rate, &err));
	while(jelopt_int(argv, 'p', "port", &port, &err));
	while(jelopt_int(argv, 'l', "loss", &conf.nr_allow_loss, &err));
	while(jelopt_int(argv, 'R', "rtime", &conf.recoverytime, &err));
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
		gettimeofday(&ip->gracets, NULL);
		ip->gracets.tv_sec += 5; /* no events for 5 seconds */
		jl_append(ips, ip);
		argc--;
	}
	
	{
		struct sigaction act;
		memset(&act, 0, sizeof(act));
		act.sa_handler = SIG_DFL;
		act.sa_flags = SA_NOCLDSTOP|SA_NOCLDWAIT;
		sigaction(SIGCHLD, &act, NULL);
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
		opensyslog();
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
		opensyslog();
		sender(port, ips, rate);
		exit(1);
	}
	return 2;
}
