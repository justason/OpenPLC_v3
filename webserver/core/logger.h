#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <libgen.h>
#include <stdarg.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>

#include "ladder.h"

#define LOG_MSG_LENGTH    256

typedef enum _log_media
{
	LOG_MEDIA_NONE = -1,
	LOG_MEDIA_CONSOLE = 0,
	LOG_MEDIA_FILE = 1,
	LOG_MEDIA_SYSLOG = 2,
	LOG_MEDIA_UDP = 3,
} LogMedia;

class Logger
{
private:
	time_t m_starttime;
	pthread_mutex_t m_mutex;
	LogMedia m_media_type;
	char *m_msg_buffer1, *m_msg_buffer2, *m_console_log_buffer, *m_log_name;
	struct sockaddr_in m_server_addr;
	int m_socket;
	char *m_udp_addr;


	/* ********************************************************** */
	int setup_log_console()
	{
		m_media_type = LOG_MEDIA_CONSOLE;

		return 0;
	}

	/* ********************************************************** */
	int setup_log_syslog()
	{
		openlog("", LOG_PERROR, LOG_USER);
		m_media_type = LOG_MEDIA_SYSLOG;

		return 0;
	}

	/* ********************************************************** */
	int setup_log_file(const char * log_name)
	{
		int retval;
		FILE *logfp;


		logfp = fopen(log_name, "w");
		if (logfp != NULL)
		{
			m_log_name = (char *)malloc(strlen(log_name)*sizeof(char));
			if (m_log_name != NULL)
			{
				m_media_type = LOG_MEDIA_FILE;
				strcpy(m_log_name, log_name);
				retval = 0;
			}
			else
			{
				retval = -2;
			}
			fclose(logfp);
		}
		else
		{
			retval = -1;
		}

		return retval;
	}	

	int print_log_file(const char * buffer1, const char * buffer2)
	{
		FILE *logfp;
		int retval = 0;

		if (m_log_name != NULL)
		{
			logfp = fopen(m_log_name, "a");
			if (logfp != NULL)
			{
				int rv = 0;

				rv = fprintf(logfp, "%s%s", buffer1, buffer2);
				if (rv >= 0)
				{
					retval = -3;
				}

				fflush(logfp);
				fclose(logfp);
			}
			else
			{
				retval = -2;
			}
		}
		else
		{
			retval = -1;
		}

		return retval;
	}

	/* ********************************************************** */
	int setup_log_udp(const char * addr_port)
	{
		int retval = 0;
		int port = 8080;


		m_udp_addr = (char *)malloc(strlen(addr_port) * sizeof(char));
		if (m_udp_addr != NULL)
		{
			m_socket = socket(PF_INET, SOCK_DGRAM, 0);
			if (m_socket != -1)
			{
				int rv = sscanf(addr_port, "%99[^:]:%99d", m_udp_addr, &port);
				if (rv == 2)
				{
					/* clear it out */
					memset(&m_server_addr, 0, sizeof(m_server_addr));

					/* it is an INET address */
					//server_addr.sin6_family = AF_INET6;
					m_server_addr.sin_family = AF_INET;

					/* the server IP address, in network byte order */
					rv = inet_pton(AF_INET, m_udp_addr, &(m_server_addr.sin_addr));
					if (rv == 1)
					{
						/* the port we are going to send to, in network byte order */
						m_server_addr.sin_port = htons(port);

						m_media_type = LOG_MEDIA_UDP;
					}
					else // network address convertion failed
					{
						retval = -4;
					}
				}
				else // String parsing failed
				{
					retval = -3;
				}
			}
			else // Socket failed
			{
				retval = -2;
			}
		}
		else // Memory allocation failed
		{
			retval = -1;
		}

		return retval;
	}
	
public:
	Logger(LogMedia media, const char *target)
	{
		m_msg_buffer1 = NULL;
		m_msg_buffer2 = NULL;


		m_mutex = PTHREAD_MUTEX_INITIALIZER;
		pthread_mutex_lock(&m_mutex);
		time(&m_starttime);

		m_media_type = media;

		m_console_log_buffer = (char *)malloc(LOG_MSG_LENGTH * sizeof(char));
		if (m_console_log_buffer == NULL)
		{
			fprintf(stderr, "Memory allocation #1 failed", __func__);
		}
		else
		{
			m_msg_buffer1 = (char *)malloc(LOG_MSG_LENGTH * sizeof(char));
			if (m_msg_buffer1 == NULL)
			{
				fprintf(stderr, "Memory allocation #2 failed", __func__);
			}
			else
			{
				m_msg_buffer2 = (char *)malloc(LOG_MSG_LENGTH * sizeof(char));
				if (m_msg_buffer2 == NULL)
				{
					fprintf(stderr, "Memory allocation #3 failed", __func__);
				}
				else
				{
					int retval = 0;

					switch (media)
					{
					case LOG_MEDIA_FILE:
						retval = setup_log_file(target);
						break;
					case LOG_MEDIA_SYSLOG:
						retval = setup_log_syslog();
						break;
					case LOG_MEDIA_UDP:
						retval = setup_log_udp(target);
						break;
					case LOG_MEDIA_CONSOLE:
						setup_log_console();
						break;
					default:
						break;
					}

					if (retval != 0)
					{
						setup_log_console();
					}
				}
			}
		}

		pthread_mutex_unlock(&m_mutex);
	}

	int print(int syslog_logtype, const char * format, ...)
	{
		time_t uptime, rawtime;
		struct tm * timeinfo;
		va_list args;
		char systime_buffer[80], uptime_buffer[80], tmp_buffer[80];
		const char * error_str;


		pthread_mutex_lock(&m_mutex);

		time(&rawtime);
		uptime = rawtime - m_starttime;
		
		timeinfo = localtime(&rawtime);
		strftime(systime_buffer, 80, "%d.%m.%Y %H:%M:%S", timeinfo);
		timeinfo = localtime(&uptime);
		strftime(tmp_buffer, 80, "%H:%M:%S", timeinfo);
		snprintf(uptime_buffer, 80, "%ldd %s", (uptime / 86400), tmp_buffer);
		
		if (m_msg_buffer1 == NULL || m_msg_buffer2 == NULL)
		{
			return -1;
		}

		switch (syslog_logtype)
		{
		case LOG_DEBUG:
			error_str = "DEBUG";
			break;
		case LOG_INFO:
			error_str = "INFO";
			break;
		case LOG_NOTICE:
			error_str = "NOTICE";
			break;
		case LOG_WARNING:
			error_str = "WARNING";
			break;
		case LOG_ERR:
			error_str = "ERROR";
			break;
		case LOG_CRIT:
			error_str = "CRITICAL";
			break;
		case LOG_ALERT:
			error_str = "ALERT";
			break;
		default:
			error_str = "";
			break;
		}

		snprintf(m_msg_buffer1, LOG_MSG_LENGTH, "%s [%s]: %s : ", systime_buffer, uptime_buffer, error_str);
		va_start (args, format);
		vsnprintf(m_msg_buffer2, LOG_MSG_LENGTH, format, args);

		switch (m_media_type)
		{
		case LOG_MEDIA_FILE:
			print_log_file(m_msg_buffer1, m_msg_buffer2);
			break;
		case LOG_MEDIA_SYSLOG:
			syslog(syslog_logtype, "[%s] %s", uptime_buffer, m_msg_buffer2);
			break;
		case LOG_MEDIA_UDP:
			int ret;

			ret = sendto(m_socket, m_msg_buffer1, strlen(m_msg_buffer1), 0, (struct sockaddr *)&m_server_addr, sizeof(m_server_addr));
			if (ret < 0)
			{
				perror("sendto #1 failed");
			}
			ret = sendto(m_socket, m_msg_buffer2, strlen(m_msg_buffer2), 0, (struct sockaddr *)&m_server_addr, sizeof(m_server_addr));
			if (ret < 0)
			{
				perror("sendto #2 failed");
			}
			break;
		case LOG_MEDIA_CONSOLE:
			snprintf(m_console_log_buffer, LOG_MSG_LENGTH,"%s %s\n", m_msg_buffer1, m_msg_buffer2);
			log(m_console_log_buffer);
			break;
		default:
			fprintf(stderr, "%s %s", m_msg_buffer1, m_msg_buffer2);
			break;
		}

		va_end(args);
		pthread_mutex_unlock(&m_mutex);
	}

	~Logger()
	{
		switch(m_media_type)
		{
		case LOG_MEDIA_FILE:
			if (m_log_name != NULL) free(m_log_name);
			break;
		case LOG_MEDIA_SYSLOG:
			closelog();
			break;
		case LOG_MEDIA_UDP:
			close(m_socket);
			if (m_udp_addr != NULL) free(m_udp_addr);
			break;
		case LOG_MEDIA_CONSOLE:
		default:
			break;
		 }

		if (m_msg_buffer1 != NULL) free(m_msg_buffer1);
		if (m_msg_buffer2 != NULL) free(m_msg_buffer2);
		if (m_console_log_buffer != NULL) free(m_console_log_buffer);
	}
};
