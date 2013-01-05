/*
 * Hacklace.c
 *
 */ 

/**********************************************************************************

Title:				Hacklace - A necklace for hackers

Hardware:			Hacklace-Board with ATtiny4313 running at 4 MHz and a
					5 x 7 dot matrix display.
Author:				Frank Andre
License:			This software is distributed under the creative commons license
					CC-BY-NC-SA.
Disclaimer:			This software is provided by the copyright holder "as is" and any 
					express or implied warranties, including, but not limited to, the 
					implied warranties of merchantability and fitness for a particular 
					purpose are disclaimed. In no event shall the copyright owner or 
					contributors be liable for any direct, indirect, incidental, 
					special, exemplary, or consequential damages (including, but not 
					limited to, procurement of substitute goods or services; loss of 
					use, data, or profits; or business interruption) however caused 
					and on any theory of liability, whether in contract, strict 
					liability, or tort (including negligence or otherwise) arising 
					in any way out of the use of this software, even if advised of 
					the possibility of such damage.
					
**********************************************************************************/

#include <inttypes.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <avr/sleep.h>
#include <util/delay.h>
#include "config.h"
#include "dot_matrix.h"
#include "animations.h"


/*********
* fuses *
*********/

FUSES =
{
	.low = 0xE2,
	.high = 0xDF,
	.extended = 0xFF,
};


/********************
 * global variables *
 ********************/

uint8_t scroll_speed = 8;					// scrolling speed (0 = fastest)
volatile uint8_t button = PB_ACK;			// button event
//uint8_t* msg_ptr = (uint8_t*) messages;		// pointer to next message in EEPROM
uint8_t* msg_ptr;							// pointer to next message in EEPROM
uint8_t* ee_write_ptr = (uint8_t*) messages;


/*************
 * constants *
 *************/
// serial input states
#define IDLE			0
#define AUTH			1		// first authentication byte received
#define RESET			2
#define DISP_SET_MODE	3
#define DISP_CHAR		4
#define EE_NORMAL		5
#define EE_SPECIAL_CHAR	6
#define EE_HEX_CODE		7

#define AUTH1_CHAR		'H'
#define EE_AUTH2_CHAR	'L'		// authentication for entering EEPROM mode
#define DISP_AUTH2_CHAR	'D'		// authentication for entering DISPLAY mode


/**********
 * macros *
 **********/

// Usage: b=swap(a) or b=swap(b)
#define swap(x)													\
	({															\
		unsigned char __x__ = (unsigned char) x;				\
		asm volatile ("swap %0" : "=r" (__x__) : "0" (__x__));	\
		__x__;													\
	})


/*************
 * functions *
 *************/

/*======================================================================
	Function:		InitHardware
	Input:			none
	Output:			none
	Description:	.
======================================================================*/
void InitHardware(void)
{
	// switch all pins that are connected to the dot matrix to output
	DDRA = DISP_MASK_A;
	DDRB = DISP_MASK_B;
	DDRD = DISP_MASK_D;
	
	// enable pull-ups on all input pins to avoid floating inputs
	PORTA |= ~DISP_MASK_A;
	PORTB |= ~DISP_MASK_B;
	PORTD |= ~DISP_MASK_D;
	
	// timer 0
	TCCR0A = (0<<WGM00);				// timer mode = normal
	TCCR0B = (5<<CS00);					// prescaler = 1:1024
	OCR0A = OCR0A_CYCLE_TIME;
	OCR0B = OCR0B_CYCLE_TIME;
	TIMSK |= (1<<OCIE0B)|(1<<OCIE0A);
	
	// serial interface
	// Note: Speed of the serial interface must not be higher than 2400 Baud.
	// This leaves enough time for the EEPROM write operation to complete before 
	// the next byte arrives.
	UBRRL = (uint8_t) (103.5 * SER_CLK_CORRECTION);	// 2400 baud, corrected value
//	UBRRL = 103;						// 2400 baud, ideal value
//	UBRRL = 207;						// 1200 baud, ideal value
	UBRRH = 0;
	UCSRB = (1<<RXCIE)|(1<<RXEN);		// enable receiver, enable RX interrupt
	UCSRC = (3<<UCSZ0);					// async USART, 8 data bits, no parity, 1 stop bit
}


/*======================================================================
	Function:		SetMode
	Input:			mode byte
	Output:			none
	Description:	Set display parameters.
					The mode byte is interpreted as follows:
					Bit 7:		reverse scrolling direction (0 = no, always scroll forward, 1 = yes, bidirectional scrolling)
					Bit 6..4:	delay between scrolling repetitions (0 = shortest, 7 = longest)
					Bit 3:		scrolling increment (cleared = +1 (for texts), set = +5 (for animations))
					Bit 2..0:	scrolling speed (1 = slowest, 7 = fastest)
======================================================================*/
void SetMode(uint8_t mode)
{
	uint8_t inc, dir, dly, spd;

	if (mode & 0x08)	{ inc = 5; }
		else			{ inc = 1; }
	if (mode & 0x80)	{ dir = BIDIRECTIONAL; }
		else			{ dir = FORWARD; }
	spd = mode & 0x07;
	dly = swap(mode) & 0x07;
	dmSetScrolling(inc, dir, pgm_read_byte(&dly_conv[dly]));
	scroll_speed = pgm_read_byte(&spd_conv[spd]);
}		


/*======================================================================
	Function:		DisplayMessage
	Input:			pointer to zero terminated message data in EEPROM memory
	Output:			pointer to next message
	Description:	Show a message (i. e. text or animation) on the display.

					Escape characters:

					Character '^' is used to access special characters by 
					shifting the character code of the next character by 96 
					so that e. g. '^A' becomes char(161).
					To enter a '^' character simply double it: '^^'

					Character '~' followed by an upper case letter is used
					to insert (animation) data from flash.
					
					The character 0xFF is used to enter direct mode in which 
					the following bytes are directly written to the display 
					memory without being decoded using the character font.
					Direct mode is ended by 0xFF.
======================================================================*/
uint8_t* DisplayMessage(uint8_t* ee_adr)
{
	uint8_t ch;

	SetMode(eeprom_read_byte(ee_adr));
	ee_adr++;
	dmClearDisplay();

	ch = eeprom_read_byte(ee_adr++);
	while (ch) {
		if (ch == '~') {					// animation
			ch = eeprom_read_byte(ee_adr++);
			if (ch != '~') {
				ch -= 'A';
				if (ch < ANIMATION_COUNT) {
					dmDisplayImage((const uint8_t*)pgm_read_word(&animation[ch]));
				}				
			}
		}
		else if (ch == 0xFF) {				// direct mode
			ch = eeprom_read_byte(ee_adr++);
			while (ch != 0xFF) {
				dmPrintByte(ch);
				ch = eeprom_read_byte(ee_adr++);
			}
		}
		else {								// character
			if (ch == '^') {				// special character
				ch = eeprom_read_byte(ee_adr++);
				if (ch != '^') {
					ch += 63;
				}
			}
			dmPrintChar(ch);
		}
		ch = eeprom_read_byte(ee_adr++);
		if (ch) { dmPrintByte(0); }			// print a narrow space except for the last character					
	}
	ch = eeprom_read_byte(ee_adr);			// read mode byte of next message
	if (ch)		{ return(ee_adr); }
		else	{ return((uint8_t*) messages); }	// restart all-over if mode byte is 0
}


/*======================================================================
	Function:		GoToSleep
	Input:			none
	Output:			none
	Description:	Put the controller into sleep mode and prepare for
					wake-up by a pin change interrupt.
======================================================================*/
void GoToSleep(void)
{
	dmClearDisplay();
	_delay_ms(1000);
	GIFR = (1<<PCIF2);				// clear interrupt flag
	PCMSK2 = (1<<PCINT17);			// enable pin change interrupt
	GIMSK = (1<<PCIE2);				// enable pin change interrupt
	set_sleep_mode(SLEEP_MODE_PWR_DOWN);
	sleep_mode();
	GIMSK = 0;						// disable all external interrupts (including pin change)
	dmPrintChar(131);				// happy smiley
	_delay_ms(500);
	msg_ptr = DisplayMessage((uint8_t*) messages);
}


/********
 * main *
 ********/

int main(void)
{
	InitHardware();
	dmInit();
	sei();									// enable interrupts

	GoToSleep();
	button |= PB_ACK;

	while(1)
	{
		if (button == PB_RELEASE) {			// short button press
			msg_ptr = DisplayMessage(msg_ptr);
			button |= PB_ACK;
		}
		
		if (button == PB_LONGPRESS) {		// button pressed for some seconds
			dmClearDisplay();
			dmPrintChar(130);				// sad smiley
			_delay_ms(500);
			GoToSleep();
			button |= PB_ACK;
		}
		
	} // of while(1)
}


/******************************
 * interrupt service routines *
 ******************************/

ISR(TIMER0_COMPA_vect)
// display interrupt
{
	OCR0A += OCR0A_CYCLE_TIME;				// setup next cycle

	dmDisplay();							// show next column on dot matrix display
}


ISR(TIMER0_COMPB_vect)
// system timer interrupt
{
	static uint8_t scroll_timer = 1;
	static uint8_t pb_timer = 0;			// push button timer
	uint8_t temp;
		
	OCR0B += OCR0B_CYCLE_TIME;				// setup next cycle

	if (scroll_timer) {
		scroll_timer--;
	}
	else {
		scroll_timer = scroll_speed;		// restart timer
		dmScroll();							// do a scrolling step
	}
	
	// push button sampling
	temp = ~PB_PIN;							// sample push button
	temp &= PB_MASK;						// extract push button state
	if (temp == 0) {						// --- button not pressed ---
		if (button & PB_PRESS) {			// former state = pressed?
			button &= ~(PB_PRESS | PB_ACK);	// -> issue release event
		}
	}
	else {									// --- button pressed ---
		if ((button & PB_PRESS) == 0) {		// former state = button released?
			button = PB_PRESS;				// issue new press event
			pb_timer = PB_LONGPRESS_DELAY;	// start push button timer
		}
		else {
			if (button == PB_PRESS) {		// holding key pressed
				if (pb_timer == 0) {		// if push button timer has elapsed
					button = PB_LONGPRESS;	// issue long event
				}
				else {
					pb_timer--;
				}
			}			
		}		
	}
	
}


ISR(PCINT_D_vect)
// pin change interrupt (for wake-up)
{
}



ISR(USART0_RX_vect)
{
	static uint8_t state = IDLE;
	static uint8_t val;
	uint8_t ch;

	if (UCSRA & (1<<FE)) { return; }	// framing error? -> return
	ch = UDR;							// read received character
	if (ch == 27) { state = RESET; }	// <ESC> resets the state machine
	if (state >= EE_NORMAL) {
		dmClearDisplay();
		dmPrintChar(ch);
	}	

	switch (state) {
		case IDLE:
			if (ch == AUTH1_CHAR)	{ state = AUTH; }
			else					{ state = IDLE; }
			break;
		case AUTH:
			if (ch == EE_AUTH2_CHAR)		{ state = EE_NORMAL; }
			else if (ch == DISP_AUTH2_CHAR)	{ state = DISP_SET_MODE; }
			else							{ state = IDLE; }
			break;
		case RESET:
			msg_ptr = (uint8_t*) messages;
			ee_write_ptr = (uint8_t*) messages;
			dmClearDisplay();
			dmPrintChar(129);						// show logo
			state = IDLE;
			break;
		case DISP_SET_MODE:
			dmClearDisplay();
			SetMode(ch);
			state = DISP_CHAR;
			break;
		case DISP_CHAR:
			if ((ch == 13) || (ch == 10)) {	dmClearDisplay(); }		// chr(13) = <CR>, chr(10) = <LF>
			else { dmPrintChar(ch); dmPrintByte(0); }				// print character followed by empty column
			break;
		case EE_NORMAL:
			if (ch == '^') { state = EE_SPECIAL_CHAR; }
			else if (ch == '$') { val = 0;  state = EE_HEX_CODE; }
			else if ((ch == 13) || (ch == 10)) {	// chr(13) = <CR>, chr(10) = <LF>
				eeprom_write_byte(ee_write_ptr++, 0);
			}
			else if (ch >= ' ') {					// ignore non-printing characters
				eeprom_write_byte(ee_write_ptr++, ch);
			}
			break;
		case EE_SPECIAL_CHAR:
			eeprom_write_byte(ee_write_ptr++, ch+63);
			state = EE_NORMAL;
			break;
		case EE_HEX_CODE:
			if (ch >= 'A') { ch -= ('A' - '9' - 1); }
			ch -= '0';								// map characters '0'..'9' and 'A'..'F' to values 0..15
			if (ch > 15) {							// any character below '0' or above 'F' terminates hex input
				eeprom_write_byte(ee_write_ptr++, val);
				state = EE_NORMAL;
			}
			else { val <<= 4;  val += ch; }
			break;
	}
}


/*
ISR(TIMER1_COMPA_vect)
{
}


ISR(TIMER1_COMPB_vect)
{
}
*/
