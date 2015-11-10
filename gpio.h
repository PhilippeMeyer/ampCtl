#ifndef GPIO_H 
#define GPIO_H
/*
Structure to maintain gpio status
*/

struct gpio {
	int 	pin;								// Gpio port number
	int 	fd;									// file descriptor associated to the port
	int 	direction;							// in or out
	char 	value;								// 1 or 0
	char	*gpioPath;							// Path to gpio in the user space
	void 	*userData;							// user data for callbacks
	void 	(* callback)(void *usrData);		// callback when an event is received
};


int gpio_export(struct gpio *g);
int gpio_unexport(struct gpio *g);
int gpio_set_direction(struct gpio *g, int dir);
int gpio_set_value(struct gpio *g, unsigned int value);
int gpio_get_value(struct gpio *g, unsigned int *value);
int gpio_set_edge(struct gpio *g, char *edge);
int gpio_fd_open(struct gpio *g);
int gpio_fd_close(struct gpio *g);

#define MAX_BUF 64

#endif
