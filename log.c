#include <stdarg.h> 
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <zlib.h>
#include <limits.h> /* for PATH_MAX */
#include "log.h"

char log_tags[3][20] = {"Error", "Info", "Debug"};
static int verboseLevel = LOG_ERROR;
static FILE *fp = NULL;

int compressFile(char *inFileName, char *outFileName);
void rotateLog(char *s);



void log_format(const int level, const char* message, va_list args) {   
	time_t now;

	if (level > verboseLevel) return;
	if (fp == NULL) setLogFile(NULL);
	
	time(&now);     
	char *date = ctime(&now);   
	date[strlen(date) - 1] = '\0';  
	fprintf(fp, "%s [%s] ", date, log_tags[level]);  vfprintf(fp, message, args);   fprintf(fp, "\n"); 
	fflush(fp);
}

void setLogFile(char *s) {
	if ((s == NULL) || (*s == '\0')) fp = stdout;
	else {
		rotateLog(s);
		fp = fopen(s, "w");
		if (fp == NULL) {
			printf("Error opening logFile ... exiting\n");
			exit(-1);
		}
	}
}

void setLogLevel(int level) {
		verboseLevel = level;
}

void logError(const char* message, ...) {  
	va_list args;   
	
	va_start(args, message);    
	log_format(LOG_ERROR, message, args);     
	va_end(args); 
}

void logInfo(const char* message, ...) {
	va_list args;   
	
	va_start(args, message);    
	log_format(LOG_INFO, message, args);  
	va_end(args); 
}

void logDebug(const char* message, ...) {
	va_list args;   
	
	va_start(args, message);    
	log_format(LOG_DEBUG, message, args);     
	va_end(args); 
}

int compressFile(char *inFileName, char *outFileName)
{
   char buf[BUFSIZ] = { 0 };
   size_t bytes_read = 0;
   
   
   FILE *in = fopen(inFileName, "r");
   if (!in) return -1;
   
   gzFile out = gzopen(outFileName, "wb");
   if (!out) {
      logError("Unable to open %s for writing\n", outFileName);
      return -1;
   }

   bytes_read = fread(buf, 1, BUFSIZ, in);
   while (bytes_read > 0)
   {
      int bytes_written = gzwrite(out, buf, bytes_read);
      if (bytes_written == 0) {
         int err_no = 0;
         logError("Error during compression: %s", gzerror(out, &err_no));
         gzclose(out);
         return -1;
      }
      bytes_read = fread(buf, 1, BUFSIZ, in);
   }
   gzclose(out);
   fclose(in);

   return 0;
}

void rotateLog(char *s) {
	char 	outFileName1[PATH_MAX] = { 0 };
	char 	outFileName2[PATH_MAX] = { 0 };
	int 	i;
	
	for (i = LOG_NB_FILES -1; i > 1; i--) {
		snprintf(outFileName1, PATH_MAX, "%s.%i.gz", s, i);
		snprintf(outFileName2, PATH_MAX, "%s.%i.gz", s, i+1);
		rename(outFileName1, outFileName2);
	}
	//Compress the ".1" which has not yet been compressed
	snprintf(outFileName1, PATH_MAX, "%s.1", s);
	snprintf(outFileName2, PATH_MAX, "%s.2.gz", s);
	compressFile(outFileName1, outFileName2);

	snprintf(outFileName1, PATH_MAX, "%s.1", s);
	rename(s, outFileName1);
}
