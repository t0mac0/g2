/*
 * xio.cpp - extended IO functions
 * This file is part of the TinyG2 project
 *
 * Copyright (c) 2013 - 2014 Alden S. Hart Jr.
 * Copyright (c) 2013 - 2014 Robert Giseburt
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
/*
 * XIO acts as an entry point into lower level IO routines - mostly serial IO. It supports
 * the USB, SPI and fiol IO sub-systems, as well as providing low level character functions
 * used by stdio (printf()).
 */
#include "tinyg2.h"
#include "config.h"
#include "util.h"
#include "hardware.h"
#include "text_parser.h"
#include "xio.h"

/**** Structures ****/
/*
struct xioChannel_t {					// description of a channel
	uint8_t type;						// channel type - control or data
	uint8_t caps;						// channel capabilities: R, RW, I, IC
//	uint8_t state;						// channel state
	int8_t device;						// device or file handle channel is bound to
};
*/

struct xioDevice_t {					// description pf a device for reading and writing
	// connection management
	uint8_t device_state;				// physical device state
	uint8_t next_state;					// transitional device state
	uint16_t flags;						// bitfield for device flags (See // Device flags in .h)

	// line reader functions
	uint16_t linelen;					// length of completed line
	uint16_t read_index;				// index into line being read
	uint16_t read_buffer_len;			// static variable set at init time.
	char_t read_buf[READ_BUFFER_LEN];	// primary input buffer
};

struct xioDeviceWrapperBase {			// C++ base class for device primitives
	virtual int16_t readchar() = 0;		// Pure virtual. Will be subclassed for every device
//	virtual int16_t write() = 0;		// Pure virtual. Will be subclassed for every device
};

// Use a templated subclass so we don't have to create a new subclass for every type of Device.
// All this requires is the readByte() function to exist in type Device.
// Note that we expect Device to be a pointer type!
// See here for a discussion of what this means if you are not familiar with C++
// https://github.com/synthetos/g2/wiki/Dual-Endpoint-USB-Internals#c-classes-virtual-functions-and-inheritance

template<typename Device>
struct xioDeviceWrapper : xioDeviceWrapperBase {	// describes a device for reading and writing
    Device _dev;
    xioDeviceWrapper(Device  dev) : _dev{dev} {};

    int16_t readchar() final {
        return _dev->readByte();		// readByte calls the USB endpoint's read function
    };
};

// ALLOCATIONS
// Declare a device wrapper class for USB0 and USB1 and put pointer in an array as elements 0 and 1
xioDeviceWrapper<decltype(&SerialUSB)> serialUSB0Wrapper {&SerialUSB};
xioDeviceWrapper<decltype(&SerialUSB1)> serialUSB1Wrapper {&SerialUSB1};
xioDeviceWrapperBase* DeviceWrappers[] {&serialUSB0Wrapper, &serialUSB1Wrapper};

static xioDevice_t _ds[DEV_MAX];			// allocate device structures

// Singleton acts and namespace for xio
struct xioSingleton_t {
	uint16_t magic_start;							// magic number to test memory integrity
	xioDevice_t* d[DEV_MAX] {&_ds[0], &_ds[1]};		// initialize pointers to device structures
	xioDeviceWrapperBase *DeviceWrappers[];			// access device wrappers from within the singleton
	uint8_t spi_state;								// tick down-counter (unscaled)
	uint16_t magic_end;
};
xioSingleton_t xio;
//extern xioSingleton_t xio; // not needed externally)

// convenience macros
#define usb0 xio.d[DEV_USB0]		// pointer to device structure for USB0 serial
#define usb1 xio.d[DEV_USB1]		// pointer to device structure for USB1 serial

/**** CODE ****/

/*
 * read, set and clear flags
 */
inline bool canRead(xioDevice_t *d)	{ return d->flags & DEVICE_CAN_READ; }
inline bool canWrite(xioDevice_t *d) { return d->flags & DEVICE_CAN_WRITE; }
inline bool canBeCtrl(xioDevice_t *d) { return d->flags & DEVICE_CAN_BE_CTRL; }
inline bool canBeData(xioDevice_t *d) { return d->flags & DEVICE_CAN_BE_DATA; }
inline bool isCtrl(xioDevice_t *d) { return d->flags & DEVICE_IS_CTRL; }
inline bool isData(xioDevice_t *d) { return d->flags & DEVICE_IS_DATA; }
inline bool isPrimary(xioDevice_t *d) {return d->flags & DEVICE_IS_PRIMARY; }

inline void setCtrl(xioDevice_t *d) { d->flags |= DEVICE_IS_CTRL; }
inline void clrCtrl(xioDevice_t *d) { d->flags &= ~DEVICE_IS_CTRL; }
inline void setData(xioDevice_t *d) { d->flags |= DEVICE_IS_DATA; }
inline void clrData(xioDevice_t *d) { d->flags &= ~DEVICE_IS_DATA; }
inline void setPrimary(xioDevice_t *d) { d->flags |= DEVICE_IS_PRIMARY; }
inline void clrPrimary(xioDevice_t *d) { d->flags &= ~DEVICE_IS_PRIMARY; }

// Naiive data channel shutdown function. No allowance for buffered data (buffer clears, etc.)
void downData(xioDevice_t *d)
{
	clrData(d);
}

/*
 * xio_init()
 */

void xio_init()
{
	uint8_t i;

    xio_init_assertions();

	for (i=0; i<DEV_MAX; i++) {
		memset(&xio.d[i], 0, sizeof(xioDevice_t));	// clear states and all values
	}

	// set up USB device state change callbacks
	// See here for info on lambda functions:
	// http://www.cprogramming.com/c++11/c++11-lambda-closures.html

	// setup for USBserial0
	SerialUSB.setConnectionCallback([&](bool connected) {
		usb0->next_state = connected ? DEVICE_CONNECTED : DEVICE_NOT_CONNECTED;
	});
	usb0->read_buffer_len = READ_BUFFER_LEN;
	usb0->flags = DEVICE_CAN_READ | DEVICE_CAN_WRITE | DEVICE_CAN_BE_CTRL | DEVICE_CAN_BE_DATA;

	// setup for USBserial1
	SerialUSB1.setConnectionCallback([&](bool connected) {
		usb1->next_state = connected ? DEVICE_CONNECTED : DEVICE_NOT_CONNECTED;
	});
	usb1->read_buffer_len = READ_BUFFER_LEN;
	usb0->flags = DEVICE_CAN_READ | DEVICE_CAN_WRITE | DEVICE_CAN_BE_CTRL | DEVICE_CAN_BE_DATA;
}

/*
 * xio_init_assertions()
 * xio_test_assertions() - check memory integrity of xio sub-systems
 */

void xio_init_assertions()
{
	xio.magic_start = MAGICNUM;
	xio.magic_end = MAGICNUM;
}

stat_t xio_test_assertions()
{
	if ((xio.magic_start != MAGICNUM) || (xio.magic_end != MAGICNUM)) return (STAT_XIO_ASSERTION_FAILURE);
	return (STAT_OK);
}


/*
 * xio_callback() - callback from main loop for various IO functions
 *
 *	The USB channel binding functionality is in here. If this gets too big or there are other
 *	things to do during the callback it may make sense to break this out into a separate function.
 *
 *	Channel binding state machine (simple - does not do multiple ctrl channels yet)
 *	(0)	No connection
 *	(1)	Single USB (CTRL_AND_DATA)
 *	(2) Dual USB (CTRL, DATA)
 *	(3) Force disconnect of DATA channel
 *
 *	Channel binding rules (start state --> end state)
 *	(0-->0) Initially all channels are disconnected. Channel types are TYPE_NONE following initialization
 *	(0-->1) One of the USB serial channels connects. It becomes type CTRL_AND_DATA
 *	(1-->2) The other channel connects (maybe). It becomes type DATA. The first becomes type CTRL
 *
 *	Channel unbinding rules
 *	(1-->0) CTRL_AND_DATA channel disconnects. No connection exists (what about a secondary CTRL channel?)
 *	(2-->1) DATA channel disconnects. The first CTRL channel reverts to being CTRL_AND_DATA
 *	(2-->3-->0) CTRL channel disconnects. If this is the last open CTRL channel all channels are 
 *		disconnected (including DATA). If it is not the last CTRL channel then only it disconnects.
 *
 *		(We might need some way to designate a particular control channel as being the primary, 
 *		 or first, rather than just by position in the list)
 *
 *	Note: We will not receive a second DEVICE_CONNECTED on a channel that has already received one,
 *		  and similarly for DEVICE_NOT_CONNECTED. IOW we will only get valid state transitions w/no repeats.
 */
stat_t xio_callback()
{
	// exit the callback if there is no new state information to process
	if ((usb0->next_state == DEVICE_IDLE) && (usb1->next_state == DEVICE_IDLE)) {
		return (STAT_OK);
	}
	
	// USB0 has just connected
	// Case 1: USB0 is the 1st channel to open
	// Case 2: USB0 is the 2nd channel to open
	if (usb0->next_state == DEVICE_CONNECTED) {	
		usb0->device_state = DEVICE_CONNECTED;
		usb0->next_state = DEVICE_IDLE;						// clear next state
		if (usb1->device_state == DEVICE_NOT_CONNECTED) {	// Case 1
			setCtrl(usb0);									// becomes ctrl and data
			setData(usb0);
			setPrimary(usb0);								//...and gets set as the primary ctrl channel
		} else {											// Case 2
			downData(usb1);									// shut down current data channel
			setData(usb0);									// change location
		}
		return (STAT_OK);
	}

	// USB1 has just connected
	// Case 1: USB1 is the 1st channel to open
	// Case 2: USB1 is the 2nd channel to open
	if (usb1->next_state == DEVICE_CONNECTED) {
		usb1->device_state = DEVICE_CONNECTED;
		usb1->next_state = DEVICE_IDLE;
		if (usb0->device_state == DEVICE_NOT_CONNECTED) {
			setCtrl(usb1);
			setData(usb1);
			setPrimary(usb1);
		} else {
			downData(usb0);
			setData(usb1);
		}
		return (STAT_OK);
	}

	// USB0 has just disconnected
	// Case 1: USB0 closed while it was a ctrl+data channel
	// Case 2: USB0 closed while it was the primary ctrl channel
	// Case 3: USB0 closed while it was a non-primary ctrl channel
	// Case 4: USB0 closed while it was a data channel
	if (usb0->next_state == DEVICE_NOT_CONNECTED) {
		usb0->device_state = DEVICE_NOT_CONNECTED;
		usb0->next_state = DEVICE_IDLE;

		// can be reorganized to be more efficient, but maybe the compiler will do it for us
		if ((isCtrl(usb0)) && (isData(usb0))) {		// Case 1
			clrCtrl(usb0);
			clrData(usb0);
		}
		if ((isCtrl(usb0)) && (isPrimary(usb0))) {	// Case 2
			clrCtrl(usb0);
			clrData(usb0);
		}
		if ((isCtrl(usb0)) && (!isPrimary(usb0))) {	// Case 3
			clrCtrl(usb0);
		}
		if (isData(usb0)) {							// Case 4
			downData(usb0);
		}
		return (STAT_OK);
	}

	// USB1 has just disconnected - same cases as USB0
	if (usb1->next_state == DEVICE_NOT_CONNECTED) {
		usb1->device_state = DEVICE_NOT_CONNECTED;
		usb1->next_state = DEVICE_IDLE;
		
		if ((isCtrl(usb1)) && (isData(usb1))) {		// Case 1
			clrCtrl(usb1);
			clrData(usb1);
		}
		if ((isCtrl(usb1)) && (isPrimary(usb1))) {	// Case 2
			clrCtrl(usb1);
			clrData(usb1);
		}
		if ((isCtrl(usb1)) && (!isPrimary(usb1))) {	// Case 3
			clrCtrl(usb1);
		}
		if (isData(usb1)) {							// Case 4
			downData(usb1);
		}
	}
	return (STAT_OK);
}

/*
 * read_char() - returns single char or -1 (_FDEV_ERR) is none available
 */
int read_char (uint8_t dev)
{
    return DeviceWrappers[dev]->readchar();
//    return DeviceWrappers[DEV_USB0]->readchar();
//    return SerialUSB1.readByte();
}

/*
 * readline() - read a complete line from stdin (NEW STYLE)
 *
 *	Reads a line of text from the next device that has one ready. With some exceptions.
 *	Accepts CR or LF as line terminator. Replaces CR or LF with NUL in the returned string.
 *
 *	This function reads from separate control and data devices, including reading from multiple
 *	control devices. It will also manage multiple data devices, but only one data device may
 *	be open at a time.
 *
 *	ARGS:
 *
 *	 type - Sets channel type to read. If TYPE_NONE will read either control or data.
 *			On return this variable is set to the channel type that was read, or
 *			TYPE_NONE if no text was returned.
 *
 *   size - Sets the maximum size of the read buffer. On return is set to the number of
 *			characters in the buffer, including the NUL termination. Set to zero if no text
 *			was returned. Will truncate a text line with a NUL if size is reached.
 *
 *	 char * Returns a pointer to the buffer containing the line, or FDEV_ERR (-1) if no text
 */
char *readline(uint8_t &type, int16_t &size)
{
	return ((char *)_FDEV_ERR);
}

/*
 * read_line() - read a complete line from stdin (OLD STYLE)
 *
 *	Accepts CR or LF as line terminator. Replaces CR or LF with NUL in the returned string.
 *
 *	Returns:
 *
 *	  STAT_OK		  Returns a complete null terminated string.
 *					  Index contains total character count (less terminating NUL)
 *					  The terminating LF is not written to the string.
 *
 *	  STAT_EAGAIN	  Line is incomplete because input has no more characters.
 *					  Index is left at the first available space.
 *					  Retry later to read more of the string. Use index from previous call.
 *
 *	  STAT_EOF		  Line is incomplete because end of file was reached (file devices)
 *					  Index can be used as a character count.
 *
 *	  STAT_BUFFER_FULL Incomplete because size was reached.
 *                    Index will equal size.
 *
 *	  STAT_FILE_SIZE_EXCEEDED returned if the starting index exceeds the size.
 *
 *	Note: uint8_t aka char_t but you might not have that typedef at this low a level
 */
stat_t read_line (uint8_t *buffer, uint16_t *index, size_t size)
{
	if (*index >= size) { return (STAT_FILE_SIZE_EXCEEDED);}

	for (int c; *index < size; (*index)++ ) {
		if ((c = read_char()) != _FDEV_ERR) {
			buffer[*index] = (uint8_t)c;
			if ((c == LF) || (c == CR)) {
				buffer[*index] = NUL;
				return (STAT_OK);
			}
			continue;
		}
		return (STAT_EAGAIN);
	}
	return (STAT_BUFFER_FULL);
}

size_t write(uint8_t *buffer, size_t size)
{
	size_t written = SerialUSB.write(buffer, size);
//    size_t written = SerialUSB1.write(buffer, size);
	return (written);
}

/***********************************************************************************
 * CONFIGURATION AND INTERFACE FUNCTIONS
 * Functions to get and set variables from the cfgArray table
 ***********************************************************************************/

/*
 * xio_set_spi() = 0=disable, 1=enable
 */
stat_t xio_set_spi(nvObj_t *nv)
{
	xio.spi_state = (uint8_t)nv->value;

#ifdef __ARM
	if (fp_EQ(nv->value, SPI_ENABLE)) {
		spi_miso_pin.setMode(kOutput);
		spi_mosi_pin.setMode(kOutput);
		spi_sck_pin.setMode(kOutput);

    } else if (fp_EQ(nv->value, SPI_DISABLE)) {
		spi_miso_pin.setMode(kInput);
		spi_mosi_pin.setMode(kInput);
		spi_sck_pin.setMode(kInput);
	}
#endif
	return (STAT_OK);
}

/***********************************************************************************
 * TEXT MODE SUPPORT
 * Functions to print variables from the cfgArray table
 ***********************************************************************************/

#ifdef __TEXT_MODE

static const char fmt_spi[] PROGMEM = "[spi] SPI state%20d [0=disabled,1=enabled]\n";
void xio_print_spi(nvObj_t *nv) { text_print_ui8(nv, fmt_spi);}

#endif // __TEXT_MODE
