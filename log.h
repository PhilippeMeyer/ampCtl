#ifndef LOG_H 
#define LOG_H

#define LOG_ERROR		0
#define LOG_INFO		1
#define LOG_DEBUG		2
#define LOG_NB_FILES	5

#define GPIO_READ	0		/* Open GPIO in read mode */
#define GPIO_WRITE	1		/* Open GPIO in write mode */

void logError(const char* message, ...); void logInfo(const char* message, ...); void logDebug(const char* message, ...);
void setLogLevel(int level);
void setLogFile(char *s);

#endif