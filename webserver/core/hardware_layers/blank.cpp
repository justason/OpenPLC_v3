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
#include <gpiod.h>

#include "ladder.h"
#include "custom_layer.h"
#include "logger.h"

#if !defined(ARRAY_SIZE)
#define ARRAY_SIZE(x) (sizeof((x)) / sizeof((x)[0]))
#endif

//#define DUTY_SCALING      30.518043793392843518730449378195
#define PWM_PERIOD_500HZ  2000000
#define PWM_DUTY_SCALING  20000

#define MAX_GPIO_INPUTS	  16
#define MAX_GPIO_OUTPUTS  16
#define MAX_PWM_OUTPUTS   16

#define GPIO_IN_OFFSET    480
#define GPIO_OUT_OFFSET   464

#define LOG_MSG_LENGTH    256
#define SMALL_BUFF_SIZE   256

int pwm_export(int pwm);
void pwm_unexport(int pwm);
IEC_UDINT pwm_read(int pwm);
int write_pwm_param(int pwm, const char* param_name, IEC_UDINT value);
int pwm_write(int pwm, IEC_UDINT value);

Logger *g_logger;
IEC_UDINT g_int_pwm_val_buffer[MAX_PWM_OUTPUTS];
IEC_BOOL g_bool_pwm_en_buffer[MAX_PWM_OUTPUTS];


struct gpiod_chip *g_gpio_input_chip;
struct gpiod_chip *g_gpio_output_chip;
struct gpiod_line_bulk g_bulk_in;
struct gpiod_line_bulk g_bulk_out;

//-----------------------------------------------------------------------------
// This function is called by the main OpenPLC routine when it is initializing.
// Hardware initialization procedures should be here.
//-----------------------------------------------------------------------------
void initializeHardware()
{
	int rv;
	int def_vals[MAX_GPIO_OUTPUTS];


	g_logger = new Logger(LOG_MEDIA_FILE, "/tmp/log.txt");
	g_logger->print(LOG_INFO, "%s: Starting\n", __func__);

	// Configure digital inputs
	g_gpio_input_chip = gpiod_chip_open_lookup("gpiochip6");
	if (g_gpio_input_chip == NULL)
	{
		g_logger->print(LOG_ERR, "%s: Failed to open \"gpiochip6\" Error: %s\n", __func__, strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (gpiod_chip_get_all_lines(g_gpio_input_chip, &g_bulk_in) != 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to retrieve gpio lines.\n", __func__);
		exit(EXIT_FAILURE);
	}
	if (gpiod_line_request_bulk_input(&g_bulk_in, "OPLC") != 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to assign gpio lines IN.\n", __func__);
		exit(EXIT_FAILURE);
	}

	// Configure digital outputs
	for (int i = 0; i < MAX_GPIO_OUTPUTS; i++)
	{
		def_vals[i] = 0;
	}

	g_gpio_output_chip = gpiod_chip_open_lookup("gpiochip7");
	if (g_gpio_output_chip == NULL)
	{
		g_logger->print(LOG_ERR, "%s: Failed to open \"gpiochip7\" Error: %s\n", __func__, strerror(errno));
		exit(EXIT_FAILURE);
	}
	if (gpiod_chip_get_all_lines(g_gpio_output_chip, &g_bulk_out) != 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to retrieve \"gpiochip7\" lines.\n", __func__);
		exit(EXIT_FAILURE);
	}
	if (gpiod_line_request_bulk_output(&g_bulk_out, "OPLC", def_vals) != 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to assign gpio lines OUT.\n", __func__);
		exit(EXIT_FAILURE);
	}

	// Configure PWM outputs
	for (int i = 0; i < MAX_PWM_OUTPUTS; i++)
	{
		if (pinNotPresent(ignored_int_outputs, ARRAY_SIZE(ignored_int_outputs), i))
		{
			pwm_export(i);
		}
		g_int_pwm_val_buffer[i] = 0;
		g_bool_pwm_en_buffer[i] = 0;
		write_pwm_param(i, "period", PWM_PERIOD_500HZ);
	}
	g_logger->print(LOG_INFO, "%s: Finished\n", __func__);
}

//-----------------------------------------------------------------------------
void finalizeHardware()
{
	g_logger->print(LOG_INFO, "%s: Starting\n", __func__);

	// Configure digital inputs
	if (g_gpio_input_chip != NULL)
	{
		gpiod_line_release_bulk(&g_bulk_in);
		gpiod_chip_close(g_gpio_input_chip);
	}

	// Configure digital outputs
	if (g_gpio_output_chip != NULL)
	{
		gpiod_line_release_bulk(&g_bulk_out);
		gpiod_chip_close(g_gpio_output_chip);
	}

	// Configure PWM outputs
	for (int i = 0; i < MAX_PWM_OUTPUTS; i++)
	{
		if (pinNotPresent(ignored_int_outputs, ARRAY_SIZE(ignored_int_outputs), i))
		{
			pwm_unexport(i);
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
	const int gpio_values[MAX_GPIO_INPUTS];


	pthread_mutex_lock(&bufferLock); //lock mutex

	gpiod_line_get_value_bulk(&g_bulk_in, gpio_values);

	for (int i = 0; i < MAX_GPIO_INPUTS; i++)
	{
		if (pinNotPresent(ignored_bool_inputs, ARRAY_SIZE(ignored_bool_inputs), i))
		{
			if (bool_input[i / 8][i % 8] != NULL)
			{
				*bool_input[i / 8][i % 8] = gpio_values[i];
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
	int gpio_values[MAX_GPIO_OUTPUTS];


	pthread_mutex_lock(&bufferLock); //lock mutex

	// GPIO OUT
	for (int i = 0; i < MAX_GPIO_OUTPUTS; i++)
	{
		gpio_values[i] = 0;
		if (pinNotPresent(ignored_bool_outputs, ARRAY_SIZE(ignored_bool_outputs), i))
		{
			if (bool_output[i / 8][i % 8] != NULL)
			{
				gpio_values[i] = *bool_output[i / 8][i % 8];
			}
		}
	}
	gpiod_line_set_value_bulk(&g_bulk_out, gpio_values);

	//ANALOG OUT (PWM)
	for (int i = 0; i < MAX_PWM_OUTPUTS; i++)
	{
		if (pinNotPresent(ignored_int_outputs, ARRAY_SIZE(ignored_int_outputs), i))
		{
			if (int_output[i] != NULL)
			{
				pwm_write(i, *int_output[i]);
			}
		}
	}

	pthread_mutex_unlock(&bufferLock); //unlock mutex
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
		g_logger->print(LOG_ERR, "%s: Cannot open pwmchip0: %s\n", __func__, pwm, strerror(errno));
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

	g_logger->print(LOG_INFO, "%s: Finished PWM #%d\n", __func__, pwm);
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
	
	g_logger->print(LOG_INFO, "%s: Finished PWM #%d\n", __func__, pwm);
}

//-----------------------------------------------------------------------------
IEC_UDINT pwm_read(int pwm)
{
	int pwmfd, nread;
	char buf[SMALL_BUFF_SIZE];
	char read_buf[SMALL_BUFF_SIZE];
	char *endptr = read_buf;
	IEC_UINT retval = 0;


	sprintf(buf, "/sys/class/pwm/pwmchip0/pwm%d/duty_cycle", pwm);
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
		return -2;
	}

	close(pwmfd);

	retval = (IEC_UINT)(strtod(read_buf, &endptr) / PWM_DUTY_SCALING);
	return retval;
}

//-----------------------------------------------------------------------------
/*
PCA9685 PWM write logic.

500Hz period = 2000000
IEC_UINT: [0, 65535]
duty cycle: [0, 2000000]
when duty cycle == 0 -> disable

# echo 2000000 > /sys/class/pwm/pwmchip0/pwmX/period"
# echo 500000 > /sys/class/pwm/pwmchip0/pwmX/duty_cycle
*/

int write_pwm_param(int pwm, const char* param_name, IEC_UDINT value)
{
	int pwmfd, ret;
	char name_buf[SMALL_BUFF_SIZE];
	char write_buf[SMALL_BUFF_SIZE];


	//g_logger->print(LOG_INFO, "%s: PWM %d <-- \"%s\" : %lu\n", __func__, pwm, param_name, value);

	sprintf(name_buf, "/sys/class/pwm/pwmchip0/pwm%d/%s", pwm, param_name);
	pwmfd = open(name_buf, O_RDWR | O_SYNC);
	if (pwmfd < 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to open PWM %d: %s\n", __func__, pwm, strerror(errno));
		return -1;
	}

	snprintf(write_buf, SMALL_BUFF_SIZE, "%lu", value);
	ret = write(pwmfd, write_buf, SMALL_BUFF_SIZE);
	if (ret < 0)
	{
		g_logger->print(LOG_ERR, "%s: Failed to set PWM %d: %s\n", __func__, pwmfd, strerror(errno));
		return -2;
	}

	close(pwmfd);

	return 0;
}

//-----------------------------------------------------------------------------
int pwm_write(int pwm, IEC_UDINT value)
{
	IEC_UDINT pwm_value;


	pwm_value = (unsigned long)((double)value * PWM_DUTY_SCALING);
	if (pwm_value != g_int_pwm_val_buffer[pwm])
	{
		g_int_pwm_val_buffer[pwm] = pwm_value;
		write_pwm_param(pwm, "duty_cycle", pwm_value);

		if (g_bool_pwm_en_buffer[pwm] != 1)
		{
			g_bool_pwm_en_buffer[pwm] = 1;
			write_pwm_param(pwm, "enable", 1);
		}
	}

	return 0;
}