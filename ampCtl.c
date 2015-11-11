#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <getopt.h>
#include <sys/time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <mpd/client.h>
#include <mpd/status.h>
#include <mpd/entity.h>
#include <mpd/search.h>
#include <mpd/tag.h>
#include <mpd/message.h>

#include <confuse.h>


#include "log.h"
#include "gpio.h"

 /****************************************************************
 * Constants
 ****************************************************************/
 
#define AMP_SYSFS_GPIO_DIR 			"/sys/class/gpio"
#define AMP_OFF_CLICK_TIMEOUT 		1000000  	/*  1.0 seconds 	*/
#define AMP_PAUSE_TIMEOUT_DELAY		300  		/*  5 minutes 		*/
#define AMP_DRIVER_PROTECT_DELAY 	1500000		/*  1.5 seconds 	*/
#define AMP_DEBOUNCE 				50000 		/*  0.05 seconds 	*/
#define AMP_READ_GPIO 				3			/* 3 GPIOs are read : switch encoderA and encoderB */
#define AMP_DEF_CONFIG_FILE			"ampCtl.conf"
#define AMP_SWITCH_ON				1
#define AMP_SWITCH_OFF				2
#define AMP_SWITCH_MUTE_ON			4
#define AMP_SWITCH_MUTE_OFF			8
#define AMP_SWITCH_VOL				16
#define AMP_MPD_PLAY				32
#define AMP_MPD_PAUSE				64
#define AMP_MPD_STOP				128
#define AMP_PAUSE_TIMEOUT			256
#define	AMP_DRIVER_PROTECT			512
#define AMP_MPD_NB_CNX_ATTEMPT		5
#define	AMP_MPD_CNX_TIMEOUT			2			/* 2 seconds 		*/

#define AMP_UNMUTE					0
#define AMP_MUTE					1
#define AMP_ON						1
#define AMP_OFF						0

#define MAX_BUF 64

 
struct amp {
	struct gpio 			button;
	struct gpio 			mute;
	struct gpio 			off;
	struct gpio				encoderA;
	struct gpio				encoderB;
	struct timeval 			prev;
	struct timeval 			cur;
	bool					init;
	bool					pressed;
	int 					stateAmp;
	int						stateMute;
	struct mpd_connection 	*connMpd;
	int						prevEncoded;
	char					gpioPath[MAX_BUF];
	char					logFile[MAX_BUF];
	int						pauseTimeout;
	int						driverProtect;
	int						event;
	bool					muteOngoing;
	pthread_t				pauseThread;
	pthread_t				longPressedThread;
};

static void *pauseTimeout (void *arg);
void ampState(struct amp *ampCtl, int state);
void ampMute (struct amp *ampCtl, int state);
void handleMPDerror(struct mpd_connection *c);
void processEvent(struct amp *ampCtl, int evt, int inc);

static pthread_mutex_t 	mutexProcess = PTHREAD_MUTEX_INITIALIZER;
static int 				MPDcountError = 0;



void recordEventTime(struct timeval *p, struct timeval *n) {

	p->tv_sec = n->tv_sec;
	p->tv_usec = n->tv_usec;

	gettimeofday(n, NULL);
}

void storeEventTime(struct amp *a, struct timeval *t) {

	a->prev.tv_sec = a->cur.tv_sec;
	a->prev.tv_usec = a->cur.tv_usec;
	a->cur.tv_sec = t->tv_sec;
	a->cur.tv_usec = t->tv_usec;
}

int delay(struct timeval *p, struct timeval *n) {

	return(((n->tv_sec - p->tv_sec)*1000000) + n->tv_usec - p->tv_usec);
}

static void *pauseTimeout (void *arg){
	struct amp 	*ampCtl = (struct amp *) arg;

	sleep(ampCtl->pauseTimeout);
	logDebug("Pause timeout");
	if(ampCtl->muteOngoing) processEvent(ampCtl, AMP_PAUSE_TIMEOUT, 0);
	return 0;
}

void setupPauseTimeout(struct amp *ampCtl){
	pthread_t	task;

	if (!ampCtl->muteOngoing) {
		ampCtl->muteOngoing = true;
		pthread_cancel(ampCtl->pauseThread);
		task = pthread_create (&ampCtl->pauseThread, NULL, pauseTimeout, ampCtl);
		if(task) logError("Error creating pause timeout thread. Error : %i", task);
	}	
}

void processEvent(struct amp *ampCtl, int evt, int inc) {
	int 		prevEvt;
	
	pthread_mutex_lock(&mutexProcess);
	prevEvt = ampCtl->event;
	ampCtl->event = evt;
	
	if (evt & AMP_SWITCH_ON) {
		logDebug("Process Event Switch on");
		ampState(ampCtl, AMP_ON);
	}
	if (evt & AMP_SWITCH_OFF) {
		logDebug("Process Event Switch off");
		ampState(ampCtl, AMP_OFF);
		ampCtl->muteOngoing = false;
		if(!mpd_run_stop(ampCtl->connMpd)) handleMPDerror(ampCtl->connMpd);
	}
	if (evt & AMP_SWITCH_MUTE_ON) {
		logDebug("Process Event Mute on");
		ampMute(ampCtl, AMP_MUTE);
		if(!mpd_run_pause(ampCtl->connMpd, true)) handleMPDerror(ampCtl->connMpd);
		setupPauseTimeout(ampCtl);
	}
	if (evt & AMP_SWITCH_MUTE_OFF) {
		logDebug("Process Event Mute off");
		ampMute(ampCtl, AMP_UNMUTE);
		ampCtl->muteOngoing = false;
		if(!mpd_run_pause(ampCtl->connMpd, false)) handleMPDerror(ampCtl->connMpd);
	}
	if (evt & AMP_SWITCH_VOL) {
		logDebug("Process Event Switch volume");
		if(!ampCtl->stateAmp) return;				/* Amp is off. No change in volume */
		if(!mpd_run_change_volume(ampCtl->connMpd, inc)) handleMPDerror(ampCtl->connMpd);
	}
	if (evt & AMP_MPD_PLAY) {
		logDebug("Process Event MPD Play");
		if (ampCtl->stateAmp) ampMute(ampCtl, AMP_UNMUTE);	/* Amp is already on just unmute */
		else ampState(ampCtl, AMP_ON);						/* Switch on Amp */
		ampCtl->muteOngoing = false;
	}
	if (evt & AMP_MPD_PAUSE) {
		logDebug("Process Event MPD Pause");
		ampMute(ampCtl, AMP_MUTE);
		setupPauseTimeout(ampCtl);
	}
	if (evt & AMP_MPD_STOP) {
		logDebug("Process Event MPD Stop");
		ampCtl->muteOngoing = false;
		ampState(ampCtl, AMP_OFF);
	}
	if (evt & AMP_PAUSE_TIMEOUT) {
		logDebug("Process Event Pause timeout");
		ampCtl->muteOngoing = false;
		ampState(ampCtl, AMP_OFF);
		ampMute(ampCtl, AMP_MUTE);
		if(!mpd_run_stop(ampCtl->connMpd)) handleMPDerror(ampCtl->connMpd);
	}
	if (evt & AMP_DRIVER_PROTECT) {
		logDebug("Process Event Drivers protection");
		ampMute(ampCtl, AMP_UNMUTE);
		if (prevEvt & AMP_SWITCH_ON)
			if(!mpd_run_play(ampCtl->connMpd)) handleMPDerror(ampCtl->connMpd);
	}
	
	pthread_mutex_unlock(&mutexProcess);
}

static void *unmuteDelay (void *arg){
	struct amp 		*ampCtl = (struct amp *) arg;

	usleep(ampCtl->driverProtect);
	logDebug("End of drivers protection delay");
	processEvent(ampCtl, AMP_DRIVER_PROTECT, 0);
	return 0;
}

void ampMute (struct amp *ampCtl, int state) {
		
	if(ampCtl->stateMute == state) return;

	logInfo("Mute changing to : %d", state);
	
	ampCtl->stateMute = state;
	gpio_set_value(&ampCtl->mute, !state);		/*The relay is active when low */
}

void ampState(struct amp *ampCtl, int state) {
	pthread_t	threadId;
	
	if(ampCtl->stateAmp == state) return;

	logInfo("Amp changing to : %d", state);
	
	ampCtl->stateAmp = state;
	if(state) {							/* Amp is switching on : mute first and unmute sometime later to protect drivers */
		ampMute(ampCtl, AMP_MUTE);
		gpio_set_value(&ampCtl->off, state);
		int task = pthread_create (&threadId, NULL, unmuteDelay, ampCtl);	/* Short delay to protect drivers */
		if(task) logError("Error creating driver protect thread. Error : %i", task);
	}
	else {
		gpio_set_value(&ampCtl->off, state);
	}
}

int restoreMPDcnx(struct mpd_connection *c) {
	
	int status, i;
	
	for (i = 0 ; i < AMP_MPD_NB_CNX_ATTEMPT ; i++) {
		mpd_connection_free(c);
		c = mpd_connection_new(NULL, 0, 30000);
		if ((status = mpd_connection_get_error(c)) == MPD_ERROR_SUCCESS) return(status);
	}
	return(status);
}

void handleMPDerror(struct mpd_connection *c)
{	
	MPDcountError++;
	logError("Error connecting to MPD : %s", mpd_connection_get_error_message(c));
	if (restoreMPDcnx(c) == MPD_ERROR_SUCCESS) {
		logDebug("Connection to  MPD reestablished");
		return;
	}
	
	logError("Error connecting to MPD : %s, going to wait %i seconds", mpd_connection_get_error_message(c), AMP_MPD_CNX_TIMEOUT);
	sleep(AMP_MPD_CNX_TIMEOUT);
	if (restoreMPDcnx(c) == MPD_ERROR_SUCCESS) return;
	
	/* Connection to MPD impossible -> restart MPD*/
	logError("Error connecting to MPD : going to restart MPD");
	system("service mpd restart");
	if (restoreMPDcnx(c) == MPD_ERROR_SUCCESS) return;
	
	logError("Error connecting to MPD : restart has not been successfull ... Exiting");
	exit(-1);
}

int cvtToDigit(int c) {
	
	return ( (c == '1') ? 1: 0);
}

void readEncoderCallback(void *userData) {
	struct amp *ampCtl = (struct amp *)userData;
	int inc = 0;
	
	int MSB = cvtToDigit(ampCtl->encoderA.value);
	int LSB = cvtToDigit(ampCtl->encoderB.value);
	int encoded = (MSB << 1) | LSB;

	if (ampCtl->prevEncoded == encoded) return;
	int sum = (ampCtl->prevEncoded << 2) | encoded;
	
	if(sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) inc = -1;
	if(sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) inc = +1;	
	ampCtl->prevEncoded = encoded;

	logDebug("Callback encoder encode: %i inc : %i", encoded, inc);
	if(inc == 0) return;
	
	processEvent(ampCtl, AMP_SWITCH_VOL, inc);
}

static void *longPressSensor (void *arg){
	struct amp 		*ampCtl = (struct amp *) arg;

	usleep(AMP_OFF_CLICK_TIMEOUT);
	logDebug("Long press detected");
	if ((ampCtl->pressed) && (ampCtl->stateAmp)) {
		ampCtl->pressed = false;
		processEvent(ampCtl, AMP_SWITCH_OFF, 0);
	}
	else
		ampCtl->pressed = false;
	
	return 0;
}

void readButtonCallback(void *userData) {
		struct amp 		*ampCtl = (struct amp *)userData;
		struct timeval 	current;
	
	gettimeofday(&current, NULL);

		
	if (ampCtl->init) {												/* Still in the init phase ? */
		ampCtl->pressed = false;
		ampCtl->init = false;
		return;
	}
	logDebug("Switch button pressed. Value %i Pressed : %i", ampCtl->button.value, ampCtl->pressed);

	if (delay(&ampCtl->cur, &current) < AMP_DEBOUNCE) return;		/* Debouncing the switch */

	storeEventTime(ampCtl, &current);

	if (ampCtl->button.value == '0') {							/* The button has been pressed  0 to the ground*/
		if (!ampCtl->pressed) {
			ampCtl->pressed = true;
			int task = pthread_create(&ampCtl->longPressedThread, NULL, longPressSensor, ampCtl);	/* Detect long press */
			logDebug("longPressed thread detection created");
			if(task) logError("Error creating longPress sensor thread. Error : %i", task);
		}
		
		if (ampCtl->stateAmp == 0) { 							/* Amp is currently off -> switch on */
			processEvent(ampCtl, AMP_SWITCH_ON, 0);
		}
		else {				/* Amp is currently on :  mute  - unmute and or switch off */
			if (ampCtl->stateMute == AMP_MUTE) processEvent(ampCtl, AMP_SWITCH_MUTE_OFF, 0);
			else processEvent(ampCtl, AMP_SWITCH_MUTE_ON, 0);
		}
	}
	else {
		logDebug("Switch button released");
		ampCtl->pressed = false; 					/* The button has been released */
		pthread_cancel(ampCtl->longPressedThread);
	}
}

static void *mpdHandler (void *arg){
	struct amp 				*ampCtl = (struct amp *) arg;
	struct mpd_status 		*status;
	struct mpd_connection 	*connMpd;


	connMpd = mpd_connection_new(NULL, 0, 30000);					/* Another MPD connection for this thread in charge of sensing MPD changes */
	if (mpd_connection_get_error(connMpd) != MPD_ERROR_SUCCESS) {
		handleMPDerror(connMpd);
		return NULL;
	}

	while(true) {
		if (mpd_run_idle_mask(connMpd, MPD_IDLE_PLAYER) == 0) handleMPDerror(connMpd);
		if(!mpd_send_status(connMpd)) handleMPDerror(connMpd);
		status = mpd_recv_status(connMpd);
		if (status == NULL) handleMPDerror(connMpd);
		
		logDebug("MPD event received");
		if (mpd_status_get_state(status) == MPD_STATE_STOP) {
			logDebug("MPD Stopped, switching off");
			processEvent(ampCtl, AMP_MPD_STOP, 0);
		}
		else if (mpd_status_get_state(status) == MPD_STATE_PLAY) {
			logDebug("MPD Playing, switching on, mute off");
			processEvent(ampCtl, AMP_MPD_PLAY, 0);
		}
		else if (mpd_status_get_state(status) == MPD_STATE_PAUSE) {
			logDebug("MPD Pausing, muting the amplifier");
			processEvent(ampCtl, AMP_MPD_PAUSE, 0);
		}
	}
}

static void interruptHandler (void *arg){

	struct 	amp *ampCtl = (struct amp *)arg;
	struct 	pollfd fdset[AMP_READ_GPIO];
	int    	rc;

	while (true) {
		memset(&fdset, 0, sizeof(fdset) * AMP_READ_GPIO);
		fdset[0].fd = ampCtl->button.fd;
		fdset[0].events = POLLPRI;
		fdset[1].fd = ampCtl->encoderA.fd;
		fdset[1].events = POLLPRI;
		fdset[2].fd = ampCtl->encoderB.fd;
		fdset[2].events = POLLPRI;
		
		rc = poll(fdset, AMP_READ_GPIO, -1); 
		logDebug("Event received on <button>, <encoderA>, <encoderB> : %i %i %i", fdset[0].revents, fdset[1].revents, fdset[2].revents);
		if (rc < 1) {
			logError("Error in polling : %i", rc);
			return;
		}

		if (fdset[0].revents != 0) { /* Event received on switch */
			rc = read (ampCtl->button.fd, &ampCtl->button.value, 1) ;
			lseek(ampCtl->button.fd, 0, SEEK_SET);
			ampCtl->button.callback(ampCtl);
		}
		if ((fdset[1].revents != 0) || (fdset[2].revents != 0)){ /* Event received on encoder */
			rc = read (ampCtl->encoderA.fd, &ampCtl->encoderA.value, 1) ;
			lseek(ampCtl->encoderA.fd, 0, SEEK_SET);
			rc = read (ampCtl->encoderB.fd, &ampCtl->encoderB.value, 1) ;
			lseek(ampCtl->encoderB.fd, 0, SEEK_SET);
			ampCtl->encoderA.callback(ampCtl);
		}
	}
	return;
}

void gpioInit(struct amp *ampCtl, struct gpio *g, int direction, char *edge) {
	
		g->gpioPath = ampCtl->gpioPath;
		gpio_export(g);
		gpio_set_direction(g, direction);
		g->fd = gpio_fd_open(g);
		
		if ((direction == GPIO_READ) && (edge != NULL)) gpio_set_edge(g, edge);
}

void help() {
		printf("\nUsage: ampCtl -v(erbose) -d(debug) -h(help) -c(config): config_file -l(logfile): log_file \n");
		printf("Manages switches and rotary encoder to control Hypex Amp\n\n");
		printf("-v	: Informational messages\n");
		printf("-d	: Debug messages\n");
		printf("-h	: This message\n");
		printf("-l	: specify a log file (default : ampCtl.log)\n");
		printf("-c	: specify a configuration file (default : ampCtl.conf)\n\n");
		printf("The config file may contain the following informations :\n");
		printf("button\t\t: gpio port where the switch is connected\t\t\tRequired\n");
		printf("encoderA\t: gpio port where the first encoder input is connected\t\tRequired\n");
		printf("encoderB\t: gpio port where the second encoder input is connected\t\tRequired\n");
		printf("switch\t\t: gpio port where the switch on relay is connected\t\tRequired\n");
		printf("mute\t\t: gpio port where the mute relay is connected\t\t\tRequired\n");
		printf("pauseTimeout\t: timeout switching off when left in mute mode\t\t\t%i mn\n", AMP_PAUSE_TIMEOUT_DELAY / 60);
		printf("driverProtect\t: timeout unmuting after switch on\t\t\t\t%f s\n", AMP_DRIVER_PROTECT_DELAY / 10000000.0);
		printf("gpioPath\t: path to the gpios in the unix user space\t\t\t/sys/class/gpio\n");
		printf("logFile\t\t: path to the log file\t\t\t\t\t\tampCtl.conf\n\n");
		exit(-1);
}

/****************************************************************
 * Main
 ****************************************************************/
int main(int argc, char **argv, char **envp)
{
	static struct amp 	ampCtl;	
	pthread_t 			threadId ;	   
	pid_t 				pidChild;
    int 				status;
	int 				c;
	int 				option_index = 0;
	char				*configFile = NULL;
	char				*logFile = NULL;

	static struct option long_options[] =
	{
	  {"verbose", no_argument,    	 0, 'v'},	  
	  {"debug",   no_argument,       0, 'd'},	  
	  {"help",    no_argument,       0, 'h'},	  
	  {"config",  required_argument, 0, 'c'},
	  {"logfile", required_argument, 0, 'l'},
	  {0, 0, 0, 0}
	};

   
	cfg_opt_t opts[] = {
        CFG_SIMPLE_INT("button", 		&ampCtl.button.pin),
        CFG_SIMPLE_INT("encoderA", 		&ampCtl.encoderA.pin),
        CFG_SIMPLE_INT("encoderB", 		&ampCtl.encoderB.pin),
        CFG_SIMPLE_INT("switch", 		&ampCtl.off.pin),
        CFG_SIMPLE_INT("mute", 			&ampCtl.mute.pin),
        CFG_SIMPLE_INT("pauseTimeout", 	&ampCtl.pauseTimeout),
        CFG_SIMPLE_INT("driverProtect", &ampCtl.driverProtect),
		CFG_SIMPLE_STR("gpioPath", 		ampCtl.gpioPath),
		CFG_SIMPLE_STR("logFile", 		ampCtl.logFile),
        CFG_END()
    };
    cfg_t *cfg;

	strcpy(ampCtl.gpioPath, AMP_SYSFS_GPIO_DIR);
	ampCtl.pauseTimeout = AMP_PAUSE_TIMEOUT_DELAY;	
	ampCtl.driverProtect = AMP_DRIVER_PROTECT_DELAY;

	while (1)
    {
		c = getopt_long (argc, argv, "c:dhl:v", long_options, &option_index);

		if (c == -1) break;			/* End of the options */

		switch (c)
        {
			case 'h':
				help();
				break;
				
			case 'd':
				setLogLevel(LOG_DEBUG);
				break;
				
			case 'v':
				setLogLevel(LOG_INFO);
				break;
			
			case 'c':
				configFile = optarg;
				break;
				
			case 'l':
				logFile = optarg;
				break;

			case '?':
				help();
				break;

			default:
				help();
        }
    }
	
    cfg = cfg_init(opts, 0);
    if (configFile) status = cfg_parse(cfg, configFile);
	else status = cfg_parse(cfg, AMP_DEF_CONFIG_FILE);
	if (status == CFG_FILE_ERROR) logError("Non existing config file");
	if (status == CFG_PARSE_ERROR) logError("Config file parse error");
	if (status != CFG_SUCCESS) exit(-1);
	if (logFile != NULL) setLogFile(logFile);
	else setLogFile(ampCtl.logFile);

	logInfo("Starting %s with button on : %i, encoder on : %i %i, switch on %i, mute on : %i", argv[0], ampCtl.button.pin, ampCtl.encoderA.pin, ampCtl.encoderB.pin, ampCtl.off.pin, ampCtl.mute.pin);
	logInfo("Gpio path : %s", ampCtl.gpioPath);

	while (true) {
		if ((pidChild = fork()) > 0) { 		//Parent process
			logInfo("Parent process waiting...");
			if (waitpid(pidChild, &status, 0) == -1){
				logError("Error in parent process waiting for child");
				exit(-1);
			}
			
			logInfo("Child terminating...");
			
			continue;					// child aborted -> refork
		}
		else if(pidChild == -1){
			logError("Error in forking the child process");
			exit(-1);
		}

		logInfo("Child process initializing...");
		
		gpioInit(&ampCtl, &ampCtl.off, GPIO_WRITE, NULL);
		gpioInit(&ampCtl, &ampCtl.mute, GPIO_WRITE, NULL);
		gpioInit(&ampCtl, &ampCtl.button, GPIO_READ, "both");
		ampCtl.button.callback = &readButtonCallback;
		gpioInit(&ampCtl, &ampCtl.encoderA, GPIO_READ, "both");
		ampCtl.encoderA.callback = &readEncoderCallback;
		gpioInit(&ampCtl, &ampCtl.encoderB, GPIO_READ, "both");
		ampCtl.encoderB.callback = &readEncoderCallback;
		
		recordEventTime(&ampCtl.prev, &ampCtl.cur);
		recordEventTime(&ampCtl.prev, &ampCtl.cur);
		ampCtl.init = true;
		ampCtl.muteOngoing = false;

		ampCtl.connMpd = mpd_connection_new(NULL, 0, 30000);
		if (mpd_connection_get_error(ampCtl.connMpd) != MPD_ERROR_SUCCESS) { 
			handleMPDerror(ampCtl.connMpd);
			return -1;
		}
		logDebug("MPD connection initialized");

		ampCtl.stateMute = -1;
		ampCtl.stateAmp =  -1;
		ampState(&ampCtl, 0);
		ampMute(&ampCtl, 1);
		logDebug("Amp initialized");
		
		int task = pthread_create (&threadId, NULL, mpdHandler, &ampCtl);
		if(task) logError("Error creating mpdHandler thread. Error : %i", task);
		interruptHandler(&ampCtl); 
		
		gpio_fd_close(&ampCtl.button);
		gpio_fd_close(&ampCtl.off);
		gpio_fd_close(&ampCtl.mute);
		gpio_fd_close(&ampCtl.encoderA);
		gpio_fd_close(&ampCtl.encoderB);
	}
}