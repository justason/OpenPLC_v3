//-----------------------------------------------------------------------------
// Copyright 2015 Thiago Alves
//
// Based on the LDmicro software by Jonathan Westhues
// This file is part of the OpenPLC Software Stack.
//
// OpenPLC is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// OpenPLC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with OpenPLC.  If not, see <http://www.gnu.org/licenses/>.
//------
//
// This file is the hardware layer for the OpenPLC. If you change the platform
// where it is running, you may only need to change this file. All the I/O
// related stuff is here. Basically it provides functions to read and write
// to the OpenPLC internal buffers in order to update I/O state.
// Thiago Alves, Dec 2015
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/stat.h>

#include "ladder.h"
#include "custom_layer.h"
#include "logger.h"

#if !defined(ARRAY_SIZE)
#define ARRAY_SIZE(x) (sizeof((x)) / sizeof((x)[0]))
#endif

#define MAX_INPUT 		  16
#define MAX_OUTPUT 		  16
#define MAX_PWM_OUT	      16

#define GPIO_IN_OFFSET    480
#define GPIO_OUT_OFFSET   464

#define GPIO_DIR_IN       0
#define GPIO_DIR_OUT_LOW  1
#define GPIO_DIR_OUT_HIGH 2

#define GPIO_VAL_LOW      0
#define GPIO_VAL_HIGH     1

#define LOG_MSG_LENGTH    256
#define SMALL_BUFF_SIZE   256

int gpio_export(int gpio);
int gpio_direction(int gpio, int dir);
void gpio_unexport(int gpio);
int gpio_read(int gpio);
int gpio_write(int gpio, int val);

int pwm_export(int pwm);
void pwm_unexport(int pwm);
int pwm_read(int pwm, unsigned long *freq, unsigned long *duty, int *enable);
int pwm_write(int pwm, unsigned long freq, unsigned long duty, int enable);

Logger *g_logger;

//-----------------------------------------------------------------------------
// This function is called by the main OpenPLC routine when it is initializing.
// Hardware initialization procedures should be here.
//-----------------------------------------------------------------------------
void initializeHardware()
{
	g_logger = new Logger(LOG_MEDIA_UDP, "192.168.0.102:1888");


	g_logger->print(LOG_INFO, "%s: Starting\n", __func__);

	// Configure digital inputs
	for (int i = 0; i < MAX_INPUT; i++)
	{
		if (pinNotPresent(ignored_bool_inputs, ARRAY_SIZE(ignored_bool_inputs), i))
		{
			gpio_export(GPIO_IN_OFFSET + i);
			gpio_direction(GPIO_IN_OFFSET + i, GPIO_DIR_IN);
		}
	}

	// Configure digital outputs
	for (int i = 0; i < MAX_OUTPUT; i++)
	{
		if (pinNotPresent(ignored_bool_outputs, ARRAY_SIZE(ignored_bool_outputs), i))
		{
			gpio_export(GPIO_OUT_OFFSET + i);
			gpio_direction(GPIO_OUT_OFFSET + i, GPIO_DIR_OUT_LOW);
		}
	}
/*
	// Configure PWM outputs
	for (int i = 0; i < MAX_PWM_OUT; i++)
	{
		if (pinNotPresent(ignored_int_outputs, ARRAY_SIZE(ignored_int_outputs), i))
		{
			pwm_export(i);
		}
	}
*/
	g_logger->print(LOG_INFO, "%s: Finished\n", __func__);
}

void finalizeHardware()
{
	g_logger->print(LOG_INFO, "%s: Starting\n", __func__);

	// Configure digital inputs
	for (int i = 0; i < MAX_INPUT; i++)
	{
		if (pinNotPresent(ignored_bool_inputs, ARRAY_SIZE(ignored_bool_inputs), i))
		{
			gpio_unexport(GPIO_IN_OFFSET + i);
		}
	}
	
	// Configure digital outputs
	for (int i = 0; i < MAX_OUTPUT; i++)
	{
		if (pinNotPresent(ignored_bool_outputs, ARRAY_SIZE(ignored_bool_outputs), i))
		{
			gpio_unexport(GPIO_OUT_OFFSET + i);
		}
	}
	g_logger->print(LOG_INFO, "%s: Finished\n", __func__);
	
	delete g_logger;
}

//-----------------------------------------------------------------------------
// This function is called by the OpenPLC in a loop. Here the internal buffers
// must be updated to reflect the actual Input state. The mutex bufferLock
// must be used to protect access to the buffers on a threaded environment.
//-----------------------------------------------------------------------------
void updateBuffersIn()
{
	pthread_mutex_lock(&bufferLock); //lock mutex

	for (int i = 0; i < MAX_INPUT; i++)
	{
		if (pinNotPresent(ignored_bool_inputs, ARRAY_SIZE(ignored_bool_inputs), i))
		{
			if (bool_input[i / 8][i % 8] != NULL)
			{
				*bool_input[i / 8][i % 8] = gpio_read(GPIO_IN_OFFSET + i);
			}
		}
	}

	pthread_mutex_unlock(&bufferLock); //unlock mutex
}

//-----------------------------------------------------------------------------
// This function is called by the OpenPLC in a loop. Here the internal buffers
// must be updated to reflect the actual Output state. The mutex bufferLock
// must be used to protect access to the buffers on a threaded environment.
//-----------------------------------------------------------------------------
void updateBuffersOut()
{
	pthread_mutex_lock(&bufferLock); //lock mutex

	//OUTPUT
	for (int i = 0; i < MAX_OUTPUT; i++)
	{
		if (pinNotPresent(ignored_bool_outputs, ARRAY_SIZE(ignored_bool_outputs), i))
		{
			if (bool_output[i / 8][i % 8] != NULL)
			{
				gpio_write(GPIO_OUT_OFFSET + i, *bool_output[i / 8][i % 8]);
			}
		}
	}
/*
	//ANALOG OUT (PWM)
	for (int i = 0; i < MAX_PWM_OUT; i++)
	{
		if (pinNotPresent(ignored_int_outputs, ARRAY_SIZE(ignored_int_outputs), i))
		{
			if (int_output[i] != NULL)
			{
				pwm_write(i, (*int_output[i] / 64));
			}
		}
	}
*/
	pthread_mutex_unlock(&bufferLock); //unlock mutex
}

//-----------------------------------------------------------------------------
int gpio_export(int gpio)
{
	int gpiofd;
	char buf[SMALL_BUFF_SIZE];
	int ret;


	/* Quick test if it has already been exported */
	sprintf(buf, "/sys/class/gpio/gpio%d/value", gpio);
	gpiofd = open(buf, O_WRONLY);
	if (gpiofd != -1)
	{
		close(gpiofd);
		return 0;
	}

	gpiofd = open("/sys/class/gpio/export", O_WRONLY | O_SYNC);
	if (gpiofd == -1)
	{
		g_logger->print(LOG_ERR, "%s: Cannot modify \"/sys/class/gpio/export\": %s\n", __func__, gpio, strerror(errno));
		return -1;
	}
	else
	{
		sprintf(buf, "%d", gpio);
		ret = write(gpiofd, buf, strlen(buf));
		if (ret < 0)
		{
			g_logger->print(LOG_ERR, "%s: Export GPIO %d failed: %s\n", __func__, gpio, strerror(errno));
			return -2;
		}
		close(gpiofd);
	}

	return 0;
}

//-----------------------------------------------------------------------------
void gpio_unexport(int gpio)
{
	int gpiofd, ret;
	char buf[SMALL_BUFF_SIZE];
	
	
	gpiofd = open("/sys/class/gpio/unexport", O_WRONLY | O_SYNC);
	sprintf(buf, "%d", gpio);
	ret = write(gpiofd, buf, strlen(buf));
	close(gpiofd);
}

//-----------------------------------------------------------------------------
int gpio_direction(int gpio, int dir)
{
	int ret = 0, gpiofd;
	char buf[SMALL_BUFF_SIZE];


	sprintf(buf, "/sys/class/gpio/gpio%d/direction", gpio);
	gpiofd = open(buf, O_WRONLY | O_SYNC);
	if (gpiofd < 0)
	{
		g_logger->print(LOG_ERR, "%s: Couldn't open direction file\n", __func__);
		ret = -1;
	}

	if (dir == 2 && gpiofd)
	{
		if (write(gpiofd, "high", 4) != 4)
		{
			g_logger->print(LOG_ERR, "%s: Couldn't set GPIO %d direction to out/high: %s\n", __func__, gpio, strerror(errno));
			ret = -2;
		}
	}
	else if (dir == 1 && gpiofd)
	{
		if (write(gpiofd, "out", 3) != 3)
		{
			g_logger->print(LOG_ERR, "%s: Couldn't set GPIO %d direction to out/low: %s\n", __func__, gpio, strerror(errno));
			ret = -3;
		}
	}
	else if (gpiofd)
	{
		if (write(gpiofd, "in", 2) != 2)
		{
			g_logger->print(LOG_ERR, "%s: Couldn't set GPIO %d direction to in: %s\n", __func__, gpio, strerror(errno));
			ret = -4;
		}
	}

	close(gpiofd);
	return ret;
}

//-----------------------------------------------------------------------------
int gpio_read(int gpio)
{
	char in[3] = { 0, 0, 0 };
	char buf[SMALL_BUFF_SIZE];
	int nread, gpiofd;


	sprintf(buf, "/sys/class/gpio/gpio%d/value", gpio);
	gpiofd = open(buf, O_RDWR | O_SYNC);
	if (gpiofd < 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to open gpio %d value: %s\n", __func__, gpio, strerror(errno));
		return -1;
	}

	do {
		nread = read(gpiofd, in, 1);
	} while (nread == 0);
	if (nread == -1)
	{
		g_logger->print(LOG_ERR, "%s: gpio %d read failed: %s\n", __func__, gpio, strerror(errno));

		return -2;
	}

	close(gpiofd);
	return atoi(in);
}

//-----------------------------------------------------------------------------
int gpio_write(int gpio, int val)
{
	char buf[50];
	int nread, ret, gpiofd;


	sprintf(buf, "/sys/class/gpio/gpio%d/value", gpio);
	gpiofd = open(buf, O_RDWR);
	if (gpiofd < 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to open gpio %d value: %s\n", __func__, gpio, strerror(errno));
		return -1;
	}

	snprintf(buf, 2, "%d", val ? 1 : 0);
	ret = write(gpiofd, buf, 2);
	if (ret < 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to set gpio %d: %s\n", __func__, gpio, strerror(errno));
		return -2;
	}

	close(gpiofd);
	if (ret == 2) return 0;
}

//-----------------------------------------------------------------------------
int pwm_export(int pwm)
{
	int pwmfd;
	char buf[SMALL_BUFF_SIZE];
	int ret;


	/* Quick test if it has already been exported */
	sprintf(buf, "/sys/class/pwmchip0/pwm%d/enable", pwm);
	pwmfd = open(buf, O_WRONLY);
	if (pwmfd != -1)
	{
		close(pwmfd);
		return 0;
	}

	pwmfd = open("/sys/class/pwm/pwmchip0/export", O_WRONLY | O_SYNC);
	if (pwmfd == -1)
	{
		g_logger->print(LOG_ERR, "%s: Cannot modify PWM: %s\n", __func__, pwm, strerror(errno));
		return -1;
	}
	else
	{
		sprintf(buf, "%d", pwm);
		ret = write(pwmfd, buf, strlen(buf));
		if (ret < 0)
		{
			g_logger->print(LOG_ERR, "%s: Export PWM %d failed: %s\n", __func__, pwm, strerror(errno));
			return -2;
		}
		close(pwmfd);
	}

	return 0;
}

//-----------------------------------------------------------------------------
void pwm_unexport(int pwm)
{
	int pwmfd, ret;
	char buf[SMALL_BUFF_SIZE];


	pwmfd = open("/sys/class/pwm/pwmchip0/unexport", O_WRONLY | O_SYNC);
	sprintf(buf, "%d", pwm);
	ret = write(pwmfd, buf, strlen(buf));
	close(pwmfd);
}

//-----------------------------------------------------------------------------
int pwm_read(int pwm, unsigned long *period_ns, unsigned long *duty_ns, unsigned long *enable)
{
/*
	int pwmfd, ret, nread;
	char buf[SMALL_BUFF_SIZE];
	char read_buf[SMALL_BUFF_SIZE];
	char pwm_parameters[3] = { "period", "duty_cycle", "enable" };
	char *endptr;
	unsigned long *values[3] = { period_ns , duty_ns , enable };


	for (int i = 0; i < ARRAY_SIZE(pwm_parameters); i++)
	{
		sprintf(buf, "/sys/class/pwm/pwmchip0/pwm%d/%s", pwm, pwm_parameters[i]);
		pwmfd = open(buf, O_RDWR | O_SYNC);
		if (pwmfd < 0)
		{
			g_logger->print(LOG_ERR, "%s: Failed to open PWM %d: %s\n", __func__, pwm, strerror(errno));
			return -1;
		}

		nread = read(pwmfd, read_buf, SMALL_BUFF_SIZE);
		if (nread <= 0)
		{
			g_logger->print(LOG_ERR, "%s: Failed to read PWM %d: %s\n", __func__, pwm, strerror(errno));
			return -2
		}

		*(values[i]) = strtol(read_buf, &endptr, 10);

		close(pwmfd);
	}
*/
	return 0;
}

//-----------------------------------------------------------------------------
int pwm_write(int pwm, unsigned long period_ns, unsigned long duty_ns, int enable)
{
/*
	int pwmfd, ret, nread;
	char buf[SMALL_BUFF_SIZE];
	char write_buf[SMALL_BUFF_SIZE];
	char pwm_parameters[3] = { "period", "duty_cycle", "enable" };
	char *endptr;
	unsigned long *values[3] = { &period_ns , &duty_ns , &enable };


	for (int i = 0; i < ARRAY_SIZE(pwm_parameters); i++)
	{
		sprintf(buf, "/sys/class/pwm/pwmchip0/pwm%d/%s", pwm, pwm_parameters[i]);
		pwmfd = open(buf, O_RDWR | O_SYNC);
		if (pwmfd < 0)
		{
			g_logger->print(LOG_ERR, "%s: Failed to open PWM %d: %s\n", __func__, pwm, strerror(errno));
			return -1;
		}

		snprintf(write_buf, SMALL_BUFF_SIZE, "%d", *(values[i]));
		ret = write(pwmfd, write_buf, SMALL_BUFF_SIZE);
		if (ret < 0)
		{
			g_logger->print(LOG_ERR, "%s: Failed to set PWM %d: %s\n", __func__, pwmfd, strerror(errno));
			return -2;
		}

		close(pwmfd);
	}
*/
	return 0;
}