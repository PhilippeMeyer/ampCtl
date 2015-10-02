#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>

 /****************************************************************
 * Constants
 ****************************************************************/
 
#define AMP_SYSFS_GPIO 75
#define AMP_SYSFS_GPIO_DIR "/sys/class/gpio"
#define AMP_POLL_TIMEOUT 500000  /*  0.5 seconds */
#define AMP_DEBOUNCE 100000  /*  0.1 seconds */
#define AMP_INIT_TIMEOUT 1000000  /*  1 seconds */

#define MAX_BUF 64

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

void ampState(int s) {
	printf("Amp changing to : %d\n", s);
}

void ampMute (int s) {
	printf("Mute changing to : %d\n", s);
}

/****************************************************************
 * Main
 ****************************************************************/
int main(int argc, char **argv, char **envp)
{
	struct pollfd fdset[2];
	int nfds = 1;
	int gpio_fd, timeout, rc;
	char buf = 0;
	unsigned int gpio;
	int len;
	int value;
	int init = 1;

	struct timeval prev, cur, start;

	int stateAmp = 0;
	int stateMute = 1;


	if (argc < 2) {
		printf("Usage: ampCtl <gpio-pin>\n\n");
		printf("Manages a switch to control Hypex Amp\n");
		exit(-1);
	}

	gpio = atoi(argv[1]);

	gpio_export(gpio);
	gpio_set_dir(gpio, 0);
	gpio_set_edge(gpio, "both");
	gpio_fd = gpio_fd_open(gpio);

	timeout = AMP_POLL_TIMEOUT;
	recordEventTime(&prev, &cur);
	recordEventTime(&start, &cur);
 
	memset((void*)fdset, 0, sizeof(fdset));
	fdset[0].fd = gpio_fd;
	fdset[0].events = POLLPRI;

	while (1) {
		rc = poll(fdset, nfds, -1);      

		if (rc < 0) {
			printf("\npoll() failed!\n");
			return -1;
		}
		
		if (!fdset[0].revents & POLLPRI) continue;
		
		lseek(gpio_fd,  0, SEEK_SET);
		len = read(gpio_fd, &buf, 1);
		
		recordEventTime(&prev, &cur);
		if (init) {
			if (delay(&start, &cur) < AMP_INIT_TIMEOUT) continue;
			init = 0;
		}
		if (delay(&prev, &cur) < AMP_DEBOUNCE) continue;

                if (buf == '1') {  /* The button has been pressed */
                        if (stateAmp == 0) { /* Amp is currently off -> switch on */
                                stateAmp = 1;
                                stateMute = 0;
				ampState(stateAmp);
				ampMute(stateMute);
                        }
                        else { /* Amp is currently on :  mute  - unmute and or switch off */
				stateMute = !stateMute;	
				ampMute(stateMute);
                        }
                }
                else { /* The button has been released */
			if (stateMute == 0) continue; /* switch off has not been already triggered */

                        if (stateAmp == 1) { /* Amp is on switch off or mute */
				if (delay(&prev, &cur) < AMP_POLL_TIMEOUT) continue;
                                else {
                                        stateAmp = 0;
					ampState(stateAmp);
                                }
                        }
                }
	}

	gpio_fd_close(gpio_fd);
	return 0;
}
