#define GPIO_IN 			1
#define GPIO_OUT 			2
#define GPIO_EDGE_BOTH 		"both"
#define GPIO_EDGE_RISING 	"rising"
#define GPIO_EDGE_FALLING	"falling"
#define GPIO_DIR 			"/sys/class/gpio"

struct gpio {
	int pin;
	int fd;
	void *userData;
	void (* callback)(void *usrData);
};
 
