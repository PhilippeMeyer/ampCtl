#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

#include "gpio.h"
#include "log.h"

/****************************************************************
 * gpio_export
 ****************************************************************/
int gpio_export(struct gpio *g)
{
	int fd, len;
	char buf[MAX_BUF];
	
	snprintf(buf, sizeof(buf), "%s/export", g->gpioPath);

	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		logError("Error exporting Gpio : %i", g->pin);
		return fd;
	}
 
	len = snprintf(buf, sizeof(buf), "%d", g->pin);
	write(fd, buf, len);
	close(fd);
 
	return 0;
}

/****************************************************************
 * gpio_unexport
 ****************************************************************/
int gpio_unexport(struct gpio *g)
{
	int fd, len;
	char buf[MAX_BUF];
 
	snprintf(buf, sizeof(buf), "%s/unexport", g->gpioPath);
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		logError("Error unexporting Gpio : %i", g->pin);
		return fd;
	}
 
	len = snprintf(buf, sizeof(buf), "%d", g->pin);
	write(fd, buf, len);
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_set_direction
 ****************************************************************/
int gpio_set_direction(struct gpio *g, int dir)
{
	int fd;
	char buf[MAX_BUF];
 
	g->direction = dir;
	snprintf(buf, sizeof(buf), "%s/gpio%d/direction", g->gpioPath, g->pin);
 
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		logError("Error setting direction for Gpio : %i", g->pin);
		return fd;
	}
 
	if (g->direction == GPIO_WRITE)			/* This is an output	*/
		write(fd, "out", 4);
	else
		write(fd, "in", 3);
 
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_set_value
 ****************************************************************/
int gpio_set_value(struct gpio *g, unsigned int value)
{
	int fd;
	char buf[MAX_BUF];
 
	snprintf(buf, sizeof(buf), "%s/gpio%d/value", g->gpioPath, g->pin);
 
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		logError("Error settint value for Gpio : %i", g->pin);
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
int gpio_get_value(struct gpio *g, unsigned int *value)
{
	int fd;
	char buf[MAX_BUF];
	char ch;
	
	snprintf(buf, sizeof(buf), "%s/gpio%d/value", g->gpioPath, g->pin);
 
	fd = open(buf, O_RDONLY);
	if (fd < 0) {
		logError("Error getting value for Gpio : %i", g->pin);
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

int gpio_set_edge(struct gpio *g, char *edge)
{
	int fd;
	char buf[MAX_BUF];

	snprintf(buf, sizeof(buf), "%s/gpio%d/edge", g->gpioPath, g->pin);
 
	fd = open(buf, O_WRONLY);
	if (fd < 0) {
		logError("Error setting edge for Gpio : %i", g->pin);
		return fd;
	}
 
	write(fd, edge, strlen(edge) + 1); 
	close(fd);
	return 0;
}

/****************************************************************
 * gpio_fd_open
 ****************************************************************/

int gpio_fd_open(struct gpio *g)
{
	int fd;
	char buf[MAX_BUF];

	snprintf(buf, sizeof(buf), "%s/gpio%d/value", g->gpioPath, g->pin);
 
	if (g->direction == GPIO_READ) fd = open(buf, O_RDONLY | O_NONBLOCK );
	else fd = open(buf, O_WRONLY);
	if (fd < 0) {
		logError("Error openning Gpio : %i", g->pin);
	}
	return fd;
}

/****************************************************************
 * gpio_fd_close
 ****************************************************************/

int gpio_fd_close(struct gpio *g)
{
	return close(g->fd);
}