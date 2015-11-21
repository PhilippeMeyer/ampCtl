#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
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
#include <mpd/player.h>

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
#define AMP_DOUBLE_CLICK_DELAY		300000 		/*  0.3 seconds 	*/
#define AMP_READ_GPIO 				3			/* 3 GPIOs are read : switch encoderA and encoderB */
#define AMP_DEF_CONFIG_FILE			"ampCtl.conf"
#define AMP_MPD_CMD					"service mpd restart"
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
#define AMP_SWITCH_LONG_PRESSED		1024
#define AMP_DOUBLE_CLICK			2048
#define AMP_MPD_NB_CNX_ATTEMPT		5
#define	AMP_MPD_CNX_TIMEOUT			2			/* 2 seconds 		*/

#define AMP_UNMUTE					0
#define AMP_MUTE					1
#define AMP_ON						1
#define AMP_OFF						0

#define MAX_BUF 64

 
struct amp {									//Structure containing the full amplifier status
	struct gpio 			button;				//Hw On-Off switch
	struct gpio 			mute;				//Mute relay
	struct gpio 			off;				//On-Off relay
	struct gpio				encoderA;			//Encoder 1st input
	struct gpio				encoderB;			//Encoder 2nd input
	struct timeval 			pprev;				//Time of the previous previous event
	struct timeval 			prev;				//Time of the previous event
	struct timeval 			cur;				//Time of the current event
	bool					init;				//Is init completed ?
	bool					pressed;			//Is the On-Off switch pressed a long time ?
	int 					stateAmp;			//Amplifier on or off
	int						stateMute;			//Amplifier muted or not
	struct mpd_connection 	*connMpd;			//Connection to mpd
	int						prevEncoded;		//Previous value from the rotary encoder
	char					gpioPath[MAX_BUF];	//Path of the gpios in the user space
	char					logFile[MAX_BUF];	//Log file name
	char					mpdCmd[MAX_BUF];	//Shell command to restart mpd
	int						pauseTimeout;		//Duration of the pause timeout
	int						driverProtect;		//Duration of the delay before unmuting the amplifier when switching on
	int						event;				//Event to manage
	bool					muteOngoing;		//Is a mute on going ?
	pthread_t				pauseThread;		//Pause thread ID
	pthread_t				longPressedThread;	//LongPressed thread ID
	pthread_t				doubleClickThread;	//LongPressed thread ID
};

static void *pauseTimeout (void *arg);
void 		ampState(struct amp *ampCtl, int state);
void 		ampMute (struct amp *ampCtl, int state);
void 		handleMPDerror(struct mpd_connection *c);
void 		processEvent(struct amp *ampCtl, int evt, int inc);
void 		help();
void 		closeGpios(struct amp *ampCtl);
void 		initTime(struct amp *p);
void 		storeEventTime(struct amp *a, struct timeval *t);
int 		delay(struct timeval *p, struct timeval *n);
static void *pauseTimeout (void *arg);
void 		setupPauseTimeout(struct amp *ampCtl);
int 		restoreMPDcnx(struct mpd_connection *c);
void 		gpioInit(struct amp *ampCtl, struct gpio *g, int direction, char *edge);
void 		readEncoderCallback(void *userData);
static void interruptHandler (void *arg);
static void *mpdHandler (void *arg);
void 		readButtonCallback(void *userData);
//int 		execCmdMpd(bool (* mpdFunction)(), struct mpd_connection *connMpd, int nbArg, int inc);



static pthread_mutex_t 	mutexProcess = PTHREAD_MUTEX_INITIALIZER;
static int 				MPDcountError = 0;

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
		CFG_SIMPLE_STR("mpdCmd", 		ampCtl.mpdCmd),
        CFG_END()
    };
    cfg_t *cfg;

	//Default values are copied before reading the config file
	strcpy(ampCtl.gpioPath, AMP_SYSFS_GPIO_DIR);
	strcpy(ampCtl.mpdCmd, AMP_MPD_CMD);
	ampCtl.pauseTimeout = AMP_PAUSE_TIMEOUT_DELAY;	
	ampCtl.driverProtect = AMP_DRIVER_PROTECT_DELAY;

	// Command line options decoding
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
	
	//Parsing the config file either the one provided with the c switch or the default one
    cfg = cfg_init(opts, 0);
    if (configFile) status = cfg_parse(cfg, configFile);
	else status = cfg_parse(cfg, AMP_DEF_CONFIG_FILE);
	if (status == CFG_FILE_ERROR) logError("Non existing config file");
	if (status == CFG_PARSE_ERROR) logError("Config file parse error");
	if (status != CFG_SUCCESS) exit(-1);
	if (logFile != NULL) setLogFile(logFile);
	else setLogFile(ampCtl.logFile);

	//The configuration is now loaded
	logInfo("Starting %s with button on : %i, encoder on : %i %i, switch on %i, mute on : %i", argv[0], ampCtl.button.pin, ampCtl.encoderA.pin, ampCtl.encoderB.pin, ampCtl.off.pin, ampCtl.mute.pin);
	logInfo("Gpio path : %s", ampCtl.gpioPath);

	//Infinite loop where a child process in charge of the work is spawned
	//If for any reason, the child process crashes, the parent process will respawn another child
	while (true) {
		if ((pidChild = fork()) > 0) { 		//Parent process branch
			logInfo("Parent process waiting...");
			if (waitpid(pidChild, &status, 0) == -1){
				logError("Error in parent process waiting for child");
				exit(-1);
			}
			//The child process has been terminating : respawn
			logInfo("Child terminating...");
			
			continue;					// child aborted -> refork
		}
		else if(pidChild == -1){
			logError("Error in forking the child process");
			exit(-1);
		}

		// Child section : initialisation
		logInfo("Child process initializing...");
		
		gpioInit(&ampCtl, &ampCtl.off, GPIO_WRITE, NULL);		//On-off relay
		gpioInit(&ampCtl, &ampCtl.mute, GPIO_WRITE, NULL);		//Mute relay
		gpioInit(&ampCtl, &ampCtl.button, GPIO_READ, "both");	//Switch on-off button. push and relase are detected
		ampCtl.button.callback = &readButtonCallback;			//Callback to be activated when the switch is used
		gpioInit(&ampCtl, &ampCtl.encoderA, GPIO_READ, "both");	//Rotary encoder. push and relase are detected
		ampCtl.encoderA.callback = &readEncoderCallback;		//Callback to be activaed when the encoder is used
		gpioInit(&ampCtl, &ampCtl.encoderB, GPIO_READ, "both");	//Second rotary encoder entry
		ampCtl.encoderB.callback = &readEncoderCallback;
		
		initTime(&ampCtl);										//Init the variables to store the time
		ampCtl.init = true;
		ampCtl.muteOngoing = false;								//Reflects if the amplifier is on mute 

		ampCtl.connMpd = mpd_connection_new(NULL, 0, 30000);	//Connection to mpd
		if (mpd_connection_get_error(ampCtl.connMpd) != MPD_ERROR_SUCCESS) { 
			handleMPDerror(ampCtl.connMpd);
			closeGpios(&ampCtl);
			return -1;											//If connection fails exit. Parent process will repawn
		}
		logDebug("MPD connection initialized");

		ampCtl.stateMute = -1;									//Init state for stateMute and stateAmp
		ampCtl.stateAmp =  -1;
		ampState(&ampCtl, AMP_OFF);								//Switch off and mute the amplifier
		ampMute(&ampCtl, AMP_MUTE);
		logDebug("Amp initialized");
		
		//The software is listenning to two types of events : 
		//  - gpio events from the switch and the rotary encoder
		//  - mpd events
		//
		//Those two event types are received in two different threads
		//gpios events are received in the main thread
		//mpd events are received in the thread created below
		//mpdHandler is the routine in charge of processing mpd events
		//interruptHandler is in charge of gpios events
		int task = pthread_create (&threadId, NULL, mpdHandler, &ampCtl);
		if(task) logError("Error creating mpdHandler thread. Error : %i", task);
		interruptHandler(&ampCtl); 
		
		//Normally this point should never be reached as interruptHandler is an infinite loop
		//on gpios events
		closeGpios(&ampCtl);
	}
}


//Handler in charge of managing gpios interrupts
//Grab all events on the gpios file descriptors in an infinite loop. Uses poll to wait for interrupts which blocks
//execution until a new event is received.
//
//Is reading events from the swith (1 gpio line) and from the rotary encoder (2 gpios lines)
//When an event is received, the value is read and the registred callback is executed.
//arg is a pointer on the amplifier contro structure
static void interruptHandler (void *arg){

	struct 	amp *ampCtl = (struct amp *)arg;
	struct 	pollfd fdset[AMP_READ_GPIO];
	int    	rc;

	memset(&fdset, 0, sizeof(fdset) * AMP_READ_GPIO);			//Reset the memory and initialize the fdset with
	fdset[0].fd = ampCtl->button.fd;							//the 3 file descriptors for the 3 gpios
	fdset[0].events = POLLPRI;
	fdset[1].fd = ampCtl->encoderA.fd;
	fdset[1].events = POLLPRI;
	fdset[2].fd = ampCtl->encoderB.fd;
	fdset[2].events = POLLPRI;

	while (true) {												//Infinite loop to grab gpios events
		rc = poll(fdset, AMP_READ_GPIO, -1); 					//Wait for the events
		logDebug("Event received on <button>, <encoderA>, <encoderB> : %i %i %i", fdset[0].revents, fdset[1].revents, fdset[2].revents);
		if (rc < 1) {
			logError("Error in polling : %i", rc);
			return;
		}

		if (fdset[0].revents != 0) { 							//Event received on switch : read the value  
			rc = read (ampCtl->button.fd, &ampCtl->button.value, 1) ;
			lseek(ampCtl->button.fd, 0, SEEK_SET);				//reset the fd to the beginning
			ampCtl->button.callback(ampCtl);					//execute the callback
		}
		if ((fdset[1].revents != 0) || (fdset[2].revents != 0)){	//Event received on the rotary encoder (either line A or B)
			rc = read (ampCtl->encoderA.fd, &ampCtl->encoderA.value, 1) ;
			lseek(ampCtl->encoderA.fd, 0, SEEK_SET);
			rc = read (ampCtl->encoderB.fd, &ampCtl->encoderB.value, 1) ;
			lseek(ampCtl->encoderB.fd, 0, SEEK_SET);
			ampCtl->encoderA.callback(ampCtl);
		}
	}
	return;
}

//mpdHanler : handles all the mpd events
//
//This routine is called in a separate thread to catch all mpd events
//When mpd events are received they are dispatched with the routine process event
//arg : pointer on the amplifier control structure
static void *mpdHandler (void *arg){
	struct amp 				*ampCtl = (struct amp *) arg;
	struct mpd_status 		*status;
	struct mpd_connection 	*connMpd;


	connMpd = mpd_connection_new(NULL, 0, 30000);					// Create another MPD connection for this thread in charge of sensing MPD changes 
	if (mpd_connection_get_error(connMpd) != MPD_ERROR_SUCCESS) {
		handleMPDerror(connMpd);
		return NULL;
	}

	while(true) {												//Infinite loop to catch mpd events
		if (mpd_run_idle_mask(connMpd, MPD_IDLE_PLAYER) == 0) handleMPDerror(connMpd);
		if(!mpd_send_status(connMpd)) handleMPDerror(connMpd);
		status = mpd_recv_status(connMpd);
		if (status == NULL) handleMPDerror(connMpd);
		
		logDebug("MPD event received");
		
		if (mpd_status_get_state(status) == MPD_STATE_STOP) {	//Depending on the mpd event nature, event is processed
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

//Helper function used to send mpd commands reestablishing the connection when needed
//mpdFunction is a pointer on the mpd function to execute
//the following args are the mpd function arguments
//int execCmdMpd(int (* mpdFunction)(struct mpd_connection *c), struct mpd_connection *connMpd, int nbArg, ...) { 
int execCmdMpd(bool (* mpdFunction)(), struct amp *ampCtl, int nbArg, int inc) { 
	int s;
	int attempt = 0;
	struct mpd_connection *connMpd = ampCtl->connMpd;
	
	do {
		if(nbArg == 0) {
			if((s = mpdFunction(connMpd))) return s; }
		else if(nbArg == 1)		
			if((s = mpdFunction(connMpd, inc))) return s;
		
		//If we are here we had an mpd error : log and reconnect
		attempt++;

		//First attempt, try to reconnect
		logError("Error connecting to MPD : %s", mpd_connection_get_error_message(connMpd));
		if (restoreMPDcnx(connMpd) == MPD_ERROR_SUCCESS) {
			logDebug("Connection to  MPD reestablished");
			continue;
		}
			
		logError("Error connecting to MPD : %s, going to wait %i seconds", mpd_connection_get_error_message(connMpd), AMP_MPD_CNX_TIMEOUT);
		sleep(AMP_MPD_CNX_TIMEOUT);
		if (restoreMPDcnx(connMpd) == MPD_ERROR_SUCCESS) continue;
		
		if(attempt == 2) {
			//Last chance -> restart mpd
			logError("Error connecting to MPD : going to restart MPD");
			system(ampCtl->mpdCmd);
			if (restoreMPDcnx(connMpd) == MPD_ERROR_SUCCESS) continue;
	
			//mpd has not been restarted properly -> exit
			logError("Error connecting to MPD : restart has not been successfull ... Exiting");
			exit(-1);
		}
	} while(attempt < 3);
	
	return -1; 		//Never reached as there an exit to terminate the loop (attempt #2)
}

//Helper routine for processEvent used to switch off the amplifier
//ampCtl : pointer on the amplifier controling structure
void processEventSwitchOff(struct amp *ampCtl){
		logDebug("Process Event Switch off");
		ampState(ampCtl, AMP_OFF);
		ampCtl->muteOngoing = false;
		if(! execCmdMpd((bool (*)())mpd_run_stop, ampCtl, 0, 0)) 
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
}

//processEvent : process the events received either from gpios or from mpd
//
//This routine is in charge of processing all the possible events coming from the two threads
//To avoid any concurrence between the two threads using this routine, a mutex has been put in place
//The routines called here may call as well processEvent to in turn process the event further
//
//ampCtl : pointer on the amplifier controling structure
//evt : event to process
//inc : optional argument containing a value to apply with the event (like volume increment)
//
void processEvent(struct amp *ampCtl, int evt, int inc) {
	int 		prevEvt;
	
	pthread_mutex_lock(&mutexProcess);					//Holding the mutex
	prevEvt = ampCtl->event;							//Copy the event and store the previous value
	ampCtl->event = evt;
	
	if (evt & AMP_SWITCH_ON) {
		logDebug("Process Event Switch on");
		ampState(ampCtl, AMP_ON);
	}
	if (evt & AMP_SWITCH_OFF) {
		processEventSwitchOff(ampCtl);
	}
	if (evt & AMP_SWITCH_MUTE_ON) {
		logDebug("Process Event Mute on");
		ampMute(ampCtl, AMP_MUTE);
		if(! execCmdMpd((bool (*)())mpd_run_pause, ampCtl, 1, true))
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
		setupPauseTimeout(ampCtl);
	}
	if (evt & AMP_SWITCH_MUTE_OFF) {
		logDebug("Process Event Mute off");
		ampMute(ampCtl, AMP_UNMUTE);
		ampCtl->muteOngoing = false;
		if(! execCmdMpd((bool (*)())mpd_run_pause, ampCtl, 1, false))
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
	}
	if (evt & AMP_SWITCH_VOL) {
		logDebug("Process Event Switch volume");
		if(ampCtl->stateAmp) {				/* Amp is off. No change in volume */
			if(! execCmdMpd((bool (*)())mpd_run_change_volume, ampCtl, 1, inc))
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
		}
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
		if(! execCmdMpd((bool (*)())mpd_run_stop, ampCtl, 0, 0))
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
	}
	if (evt & AMP_DRIVER_PROTECT) {
		logDebug("Process Event Drivers protection");
		ampMute(ampCtl, AMP_UNMUTE);
		if (prevEvt & AMP_SWITCH_ON)
			if(! execCmdMpd((bool (*)())mpd_run_play, ampCtl, 0, 0))
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
	}
	if (evt & AMP_SWITCH_LONG_PRESSED) {
		logDebug("Process Event Long Pressed");
		if (ampCtl->stateAmp) processEventSwitchOff(ampCtl);
	}
	if (evt & AMP_DOUBLE_CLICK) {
		logDebug("Double Click Event");
		if (ampCtl->stateAmp) {
			if(! execCmdMpd((bool (*)())mpd_run_next, ampCtl, 0, 0))
				logError("Error connecting to MPD : %s", mpd_connection_get_error_message(ampCtl->connMpd));
		}
	}
	
	pthread_mutex_unlock(&mutexProcess);
}

//Helper function to close open file descriptors
void closeGpios(struct amp *ampCtl) {
		gpio_fd_close(&ampCtl->button);
		gpio_fd_close(&ampCtl->off);
		gpio_fd_close(&ampCtl->mute);
		gpio_fd_close(&ampCtl->encoderA);
		gpio_fd_close(&ampCtl->encoderB);
}


//storeEventTime transfers the current time into the previous time
//a is a pointer on the amplifier status structure
//t is the new current time 
void storeEventTime(struct amp *a, struct timeval *t) {

	a->pprev.tv_sec = a->prev.tv_sec;
	a->pprev.tv_usec = a->prev.tv_usec;
	a->prev.tv_sec = a->cur.tv_sec;
	a->prev.tv_usec = a->cur.tv_usec;
	a->cur.tv_sec = t->tv_sec;
	a->cur.tv_usec = t->tv_usec;
}

// Init the three time info variables that monitor clicks and double clicks
void initTime(struct amp *a) {
	gettimeofday(&a->cur, NULL);
	a->prev.tv_sec = a->cur.tv_sec;
	a->prev.tv_usec = a->cur.tv_usec;
	a->pprev.tv_sec = a->prev.tv_sec;
	a->pprev.tv_usec = a->prev.tv_usec;
}

//delay return the difference in micro seconds between two times
//p is a pointer on the fisrt time to compare
//n is a pointer on the second time to compare
int delay(struct timeval *p, struct timeval *n) {

	return(((n->tv_sec - p->tv_sec)*1000000) + n->tv_usec - p->tv_usec);
}

//pauseTimeout is the function run by the pause thread
//When the amplifier enters in mute (via mpd pause or the switch button),
//a thread is created to possibly switch off the amplifier after the amp->pauseTimeout
//arg is a pointer on the amplifier status structure
static void *pauseTimeout (void *arg){
	struct amp 	*ampCtl = (struct amp *) arg;

	sleep(ampCtl->pauseTimeout);
	logDebug("Pause timeout");
	if(ampCtl->muteOngoing) processEvent(ampCtl, AMP_PAUSE_TIMEOUT, 0);
	return 0;
}

//Helper procedure to setup a pause timeout thread
//Creates if needed a thread will will trigger an event after the pause Timeout
//amp is a pointer on the amplifier status structure
void setupPauseTimeout(struct amp *ampCtl){
	pthread_t	task;

	if (!ampCtl->muteOngoing) {
		ampCtl->muteOngoing = true;
		pthread_cancel(ampCtl->pauseThread);
		task = pthread_create (&ampCtl->pauseThread, NULL, pauseTimeout, ampCtl);
		if(task) logError("Error creating pause timeout thread. Error : %i", task);
	}	
}

//Routine to be called within a separate thread to delay the unmute when the amplifier is starting
//arg : pointer on the amplifier control structure
static void *unmuteDelay (void *arg){
	struct amp 		*ampCtl = (struct amp *) arg;

	usleep(ampCtl->driverProtect);					//Sleep a little time so that amplifier is stabilized
	logDebug("End of drivers protection delay");
	processEvent(ampCtl, AMP_DRIVER_PROTECT, 0);	//Process the event of continuing the power on sequence
	return 0;
}

//Routine in charge of muting the amp
//arg : pointer on the amplifier control structure
//state : state to apply 
void ampMute (struct amp *ampCtl, int state) {
		
	if(ampCtl->stateMute == state) return;

	logInfo("Mute changing to : %d", state);
	
	ampCtl->stateMute = state;
	gpio_set_value(&ampCtl->mute, !state);			//Transfer to Gpio (the relay is active when low)
}

//Routine in charge of switching on and off the amplifier
//arg : pointer on the amplifier control structure
//state : state to apply
void ampState(struct amp *ampCtl, int state) {
	pthread_t	threadId;
	
	if(ampCtl->stateAmp == state) return;

	logInfo("Amp changing to : %d", state);
	
	ampCtl->stateAmp = state;
	if(state) {										//Amp is switching on : mute first and unmute sometime later to protect drivers
		ampMute(ampCtl, AMP_MUTE);
		gpio_set_value(&ampCtl->off, state);
		int task = pthread_create (&threadId, NULL, unmuteDelay, ampCtl);	// Short delay to protect drivers with a concurrent waiting thread
		if(task) logError("Error creating driver protect thread. Error : %i", task);
	}
	else {											//Amp switching off, simply change the relay state
		gpio_set_value(&ampCtl->off, state);
	}
}

//Helper routine used in to restore an MPD connection
//c : pointer to the MPD connection to restore
int restoreMPDcnx(struct mpd_connection *c) {
	int status, i;
	
	for (i = 0 ; i < AMP_MPD_NB_CNX_ATTEMPT ; i++) {
		mpd_connection_free(c);
		c = mpd_connection_new(NULL, 0, 30000);
		if ((status = mpd_connection_get_error(c)) == MPD_ERROR_SUCCESS) return(status);
	}
	return(status);
}

//Routine to handle MPD errors which is mainly used in the MPD events reception thread
//processEvent uses another routine to perform mpd commands which has its integrated error handling mechanism
//c : pointer to the MPD connection
void handleMPDerror(struct mpd_connection *c)
{	
	MPDcountError++;
	logError("Error connecting to MPD : %s", mpd_connection_get_error_message(c));
	if (restoreMPDcnx(c) == MPD_ERROR_SUCCESS) {				//Attempting to restore the connection
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

//Fonction converting a 0 or 1 character into an int
int cvtToDigit(int c) {
	
	return ( (c == '1') ? 1: 0);
}

//Routine callback used to read the output of the rotary encoder
//userData : pointer to the amplifier control structure
void readEncoderCallback(void *userData) {
	struct amp *ampCtl = (struct amp *)userData;
	int inc = 0;
	
	int MSB = cvtToDigit(ampCtl->encoderA.value);
	int LSB = cvtToDigit(ampCtl->encoderB.value);
	int encoded = (MSB << 1) | LSB;					//Puttint the two bits together

	if (ampCtl->prevEncoded == encoded) return;		//No change
	int sum = (ampCtl->prevEncoded << 2) | encoded;	//Combining the previous output with the new one to sense the move
	
	if(sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) inc = -1;
	if(sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) inc = +1;	
	ampCtl->prevEncoded = encoded;

	logDebug("Callback encoder encode: %i inc : %i", encoded, inc);
	if(inc == 0) return;
	
	processEvent(ampCtl, AMP_SWITCH_VOL, inc);
}

//Routine for sensing longPress on the switch button
//When the user presses the switch button longer than AMP_OFF_CLICK_TIMEOUT, the amplifier switches off
//userData : pointer to the amplifier control structure
static void *longPressSensor (void *arg){
	struct amp 		*ampCtl = (struct amp *) arg;

	usleep(AMP_OFF_CLICK_TIMEOUT);
	logDebug("Long press detected");
	if (ampCtl->pressed) {			
		ampCtl->pressed = false;
		processEvent(ampCtl, AMP_SWITCH_LONG_PRESSED, 0);
	}
	else
		ampCtl->pressed = false;
	
	return 0;
}

//Routine for sensing double click on the switch button
//When the user presses the switch button this thread is triggered
//If no double click occurs then it was a simple click
//userData : pointer to the amplifier control structure
static void *doubleClickSensor (void *arg){
	struct amp 		*ampCtl = (struct amp *) arg;

	usleep(AMP_DOUBLE_CLICK_DELAY);		//Waiting for a double click before taking action
	logDebug("No double click detected");
	
	//Mute or unmute the amplifier
	if(ampCtl->stateMute == AMP_MUTE) processEvent(ampCtl, AMP_SWITCH_MUTE_OFF, 0);
	else processEvent(ampCtl, AMP_SWITCH_MUTE_ON, 0);

	return 0;
}

//Callback routine for the switch events.
//This routine debounces the switch and generates the appropriate events
//userData : pointer to the amplifier control structure
void readButtonCallback(void *userData) {
		struct amp 		*ampCtl = (struct amp *)userData;
		struct timeval 	current;
		int task;
	
	gettimeofday(&current, NULL);

	if (ampCtl->init) {												// Still in the init phase ?
		ampCtl->pressed = false;
		ampCtl->init = false;
		return;
	}
	
	logDebug("Switch button pressed. Value %i Pressed : %i", ampCtl->button.value, ampCtl->pressed);

	if (delay(&ampCtl->cur, &current) < AMP_DEBOUNCE) return;		// Debouncing the switch 
	
	storeEventTime(ampCtl, &current);								// The event time is stored to be compared with the next event

	if (ampCtl->button.value == '0') {								// The button has been pressed  0 to the ground
		if (!ampCtl->pressed) {										// Normally when here the pressed flag should be false as it is reset 
			ampCtl->pressed = true;									// when the switch is released. Set pressed to true as long as the button is pressed
			task = pthread_create(&ampCtl->longPressedThread, NULL, longPressSensor, ampCtl);	// Separate thread to sense the long pressed
			logDebug("longPressed thread detection created");
			if(task) logError("Error creating longPress sensor thread. Error : %i", task);
		}
		
		if (ampCtl->stateAmp == 0) { 								// Amp is currently off -> switch on */
			processEvent(ampCtl, AMP_SWITCH_ON, 0);
		}
		else {						// Amp is currently on :  mute  - unmute and or switch off or next song if double click
			if(delay(&ampCtl->pprev, &current) < AMP_DOUBLE_CLICK_DELAY) { 	//It is a double click
				pthread_cancel(ampCtl->doubleClickThread);
				processEvent(ampCtl, AMP_DOUBLE_CLICK, 0); 
			}
			else {					//Trigger a thread to wait for a double click
				task = pthread_create(&ampCtl->doubleClickThread, NULL, doubleClickSensor, ampCtl);	// Separate thread to wait for a double click
				logDebug("doubleClick thread detection created");
				if(task) logError("Error creating double click sensor thread. Error : %i", task);
			}
		}
	}
	else {							// The switch is released -> pressed is false and the sensing thread is cancelled
		logDebug("Switch button released");
		ampCtl->pressed = false; 					/* The button has been released */
		pthread_cancel(ampCtl->longPressedThread);
	}
}

//Helper routine to init a gpio port
//amp : pointer on the amplifier control structure
//g   : pointer on a gpio port structure
//direction : either GPIO_READ or GPIO_WRITE. Reads means that information will be read from the port
//edge : string specifying which event should be detected : "raising", "falling" or "both".
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