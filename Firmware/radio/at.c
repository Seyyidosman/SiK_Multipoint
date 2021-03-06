// -*- Mode: C; c-basic-offset: 8; -*-
//
// Copyright (c) 2011 Michael Smith, All Rights Reserved
// Copyright (c) 2013 Luke Hovington, All Rights Reserved
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
//  o Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  o Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in
//    the documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
// INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
// STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
// OF THE POSSIBILITY OF SUCH DAMAGE.
//

///
/// @file	at.c
///
/// A simple AT command parser.
///

#include "radio.h"
#include "tdm.h"

// canary data for ram wrap. It is in at.c as the compiler
// assigns addresses in alphabetial order and we want this at a low
// address
__pdata uint8_t pdata_canary = 0x41;

// AT command buffer
__pdata char at_cmd[AT_CMD_MAXLEN + 1];
__pdata uint8_t	at_cmd_len;

// Index
__pdata uint8_t		idx;

// mode flags
bool		at_mode_active;	///< if true, incoming bytes are for AT command
bool		at_cmd_ready;	///< if true, at_cmd / at_cmd_len contain valid data

// test bits
__pdata uint8_t		at_testmode;    ///< test modes enabled (AT_TEST_*)

// command handlers
static void	at_ok(void);
static void	at_error(void);
static void	at_i(void);
static void	at_s(void);
static void	at_p(void);
static void	at_ampersand(void);
static void	at_plus(void);

#pragma save
#pragma nooverlay
void
at_input(register uint8_t c)
{
	// AT mode is active and waiting for a command
	switch (c) {
		// CR - submits command for processing
	case '\r':
		putchar('\n');
		at_cmd[at_cmd_len] = 0;
		at_cmd_ready = true;
		break;

		// backspace - delete a character
		// delete - delete a character
	case '\b':
	case '\x7f':
		if (at_cmd_len > 0) {
			putchar('\b');
			putchar(' ');
			putchar('\b');
			at_cmd_len--;
		}
		break;

		// character - add to buffer if valid
	default:
		if (at_cmd_len < AT_CMD_MAXLEN) {
			if (isprint(c)) {
				c = toupper(c);
				at_cmd[at_cmd_len++] = c;
				putchar(c);
			}
			break;
		}

		// If the AT command buffer overflows we abandon
		// AT mode and return to passthrough mode; this is
		// to minimise the risk of locking up on reception
		// of an accidental escape sequence.

		at_mode_active = 0;
		at_cmd_len = 0;
		break;
	}
}
#pragma restore

// +++ detector state machine
//
// states:
//
// wait_for_idle:	-> wait_for_plus1, count = 0 after 1s
//
// wait_for_plus:	-> wait_for_idle if char != +
//			-> wait_for_plus, count++ if count < 3
// wait_for_enable:	-> enabled after 1s
//			-> wait_for_idle if any char
//

#define ATP_WAIT_FOR_IDLE   0
#define ATP_WAIT_FOR_PLUS1  1
#define ATP_WAIT_FOR_PLUS2  2
#define ATP_WAIT_FOR_PLUS3  3
#define ATP_WAIT_FOR_ENABLE 4

#define ATP_COUNT_1S		100	// 100 ticks of the 100Hz timer

static __pdata uint8_t	at_plus_state;
static __pdata uint8_t	at_plus_counter = ATP_COUNT_1S;

#pragma save
#pragma nooverlay
void
at_plus_detector(register uint8_t c)
{
	// If we get a character that's not '+', unconditionally
	// reset the state machine to wait-for-idle; this will
	// restart the 1S timer.
	//
	if (c != (uint8_t)'+')
		at_plus_state = ATP_WAIT_FOR_IDLE;

	// We got a plus; handle it based on our current state.
	//
	switch (at_plus_state) {

	case ATP_WAIT_FOR_PLUS1:
	case ATP_WAIT_FOR_PLUS2:
		at_plus_state++;
		break;

	case ATP_WAIT_FOR_PLUS3:
		at_plus_state = ATP_WAIT_FOR_ENABLE;
		at_plus_counter = ATP_COUNT_1S;
		break;

	default:
		at_plus_state = ATP_WAIT_FOR_IDLE;
		// FALLTHROUGH
	case ATP_WAIT_FOR_IDLE:
	case ATP_WAIT_FOR_ENABLE:
		at_plus_counter = ATP_COUNT_1S;
		break;
	}
}
#pragma restore

#pragma save
#pragma nooverlay
void
at_timer(void)
{
	// if the counter is running
	if (at_plus_counter > 0) {

		// if it reaches zero, the timeout has expired
		if (--at_plus_counter == 0) {

			// make the relevant state change
			switch (at_plus_state) {
			case ATP_WAIT_FOR_IDLE:
				at_plus_state = ATP_WAIT_FOR_PLUS1;
				break;

			case ATP_WAIT_FOR_ENABLE:
#if defined _BOARD_RFD900A && defined WATCH_DOG_ENABLE
					// 0x40 = Disable Watchdog
					PCA0MD &= ~0x40;
#endif // _BOARD_RFD900A
				at_mode_active = true;
				at_plus_state = ATP_WAIT_FOR_IDLE;

				// stuff an empty 'AT' command to get the OK prompt
				at_cmd[0] = 'A';
				at_cmd[1] = 'T';
				at_cmd[2] = '\0';
				at_cmd_len = 2;
				at_cmd_ready = true;
				break;
			default:
				// should never happen, but otherwise harmless
			}
		}
	}
}
#pragma restore

static uint32_t
at_parse_number() __reentrant
{
	uint32_t	reg;
	uint8_t		c;
	
	reg = 0;
	for (;;) {
		c = at_cmd[idx];
		if (!isdigit(c))
			break;
		reg = (reg * 10) + (c - '0');
		idx++;
	}
	return reg;
}

void
at_command(void)
{
	__pdata uint16_t	destination;
	
	// require a command with the AT prefix
	if (at_cmd_ready) {
		if ((at_cmd_len >= 2) && (at_cmd[0] == 'R') && (at_cmd[1] == 'T')) {
			// remote AT command - send it to the tdm
			// system to send to the remote radio
			
			// get the register number first
			idx = 3;
			for (;;) {
				if (at_cmd_len <= idx || at_cmd[idx] == ',')
					break;
				idx++;
			}
			
			// If the RT command has a ',' followed by a destination
			// send the RT packet to this node only
			if(at_cmd[idx] == ',') {
				at_cmd[idx++] = '\0';
				destination = at_parse_number();
				tdm_remote_at(destination);
			}
			else {
				tdm_remote_at(0xFFFF); // 65535 = broadcast
			}
			at_cmd_len = 0;
			at_cmd_ready = false;
			return;
		}
		
		if ((at_cmd_len >= 2) && (at_cmd[0] == 'A') && (at_cmd[1] == 'T')) {

			// look at the next byte to determine what to do
			switch (at_cmd[2]) {
			case '\0':		// no command -> OK
				at_ok();
				break;
			case '&':
				at_ampersand();
				break;
			case '+':
				at_plus();
				break;
			case 'I':
				at_i();
				break;
			case 'P':
				at_p();
				break;
			case 'O':		// O -> go online (exit command mode)
				at_plus_counter = ATP_COUNT_1S;
				at_mode_active = 0;
					
#if defined _BOARD_RFD900A && defined WATCH_DOG_ENABLE
					// 0x40 = Enable Watchdog
					PCA0MD |= 0x40;
#endif // _BOARD_RFD900A
				break;
			case 'S':
				at_s();
				break;

			case 'Z':
				// generate a software reset
				RSTSRC |= (1 << 4);
				for (;;)
					;

			default:
				at_error();
			}
		}

		// unlock the command buffer
		at_cmd_len = 0;
		at_cmd_ready = false;
	}
}

static void
at_ok(void)
{
	printf("[%u] OK\n", nodeId);
}

static void
at_error(void)
{
	printf("[%u] ERROR\n", nodeId);
}

static void
at_i(void)
{
	__pdata uint8_t	id;
	switch (at_cmd[3]) {
	case '\0':
	case '0':
		printf("[%u] %s\n", nodeId, g_banner_string);
		return;
	case '1':
		printf("[%u] %s\n", nodeId, g_version_string);
		return;
	case '2':
		printf("[%u] %u\n", nodeId, BOARD_ID);
		break;
	case '3':
		printf("[%u] %u\n", nodeId, g_board_frequency);
		break;
	case '4':
		printf("[%u] %u\n", nodeId, g_board_bl_version);
		return;
	case '5': {
		for (id = 0; id < PARAM_MAX; id++) {
			param_print(id);
		}
		return;
	}
	case '6':
		tdm_report_timing();
		return;
	case '7':
		tdm_show_rssi();
		return;
	case '8':
		if (nodeId == 0)
		{
			printf("[%u] Sync: Base\n", nodeId);
		}
		else
		{
			printf("[%u] Sync: %u\n", nodeId, tdm_state_sync());
		}
		return;
	default:
		at_error();
		return;
	}
}

static void
at_s(void)
{
	__pdata uint8_t		sreg;
	__pdata uint32_t	val;

	// get the register number first
	idx = 3;
	sreg = at_parse_number();
	// validate the selected sreg
	if (sreg >= PARAM_MAX) {
		at_error();
		return;
	}

	switch (at_cmd[idx]) {
	case '?':
		val = param_get(sreg);
		printf("[%u] %lu\n", nodeId, val);
		return;

	case '=':
		if (sreg > 0) {
			idx++;
			val = at_parse_number();
			if (param_set(sreg, val)) {
				at_ok();
				return;
			}
		}
		break;
	}
	at_error();
}

static void
at_ampersand(void)
{
	switch (at_cmd[3]) {
	case 'F':
		param_default();
		at_ok();
		break;
	case 'W':
		param_save();
		at_ok();
		break;

	case 'U':
		if (!strcmp(at_cmd + 4, "PDATE")) {
			// force a flash error
			volatile char x = *(__code volatile char *)0xfc00;
			for (;;)
				;
		}
		at_error();
		break;

	case 'T':
		// enable test modes
		if (!strcmp(at_cmd + 4, "")) {
			// disable all tests
			at_testmode = 0;
		} else if (!strcmp(at_cmd + 4, "=RSSI")) {
			// display RSSI stats
			at_testmode ^= AT_TEST_RSSI;
		} else if (!strcmp(at_cmd + 4, "=TDM")) {
			// display TDM debug
			at_testmode ^= AT_TEST_TDM;
		} else {
			at_error();
		}
		break;
		
	default:
		at_error();
		break;
	}
}

static void
at_p (void)
{
	__pdata uint8_t pinId;
	if(at_cmd[3] == 'P')
	{
		for (pinId = 0; pinId < PIN_MAX; pinId++)
		{
			printf("[%u] Pin:%d ",nodeId, pinId);
			if (pins_user_get_io(pinId))
				printf("Output ");
			else
				printf("Input  ");
			printf("Val: %d\n",pins_user_get_value(pinId));
		}
		return;
	}
	else if(at_cmd[4] != '=' || !isdigit(at_cmd[5]))
	{
		at_error();
		return;
	}
	
	pinId = at_cmd[5] - '0';

	switch (at_cmd[3]) {
			
		// Set pin to output, turn mirroring off pulling pin to ground
        // ATPO=1        Set Pin to Output
		case 'O':
			pins_user_set_io(pinId, PIN_OUTPUT);
			break;

		// ATPI=1        Set Pin to Input
		case 'I':
			pins_user_set_io(pinId, PIN_INPUT);
			break;
		
        // ATPR=1        Read Pin Direction
		case 'R':
			if(pins_user_get_io(pinId) == PIN_INPUT)
				printf("[%u] val:%u\n", nodeId, pins_user_get_adc(pinId));
			else
				at_error();
			return;
			break;
			
        // ATPC=1,1      Turn pin on
		case 'C':
			if(!isdigit(at_cmd[7]) || !pins_user_set_value(pinId, (at_cmd[7]-'0')))
			{
				at_error();
				return;
			}
			break;
        
        // ATPM=1,1,0    Mirror pin on node 0
        // ATPM=1        Turn pin mirroring off
//        case 'M':
//			if(!isdigit(at_cmd[7]) || !pins_user_set_value(pinId, (at_cmd[7]-'0')))
//			{
//				at_error();
//				return;
//			}
//			break;
		default:
			at_error();
			return;
	}
	
	at_ok();
}

static void
at_plus(void)
{
#ifdef BOARD_rfd900a
	__pdata uint8_t		creg;
	__pdata uint32_t	val;

	// get the register number first
	idx = 4;
	creg = at_parse_number();

	switch (at_cmd[3])
	{
	case 'P': // AT+P=x set power level pwm to x immediately
		if (at_cmd[4] != '=')
		{
			break;
		}
		idx = 5;
		val = at_parse_number();
		PCA0CPH0 = val & 0xFF;
		radio_set_diversity(false);
		at_ok();
		return;
	case 'C': // AT+Cx=y write calibration value
		switch (at_cmd[idx])
		{
		case '?':
			val = calibration_get(creg);
			printf("%lu\n",val);
			return;
		case '=':
			idx++;
			val = at_parse_number();
			if (calibration_set(creg, val&0xFF))
			{
				at_ok();
			} else {
				at_error();
			}
			return;
		}
		break;
	case 'L': // AT+L lock bootloader area if all calibrations written
		if (calibration_lock())
		{
			at_ok();
		} else {
			at_error();
		}
		return;
	}
#endif //BOARD_rfd900a
	at_error();
}
