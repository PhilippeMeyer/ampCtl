#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <pthread.h>

 /****************************************************************
 * Constants
 ****************************************************************/
 
#define AMP_SYSFS_GPIO 75
#define AMP_SYSFS_GPIO_DIR "/sys/class/gpio"
#define AMP_POLL_TIMEOUT 500000  /*  0.5 seconds */
#define AMP_DEBOUNCE 100000  /*  0.1 seconds */
#define AMP_INIT_TIMEOUT 1000000  /*  1 seconds */

#define MAX_BUF 64


struct gpio {
	int 	pin;
	int 	fd;
	char 	value;
	void 	*userData;
	void 	(* callback)(void *usrData);
};
 
struct amp {
	struct gpio 	button;
	struct gpio 	mute;
	struct gpio 	off;
	struct timeval 	start;
	struct timeval 	prev;
	struct timeval 	cur;
	boolean		init;
	int 		stateAmp;
	int		stateMute;
};

/****************************************************************
 * gpio_export
 ****************************************************************/
int gpio_export(unsigned int gpio)
{
	int fd, len;
	char buf[MAX_BUF];
 
	fd = open(AMP_SYSFS_GPIO_DIR "/export", O_WRONLY);
	if (fd < 0) {
		perror("gpio/export");
		return fd;
	}
 
	len = snprintf(buf, sizeof(buf), "%d", gpio);
	write(fd, buf, len);
	close(fd);
 
	return 0;
}

/****************************************************************
 * gpio_unexport
 ****************************************************************/
int gpio_unexport(unsigned int gpio)
{
	int fd, len;
	char buf[MAX_BUF];
 
	fd = open(AMP_SYSFS_GPIO_DIR "/unexport", O_WRONLY);
	if (fd < 0) {
		perror("gpio/export");
		return fd;
	}
 
	len = snprintf(buf, sizeof(buf), "%d", gpio);
	write(fd, buf, len);
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_set_dir
 ****************************************************************/
int gpio_set_dir(unsigned int gpio, unsigned int out_flag)
{
	int fd, len;
	char buf[MAX_BUF];
 
	len = snprintf(buf, sizeof(buf), AMP_SYSFS_GPIO_DIR  "/gpio%d/direction", gpio);
 
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		perror("gpio/direction");
		return fd;
	}
 
	if (out_flag)
		write(fd, "out", 4);
	else
		write(fd, "in", 3);
 
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_set_value
 ****************************************************************/
int gpio_set_value(unsigned int gpio, unsigned int value)
{
	int fd, len;
	char buf[MAX_BUF];
 
	len = snprintf(buf, sizeof(buf), AMP_SYSFS_GPIO_DIR "/gpio%d/value", gpio);
 
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		perror("gpio/set-value");
		return fd;
	}
 
	if (value)
		write(fd, "1", 2);
	else
		write(fd, "0", 2);
 
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_get_value
 ****************************************************************/
int gpio_get_value(unsigned int gpio, unsigned int *value)
{
	int fd, len;
	char buf[MAX_BUF];
	char ch;

	len = snprintf(buf, sizeof(buf), AMP_SYSFS_GPIO_DIR "/gpio%d/value", gpio);
 
	fd = open(buf, O_RDONLY);
	if (fd < 0) {
		perror("gpio/get-value");
		return fd;
	}
 
	read(fd, &ch, 1);

	if (ch != '0') {
		*value = 1;
	} else {
		*value = 0;
	}
 
	close(fd);
	return 0;
}


/****************************************************************
 * gpio_set_edge
 ****************************************************************/

int gpio_set_edge(unsigned int gpio, char *edge)
{
	int fd, len;
	char buf[MAX_BUF];

	len = snprintf(buf, sizeof(buf), AMP_SYSFS_GPIO_DIR "/gpio%d/edge", gpio);
 
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		perror("gpio/set-edge");
		return fd;
	}
 
	write(fd, edge, strlen(edge) + 1); 
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_fd_open
 ****************************************************************/

int gpio_fd_open(unsigned int gpio)
{
	int fd, len;
	char buf[MAX_BUF];

	len = snprintf(buf, sizeof(buf), AMP_SYSFS_GPIO_DIR "/gpio%d/value", gpio);
 
	fd = open(buf, O_RDONLY | O_NONBLOCK );
	if (fd < 0) {
		perror("gpio/fd_open");
	}
	return fd;
}

/****************************************************************
 * gpio_fd_close
 ****************************************************************/

int gpio_fd_close(int fd)
{
	return close(fd);
}

void recordEventTime(struct timeval *p, struct timeval *n) {

	p->tv_sec = n->tv_sec;
	p->tv_usec = n->tv_usec;

	gettimeofday(n, NULL);
}

int delay(struct timeval *p, struct timeval *n) {

	int res;

	return(((n->tv_sec - p->tv_sec)*1000000) + n->tv_usec - p->tv_usec);
	return (res);
}

void ampState(struct amp *ampCtl, int state) {
	printf("Amp changing to : %d\n", s);
	
	ampCtl->stateAmp = state;
}

void ampMute (struct amp *ampCtl, int state) {
	printf("Mute changing to : %d\n", s);
	
	ampCtl->stateMute = state;
}


void readButtonCallback(void *userData) {
	
	struct amp *ampCtl = (struct amp *)userData;
	
	printf("Appel de la callback\n");
	
	recordEventTime(&ampCtl->prev, &ampCtl->cur);
	if (ampCtl->init) {						/* Still in the init phase ? */
		if (delay(&ampCtl->start, &ampCtl->cur) < AMP_INIT_TIMEOUT) continue;
		ampCtl->init = false;
	}
	
	if (delay(&ampCtl->prev, &ampCtl->cur) < AMP_DEBOUNCE) continue;	/* Debouncing the switch */

       	if (ampCtl->button.value == '1') {  					/* The button has been pressed */
        	if (ampCtl->stateAmp == 0) { 					/* Amp is currently off -> switch on */
			ampState(ampCtl, 1);
			ampMute(ampCtl, 0);
              	}
              	else { 					/* Amp is currently on :  mute  - unmute and or switch off */
			ampMute(ampCtl, !ampCtl->stateMute);
             	}
       }
     	else { 									/* The button has been released */
		if (ampCtl->stateMute == 0) continue; 				/* switch off has not been already triggered */

              	if (ampCtl->stateAmp == 1) { 					/* Amp is on then switch off if the button has been pressed long enough */
			if (delay(&ampCtl->prev, &ampCtl->cur) < AMP_POLL_TIMEOUT) continue;	/* If the button has been pressed a short time*/
                        else {
                		ampState(ampCtl, 0);
                   	}
              	}
     	}
}

static void *interruptHandler (void *arg){

	struct 	gpio *g = (struct gpio *)arg;
	struct 	pollfd fdset;
	int    	rc;
	char 	c;	

	printf("Creation du thread  gpio : %d\n", g->pin);

	while (1) {
		memset(&fdset, 0, sizeof(fdset));
		fdset.fd = g->fd;
		fdset.events = POLLPRI;

		rc = poll(&fdset, 1, -1);      
		if (rc < 1) exit(-1);

		(void)read (g->fd, &g->value, 1) ;

		g->callback(g->userData);
	}
	return NULL;
}

pthread_t readGpio(struct gpio *g, void (*function)(void *usrData), void *userData) {

	pthread_t threadId ;
        
	gpio_export(g->pin);
        gpio_set_dir(g->pin, 0);
        gpio_set_edge(g->pin, "both");
        g->fd = gpio_fd_open(g->pin);
	g->userData = userData;
	g->callback = function;

	pthread_create (&threadId, NULL, interruptHandler, g);

	return(threadId);
}

/****************************************************************
 * Main
 ****************************************************************/
int main(int argc, char **argv, char **envp)
{
	struct amp ampCtl;	
	int timeout, rc;
	char buf = 0;
	int len;
	int value;
	int init = 1;

	struct timeval prev, cur, start;

	int stateAmp = 0;
	int stateMute = 1;


	if (argc < 4) {
		printf("Usage: ampCtl <gpio-switch> <gpio-amp> <gpio-mute>\n\n");
		printf("Manages switches to control Hypex Amp\n");
		exit(-1);
	}

	ampCtl.off.pin = atoi(argv[2]);
	gpio_export(ampCtl.off.pin);
	gpio_set_dir(ampCtl.off.pin, 1);
	ampCtl.off.fd = gpio_fd_open(ampCtl.off.pin);


	ampCtl.mute.pin = atoi(argv[3]);
	gpio_export(ampCtl.mute.pin);
	gpio_set_dir(ampCtl.mute.pin, 1);
	ampCtl.mute.fd = gpio_fd_open(ampCtl.mute.pin);

	recordEventTime(&ampCtl.prev, &ampCtl.cur);
	recordEventTime(&ampCtl.start, &ampCtl.cur);
	ampCtl.init = true;

	ampCtl.button.pin = atoi(argv[1]);
	pthread_t task = readGpio(&ampCtl.button, readButtonCallback, &ampCtl);
	pthread_join(task, NULL);

	gpio_fd_close(ampCtl.button.pin);
	return 0;
}
