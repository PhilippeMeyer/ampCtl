/*
 * $Id: simple-tcp-proxy.c,v 1.11 2006/08/03 20:30:48 wessels Exp $
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <signal.h>
#include <assert.h>
#include <syslog.h>
#include <err.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <sys/types.h>
#include <sys/select.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <arpa/ftp.h>
#include <arpa/inet.h>
#include <arpa/telnet.h>

#include <mpd/client.h>
#include <glib.h>

#include "gpio.h"

#define BUF_SIZE 			4096
#define LOCAL_HOST 			"192.168.0.116"
#define AMP_SWITCH_GPIO 	75
#define AMP_POLL_TIMEOUT 	500000  	/*  0.5 seconds */
#define AMP_DEBOUNCE 		100000  	/*  0.1 seconds */
#define AMP_INIT_TIMEOUT 	1000000  	/*  1 seconds */

#define MAX_BUF 			64

#define MAX_CONNECTIONS 	5

struct amp {
	struct gpio button;
	struct gpio mute;
	struct gpio off;
	int			mpdPort;
	int			mpdProxyPort;
	struct timeval start;
	struct timeval prev;
	struct timeval cur;
};

struct connection {
	int		cltPort;
	int 	srvPort;
	int 	cltSock;
	int 	srvSock;
	char 	cltHostname[64];
	struct 	mpd_connection *mpd;
};
struct connection cnx[MAX_CONNECTIONS];

void stopCmd(char *, struct connection *);
void pauseCmd(char *, struct connection *);
void volCmd(char *, struct connection *);
void toggleCmd(char *, struct connection *);


#define MAX_CMD 4 
struct command {
	char *name;
	void (*function)(char *, struct connection *);
}cmd[MAX_CMD] = {
			{"stop", stopCmd},
			{"pause", pauseCmd},
			{"setvol", volCmd},
			{"toggle", toggleCmd}
		};

		
void cleanup(int sig){
    syslog(LOG_NOTICE, "Cleaning up...");
    exit(0);
}

void sigreap(int sig){
    int status;
    pid_t p;
    signal(SIGCHLD, sigreap);
    while ((p = waitpid(-1, &status, WNOHANG)) > 0);
    /* no debugging in signal handler! */
}

void set_nonblock(int fd){
    int fl;
    int x;
    fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0) {
	syslog(LOG_ERR, "fcntl F_GETFL: FD %d: %s", fd, strerror(errno));
	exit(1);
    }
    x = fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    if (x < 0) {
	syslog(LOG_ERR, "fcntl F_SETFL: FD %d: %s", fd, strerror(errno));
	exit(1);
    }
}

int readConfiguration(char *filename, struct amp *conf)
{
  GKeyFile *keyfile;
  GKeyFileFlags flags;
  GError *error = NULL;
  gsize length;
  
  keyfile = g_key_file_new ();
  flags = G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS;
  
  if (!g_key_file_load_from_file (keyfile, filename, flags, &error))
  {
    g_error (error->message);
    return -1;
  }
  
  conf->button.pin 	 = g_key_file_get_integer(keyfile, "GPIO", "ButtonGPIO", NULL);
  conf->mute.pin 	 = g_key_file_get_integer(keyfile, "GPIO", "MuteGPIO", NULL);
  conf->off.pin 	 = g_key_file_get_integer(keyfile, "GPIO", "OffGPIO", NULL);
  conf->mpdPort 	 = g_key_file_get_integer(keyfile, "MPD", "MpdPort", NULL);
  conf->mpdProxyPort = g_key_file_get_integer(keyfile, "MPD", "MpdProxyPort", NULL);
  
  printf("Conf = button : %d, mute %d, off %d, Mpd port : %d, Mpd Proxy port : %d", conf->button.pin, conf->mute.pin, conf->off.pin, conf->mpdPort, conf->mpdProxyPort);
  return 0;
}

int createServerSock(int port) {

    int addrlen, s, on = 1, x;
    static struct sockaddr_in client_addr;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) err(1, "socket");

    addrlen = sizeof(client_addr);
    memset(&client_addr, '\0', addrlen);
    client_addr.sin_family = AF_INET;
    client_addr.sin_addr.s_addr = inet_addr(LOCAL_HOST);
    //client_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    client_addr.sin_port = htons(port);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, 4);
    x = bind(s, (struct sockaddr *) &client_addr, addrlen);
    if (x < 0) err(1, "bind port :%d", port);

    x = listen(s, MAX_CONNECTIONS);
    if (x < 0) err(1, "listen port: %d", port);

    syslog(LOG_NOTICE, "listening on  port %d", port);

    return s;
}

int openServerConnection(int port)
{
    struct sockaddr_in rem_addr;
    int len, s, x;
    struct hostent *H;
    int on = 1;

    H = gethostbyname(LOCAL_HOST);
    if (!H)	return (-2);

    len = sizeof(rem_addr);

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return s;

    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, 4);

    memset(&rem_addr, '\0', len);
    rem_addr.sin_family = AF_INET;
    memcpy(&rem_addr.sin_addr, H->h_addr, H->h_length);
    rem_addr.sin_port = htons(port);
    x = connect(s, (struct sockaddr *) &rem_addr, len);
    if (x < 0) {
	close(s);
	return x;
    }

    set_nonblock(s);
    return s;
}

int
get_hinfo_from_sockaddr(struct sockaddr_in addr, int len, char *fqdn)
{
    struct hostent *hostinfo;

    hostinfo = gethostbyaddr((char *) &addr.sin_addr.s_addr, len, AF_INET);
    if (!hostinfo) {
	sprintf(fqdn, "%s", inet_ntoa(addr.sin_addr));
	return 0;
    }
    if (hostinfo && fqdn)
	sprintf(fqdn, "%s [%s]", hostinfo->h_name, inet_ntoa(addr.sin_addr));
    return 0;
}


int waitForConnection(int s, struct connection *cnx)
{
    static int newsock, len;
    static struct sockaddr_in peer;

    len = sizeof(struct sockaddr);
    syslog(LOG_INFO, "calling accept FD %d", s);
    newsock = accept(s, (struct sockaddr *) &peer, &len);
    /* dump_sockaddr (peer, len); */
    if (newsock < 0) {
	if (errno != EINTR) {
	    syslog(LOG_NOTICE, "accept FD %d: %s", s, strerror(errno));
	    return -1;
	}
    }
    get_hinfo_from_sockaddr(peer, len, cnx->cltHostname);
    set_nonblock(newsock);
    return (newsock);
}

int mywrite(int fd, char *buf, int *len){
	int x = write(fd, buf, *len);
	if (x <= 0)	return x;
	if (x != *len) memmove(buf, buf+x, (*len)-x);
	*len -= x;
	return x;
}

void closeCnx(struct connection *cnx, int severity, char *s, int info, char *errstr) {

	close(cnx->cltSock);
	close(cnx->srvSock);
	cnx->cltSock = -1;
	cnx->srvSock = -1;
	if ((info != -1) && (errstr != NULL)) syslog(severity, s, info, errstr);
	else if ((info != -1) && (errstr == NULL)) syslog(severity, s, info);
	else if ((info = -1) && (errstr == NULL)) syslog(severity, s);
	else if ((info = -1) && (errstr != NULL)) syslog(severity, errstr);
}

void stopCmd(char *buff, struct connection *cnx){
	printf("stopCmd : %s\n", buff);
	
	struct mpd_status *status;
	status = mpd_run_status(cnx->mpd);
	if (status == NULL) {
		printf("%s\n", mpd_connection_get_error_message(cnx->mpd));
		return;	
	}

	switch(mpd_status_get_state(status)) {
		case MPD_STATE_PLAY:
			printf("Playing\n");
			break;
                case MPD_STATE_PAUSE:
                        printf("Paused\n");
                        break;
                case MPD_STATE_STOP:
                        printf("Stopped\n");
                        break;
                default:
                        break;
	}
}

void pauseCmd(char *buff, struct connection *cnx){
	printf("pauseCmd : %s\n", buff);
}

void volCmd(char *buff, struct connection *cnx){
	printf("volCmd : %s\n", buff);
}

void toggleCmd(char *buff, struct connection *cnx){
	printf("toggleCmd : %s\n", buff);

	struct mpd_status *status;
	status = mpd_recv_status(cnx->mpd); 
	if (status == NULL) return;	

	switch(mpd_status_get_state(status)) {
		case MPD_STATE_PLAY:
			printf("Playing\n");
			break;
                case MPD_STATE_PAUSE:
                        printf("Paused\n");
                        break;
                case MPD_STATE_STOP:
                        printf("Stopped\n");
                        break;
                default:
                        break;
	}
}

void lookForCommand(char *buff, struct connection *cnx){
	int i;

	for (i = 0 ; i < MAX_CMD ; i++){
		if (strstr(buff, cmd[i].name) != 0) cmd[i].function(buff, cnx);
	}
	printf("%s\n", buff);
}

static void *serviceClient(void *cnxData)
{
    int 	maxfd;
    char 	*sbuf;
    char 	*cbuf;
    int 	x, n;
    int 	cbo = 0;
    int 	sbo = 0;
    int 	cfd, sfd;
    fd_set 	R;
    struct 	connection *cnx;

    cnx = (struct connection *) cnxData;
    syslog(LOG_NOTICE, "connection from %s fd=%d", cnx->cltHostname, cnx->cltSock);
    syslog(LOG_INFO, "connected to %d fd=%d", cnx->srvPort, cnx->srvSock);

    cfd = cnx->cltSock;
    sfd = cnx->srvSock;
    sbuf = malloc(BUF_SIZE);
    cbuf = malloc(BUF_SIZE);
    maxfd = cfd > sfd ? cfd : sfd;
    maxfd++;

    cnx->mpd = mpd_connection_new(LOCAL_HOST, cnx->srvPort, 30000);
    if (mpd_connection_get_error(cnx->mpd) != MPD_ERROR_SUCCESS)
    	printf("%s\n", "Mpd cnx error");

    while (1) {
	struct timeval to;
	if (cbo) {
		if ((mywrite(sfd, cbuf, &cbo) < 0) && errno != EWOULDBLOCK) {
			closeCnx(cnx, LOG_ERR, "write %d: %s", sfd, strerror(errno));
			return;
		}
	}
	if (sbo) {
		if ((mywrite(cfd, sbuf, &sbo) < 0) && errno != EWOULDBLOCK) {
			closeCnx(cnx, LOG_ERR, "write %d: %s", cfd, strerror(errno));
			return;
		}
	}

	FD_ZERO(&R);
	if (cbo < BUF_SIZE) FD_SET(cfd, &R);
	if (sbo < BUF_SIZE) FD_SET(sfd, &R);
	to.tv_sec = 0;
	to.tv_usec = 1000;
	x = select(maxfd+1, &R, 0, 0, &to);

	if (x > 0) {
	    if (FD_ISSET(cfd, &R)) {
		n = read(cfd, cbuf+cbo, BUF_SIZE-cbo);
		if (n <= 0) {
			closeCnx(cnx, LOG_INFO, "Exiting from client side", -1, NULL);
			return;
		}		

		cbo += n;
		lookForCommand(cbuf, cnx);
	    }
	    if (FD_ISSET(sfd, &R)) {
		n = read(sfd, sbuf+sbo, BUF_SIZE-sbo);
		if (n <= 0) {
			closeCnx(cnx, LOG_INFO, "Exiting from server side", -1, NULL);
			return;
		}		

		sbo += n;
	    }
	} else if (x < 0 && errno != EINTR) {
		closeCnx(cnx, LOG_ERR, "Exiting %s", -1, strerror(errno));
		return;
	}
    }
}

void initCnxTbl(struct connection *cnx){
	int i;

	for (i = 0; i < MAX_CONNECTIONS ; i++) {
		cnx[i].cltSock = -1;
		cnx[i].srvSock = -1;
	}
}


int findFreeCnx(struct connection *cnx){
	int i;

	for (i = 0; i < MAX_CONNECTIONS ; i++)
		if (cnx[i].cltSock == -1) return(i);

	return -1;
}


void main(int argc, char *argv[]) {

    int 	masterSock 	= -1;
    int		curCnx	= 0;
    pthread_t	threadId;
	struct amp ampCtl;	

    char hn[128];
    struct hostent *he;

    gethostname(hn, sizeof hn);
    printf("%s\n", hn);
    he = gethostbyname(hn);
    printf("IP address: %s\n", inet_ntoa(*(struct in_addr*)he->h_addr));

    if (2 != argc) {
		fprintf(stderr, "usage: %s config_file\n", argv[0]);
		exit(1);
    }

    initCnxTbl(cnx);
	readConfiguration(argv[1], &ampCtl);

    assert(ampCtl.mpdPort > 0);
    assert(ampCtl.mpdProxyPort > 0);

    openlog(argv[0], LOG_PID, LOG_LOCAL4);

    signal(SIGINT, cleanup);
    signal(SIGCHLD, sigreap);

    masterSock = createServerSock(ampCtl.mpdProxyPort);
    while (1) {
		if (curCnx = findFreeCnx(cnx) == -1) {
			sleep(5);
			continue;
		}
		
		cnx[curCnx].cltPort = ampCtl.mpdProxyPort;	//Port used by the clients
		cnx[curCnx].srvPort = ampCtl.mpdPort;		//Port used to connect to MPD server

		if ((cnx[curCnx].cltSock = waitForConnection(masterSock, &cnx[curCnx])) < 0) continue;

		if ((cnx[curCnx].srvSock = openServerConnection(cnx[curCnx].srvPort)) < 0) {
			close(cnx[curCnx].srvSock);
			continue;
		}
		pthread_create(&threadId, NULL, serviceClient, &cnx[curCnx]);
    }

}

