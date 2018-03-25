/**
 * @file
 * @brief	HID Library - SHUT communication subdriver.
 * @copyright	@parblock
 * Copyright (C):
 * - 2006-2009 -- Arnaud Quette (<aquette.dev@gmail.com>)
 * - 2018      -- Daniele Pezzini (<hyouko@gmail.com>)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *		@endparblock
 */

#include "libshut.h"

#include "common.h"	/* for xstrdup(), upsdebug*() */
#include "hiddefs.h"
#include "serial.h"
/*	#include "str.h"	*/
#include "timehead.h"

/** @name Driver info
 * @{ *************************************************************************/

#define SHUT_DRIVER_NAME	"SHUT communication driver"			/**< @brief Name of this driver. */
#define SHUT_DRIVER_VERSION	"0.86"						/**< @brief Version of this driver. */

upsdrv_info_t	comm_upsdrv_info = {
	SHUT_DRIVER_NAME,
	SHUT_DRIVER_VERSION,
	NULL,
	0,
	{ NULL }
};

/** @} ************************************************************************/


/** @name USB stuff
 * (sync'd with --ripped from-- libusb 1.0)
 * @{ *************************************************************************/

/** @brief Descriptor types as defined by the USB specification. */
enum usb_descriptor_type {
	USB_DT_DEVICE				= 0x01,
	USB_DT_CONFIG				= 0x02,
	USB_DT_STRING				= 0x03,
	USB_DT_INTERFACE			= 0x04,
	USB_DT_ENDPOINT				= 0x05,
	USB_DT_BOS				= 0x0F,
	USB_DT_DEVICE_CAPABILITY		= 0x10,
	USB_DT_HID				= 0x21,
	USB_DT_REPORT				= 0x22,
	USB_DT_PHYSICAL				= 0x23,
	USB_DT_HUB				= 0x29,
	USB_DT_SUPERSPEED_HUB			= 0x2A,
	USB_DT_SS_ENDPOINT_COMPANION		= 0x30
};

/* Descriptor sizes per descriptor type. */
#define USB_DT_DEVICE_SIZE			18
#define USB_DT_CONFIG_SIZE			 9
#define USB_DT_INTERFACE_SIZE			 9
#define USB_DT_ENDPOINT_SIZE			 7
#define USB_DT_ENDPOINT_AUDIO_SIZE		 9
#define USB_DT_HUB_NONVAR_SIZE			 7
#define USB_DT_SS_ENDPOINT_COMPANION_SIZE	 6
#define USB_DT_BOS_SIZE				 5
#define USB_DT_DEVICE_CAPABILITY_SIZE		 3

/** @brief Endpoint direction. */
enum usb_endpoint_direction {
	USB_ENDPOINT_IN				= 0x80,
	USB_ENDPOINT_OUT			= 0x00
};

/** @brief Mask to get the endpoint direction from a value. */
#define USB_ENDPOINT_DIR_MASK			0x80

/** @brief Standard requests, as defined in table 9-5 of the USB 3.0 specifications. */
enum usb_standard_request {
	USB_REQUEST_GET_STATUS			= 0x00,
	USB_REQUEST_CLEAR_FEATURE		= 0x01,
						/* 0x02 is reserved */
	USB_REQUEST_SET_FEATURE			= 0x03,
						/* 0x04 is reserved */
	USB_REQUEST_SET_ADDRESS			= 0x05,
	USB_REQUEST_GET_DESCRIPTOR		= 0x06,
	USB_REQUEST_SET_DESCRIPTOR		= 0x07,
	USB_REQUEST_GET_CONFIGURATION		= 0x08,
	USB_REQUEST_SET_CONFIGURATION		= 0x09,
	USB_REQUEST_GET_INTERFACE		= 0x0A,
	USB_REQUEST_SET_INTERFACE		= 0x0B,
	USB_REQUEST_SYNCH_FRAME			= 0x0C,
	USB_REQUEST_SET_SEL			= 0x30,
	USB_SET_ISOCH_DELAY			= 0x31
};

/** @brief Request type bits of the "bmRequestType" field in control transfers. */
enum usb_request_type {
	USB_REQUEST_TYPE_STANDARD		= (0x00 << 5),
	USB_REQUEST_TYPE_CLASS			= (0x01 << 5),
	USB_REQUEST_TYPE_VENDOR			= (0x02 << 5),
	USB_REQUEST_TYPE_RESERVED		= (0x03 << 5)
};

/** @brief Recipient bits of the "bmRequestType" field in control transfers. */
enum usb_request_recipient {
	USB_RECIPIENT_DEVICE			= 0x00,
	USB_RECIPIENT_INTERFACE			= 0x01,
	USB_RECIPIENT_ENDPOINT			= 0x02,
	USB_RECIPIENT_OTHER			= 0x03
};

/** @brief Structure/type representing the standard USB device descriptor. */
typedef struct usb_device_descriptor_s {
	uint8_t					bLength;
	uint8_t					bDescriptorType;
	uint16_t				bcdUSB;
	uint8_t					bDeviceClass;
	uint8_t					bDeviceSubClass;
	uint8_t					bDeviceProtocol;
	uint8_t					bMaxPacketSize0;
	uint16_t				idVendor;
	uint16_t				idProduct;
	uint16_t				bcdDevice;
	uint8_t					iManufacturer;
	uint8_t					iProduct;
	uint8_t					iSerialNumber;
	uint8_t					bNumConfigurations;
} usb_device_descriptor_t;

/** @brief Structure/type representing the setup packet for control transfers. */
typedef struct usb_control_setup_s {
	uint8_t					bmRequestType;
	uint8_t					bRequest;
	uint16_t				wValue;
	uint16_t				wIndex;
	uint16_t				wLength;
} usb_control_setup_t;

/** @brief Size of the setup packet for control transfers. */
#define USB_CONTROL_SETUP_SIZE			(sizeof(usb_control_setup_t))

/** @} ************************************************************************/


/** @name SHUT stuff
 * (mostly from 'Simplified SHUT' specs)
 * @{ *************************************************************************/

/** @brief Packet type.
 *
 * Communication with a SHUT device is performed with two kinds of packets:
 * - single-byte 'tokens' ('SB', here),
 * - multi-byte data packets ('MB', here), using a structure represented by a @ref shut_packet_t.
 *
 * @note 'Sync signals' (tokens) are also used to set the @ref shut_notification_level "notification level". */
enum shut_pkt_type {
	/* Data packets */
	SHUT_PKT_MB_REQUEST			= 0x01,				/**< Data packet: request (from host to device). */
	SHUT_PKT_MB_RESPONSE			= 0x04,				/**< Data packet: response (from device to host). */
	SHUT_PKT_MB_NOTIFY			= 0x05,				/**< Data packet: notification (unsolicited transmission from device to host). */

	/* Tokens for handling errors */
	SHUT_PKT_SB_ACK				= 0x06,				/**< Acknowledgement token: packet received OK, no errors. */
	SHUT_PKT_SB_NAK				= 0x15,				/**< Acknowledgement token: error detected in packet. */

	/* Tokens for controlling synchronisation/notification */
	SHUT_PKT_SB_SYNC			= 0x16,				/**< Sync token: complete notifications. */
	SHUT_PKT_SB_SYNC_LIGHT			= 0x17,				/**< Sync token: partial notifications. */
	SHUT_PKT_SB_SYNC_OFF			= 0x18				/**< Sync token: disable notifications -- only do polling. */
};

#define SHUT_PKT_MB_LAST_MASK			0x80				/**< @brief End of transmission mask (added to one of the MB @ref shut_pkt_type's). */

#define SHUT_PKT_MAX_DATA_SIZE			USB_CONTROL_SETUP_SIZE		/**< @brief Maximum amount of data that a SHUT data packet can carry. */

/** @brief Structure/type representing a SHUT data packet. */
typedef struct shut_packet_s {
	uint8_t					bType;				/**< @brief Packet type (see @ref shut_pkt_type). */
	uint8_t					bLength;			/**< @brief Data length. */
	uint8_t					data[SHUT_PKT_MAX_DATA_SIZE];	/**< @brief Data bytes. */
	uint8_t					bChecksum;			/**< @brief Checksum. */
} shut_packet_t;

/** @brief Maximum size of a SHUT data packet. */
#define SHUT_PKT_MAX_PKT_SIZE			(sizeof(shut_packet_t))

/** @brief Size of a SHUT data packet's non-data fields. */
#define SHUT_PKT_GUTS_SIZE			(SHUT_PKT_MAX_PKT_SIZE - SHUT_PKT_MAX_DATA_SIZE)

/** @brief Structure/type for holding data received from a device. */
typedef struct shut_data_s {
	uint8_t					 type;				/**< @brief Type of data received (see @ref shut_pkt_type). */
	size_t					 length;			/**< @brief Data length. */
	unsigned char				*data;				/**< @brief Data bytes. */
} shut_data_t;

/** @brief Notification levels.
 * @note These are not processed currently. */
enum shut_notification_level {
	SHUT_NOTIFICATION_OFF			= 1,				/**< Notification off */
	SHUT_NOTIFICATION_LIGHT			= 2,				/**< Light notification */
	SHUT_NOTIFICATION_COMPLETE		= 3,				/**< Complete notification for devices which don't support disabling it
										 * (like some early Ellipse models). */
	SHUT_NOTIFICATION_DEFAULT		= SHUT_NOTIFICATION_COMPLETE	/**< Default notification level. */
};

#define SHUT_MAX_TRIES				   4				/**< @brief Maximum number of attempts before raising a comm fault. */
#define SHUT_TIMEOUT				3000				/**< @brief Default timeout (ms) for SHUT I/O operations. */
#define SHUT_MAX_STRING_SIZE			 128				/**< @brief Maximum string length... */
#define SHUT_INTERFACE_NUMBER			   0				/**< @brief SHUT (USB) interface number. */

/** @brief Debug levels for upsdebugx()/upsdebug_hex() calls.
 * @todo Actually make sense out of these and/or standardise the use of debug levels in all the tree
 * (see also https://github.com/networkupstools/nut/issues/211). */
enum shut_debug_level {
	SHUT_DBG_DEVICE				=  2,				/**< Any debuggingly useful info about a device.
										 * - > "my device is not recognised by the driver."
										 * - < "i feel for you... let's see..." */
	SHUT_DBG_SHUT				=  3,				/**< Debug SHUT routines.
										 * - < "hmm... why can't we get that piece of info there or that command done?" */
	SHUT_DBG_IO				=  4,				/**< Debug I/O (and related) operations.
										 * - < "ah! it's an I/O problem, that's why! -- but what's going on exactly?" */
	SHUT_DBG_DEEP				=  5,				/**< Debug low level (e.g. bytes) things.
										 * - < "oh wait, maybe it's just that we're sending the wrong thing! let's see it unadorned..." */
	SHUT_DBG_FUNCTION_CALLS			= 10				/**< Debug function calls (arguments).
										 * - > "no, more simply, your driver is badly broken. full stop."
										 * - < [crying] "well. beep-" */
};

/** @} ************************************************************************/


/** @name Low level SHUT routines
 * @{ *************************************************************************/

static int		 shut_error_from_errno(int errcode);
static const char	*shut_strerror(enum shut_error errcode);
static const char	*shut_strpackettype(uint8_t type);

static void		 shut_align_request(usb_control_setup_t *ctrl);
static unsigned char	 shut_checksum(const unsigned char *data, size_t datasize);

static void		 shut_setline(int fd, int reverse);
static int		 shut_synchronise(int fd);

static int		 shut_send_one(int fd, unsigned char byte);
static int		 shut_send_more(int fd, const unsigned char *bytes, size_t size);
static int		 shut_receive_data(int fd, shut_data_t *data);

static int		 shut_control_transfer(
	int		 fd,
	uint8_t		 bmRequestType,
	uint8_t		 bRequest,
	uint16_t	 wValue,
	uint16_t	 wIndex,
	unsigned char	*data,
	uint16_t	 wLength,
	unsigned int	 timeout
);
static int		 shut_interrupt_transfer(
	int		 fd,
	unsigned char	 endpoint,
	unsigned char	*data,
	int		 length,
	int		*transferred,
	unsigned int	 timeout
);
static int		 shut_get_string_descriptor_ascii(
	int		 fd,
	uint8_t		 desc_index,
	unsigned char	*data,
	int		 length
);

/** @brief Morph an error condition into a @ref shut_error "SHUT_ERROR" code.
 * @note A value of zero for *errcode* is considered akin to a timeout.
 * @return *errcode*, when it is positive (i.e. no errors),
 * @return @ref SHUT_ERROR_TIMEOUT, if *errcode* is zero,
 * @return a @ref shut_error "SHUT_ERROR" code mapping the error code. */
static int	shut_error_from_errno(
	int	errcode	/**< [in] the negative value (`-errno`) or 0, that has to be mapped to a @ref shut_error "SHUT_ERROR" code */
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d)", __func__, errcode);

	if (errcode > 0)
		return errcode;

	switch (-errcode)
	{
	case EIO:
		return SHUT_ERROR_IO;
	case EINVAL:
		return SHUT_ERROR_INVALID_PARAM;
	case EACCES:
	case EPERM:
		return SHUT_ERROR_ACCESS;
	case ENODEV:
	case ENXIO:
		return SHUT_ERROR_NO_DEVICE;
	case ENOENT:
		return SHUT_ERROR_NOT_FOUND;
	case EBUSY:
		return SHUT_ERROR_BUSY;
	case 0:
	case ETIMEDOUT:
		return SHUT_ERROR_TIMEOUT;
	case EOVERFLOW:
		return SHUT_ERROR_OVERFLOW;
	case EPIPE:
		return SHUT_ERROR_PIPE;
	case EINTR:
		return SHUT_ERROR_INTERRUPTED;
	case ENOMEM:
		return SHUT_ERROR_NO_MEM;
	case ENOSYS:
		return SHUT_ERROR_NOT_SUPPORTED;
	default:
		return SHUT_ERROR_OTHER;
	}
}

/** @brief Return a constant string with a short description of *errcode*.
 * @return a short description of *errcode*. */
static const char	*shut_strerror(
	enum shut_error	errcode	/**< [in] the @ref shut_error code whose description is desired */
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d)", __func__, errcode);

	switch (errcode)
	{
	case SHUT_SUCCESS:
		return "Success";
	case SHUT_ERROR_IO:
		return "Input/Output Error";
	case SHUT_ERROR_INVALID_PARAM:
		return "Invalid parameter";
	case SHUT_ERROR_ACCESS:
		return "Access denied (insufficient permissions)";
	case SHUT_ERROR_NO_DEVICE:
		return "No such device (it may have been disconnected)";
	case SHUT_ERROR_NOT_FOUND:
		return "Entity not found";
	case SHUT_ERROR_BUSY:
		return "Resource busy";
	case SHUT_ERROR_TIMEOUT:
		return "Operation timed out";
	case SHUT_ERROR_OVERFLOW:
		return "Overflow";
	case SHUT_ERROR_PIPE:
		return "Pipe error";
	case SHUT_ERROR_INTERRUPTED:
		return "System call interrupted (perhaps due to signal)";
	case SHUT_ERROR_NO_MEM:
		return "Insufficient memory";
	case SHUT_ERROR_NOT_SUPPORTED:
		return "Operation not supported or unimplemented on this platform";
	case SHUT_ERROR_OTHER:
		return "Other error";
	default:
		return "Unknown error";
	}
}

/** @brief Return a constant string with a short description of *type*.
 * @return a short description of *type*, if it is a known type of @ref shut_pkt_type "packet",
 * @return *type* formatted in hex, for unknown types. */
static const char	*shut_strpackettype(
	uint8_t	type	/**< [in] a packet type (see @ref shut_pkt_type) */
) {
	static char	typestr[(sizeof(type) * 2) + 1] = "";

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%x)", __func__, type);

	switch (type)
	{
	/* Single byte packets (tokens) */
	case SHUT_PKT_SB_ACK:
		return "ACK token";
	case SHUT_PKT_SB_NAK:
		return "NAK token";
	case SHUT_PKT_SB_SYNC:
		return "SYNC token";
	case SHUT_PKT_SB_SYNC_LIGHT:
		return "SYNC_LIGHT token";
	case SHUT_PKT_SB_SYNC_OFF:
		return "SYNC_OFF token";
	/* Multi-byte packets */
	case SHUT_PKT_MB_REQUEST:
		return "REQUEST packet";
	case SHUT_PKT_MB_REQUEST | SHUT_PKT_MB_LAST_MASK:
		return "REQUEST/LAST packet";
	case SHUT_PKT_MB_RESPONSE:
		return "RESPONSE packet";
	case SHUT_PKT_MB_RESPONSE | SHUT_PKT_MB_LAST_MASK:
		return "RESPONSE/LAST packet";
	case SHUT_PKT_MB_NOTIFY:
		return "NOTIFY packet";
	case SHUT_PKT_MB_NOTIFY | SHUT_PKT_MB_LAST_MASK:
		return "NOTIFY/LAST packet";
	}

	/* Unknown types */
	snprintf(typestr, sizeof(typestr), "%0*x", (int)sizeof(typestr) - 1, type);
	return typestr;
}

/** @brief Swap two adjacent bytes.
 * @param[in] data	2-byte data whose bytes have to get their positions swapped
 * @return *data* 'swapped'. */
#define SHUT_BYTESWAP(data)	(((data & 0xFF) << 8) | ((data & 0xFF00) >> 8))

/** @brief Realign *ctrl* according to endianness. */
static void	shut_align_request(
	usb_control_setup_t	*ctrl	/**< [in,out] control setup packet that has to be realigned */
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%p)", __func__, (void *)ctrl);
#if WORDS_BIGENDIAN
	/* Sparc/Mips/... are big endian, USB/SHUT little endian */
	(*ctrl).wValue	= SHUT_BYTESWAP((*ctrl).wValue);
	(*ctrl).wIndex	= SHUT_BYTESWAP((*ctrl).wIndex);
	(*ctrl).wLength	= SHUT_BYTESWAP((*ctrl).wLength);
#endif	/* WORDS_BIGENDIAN */
}

/** @brief Compute a SHUT checksum for *data*.
 * @return the computed checksum. */
static unsigned char	shut_checksum(
	const unsigned char	*data,		/**< [in] data whose checksum has to be computed */
	size_t			 datasize	/**< [in] size of *data* */
) {
	size_t		i;
	unsigned char	chk = 0;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%p, %lu)", __func__, data, datasize);

	for (i = 0; i < datasize; i++)
		chk ^= data[i];

	upsdebugx(SHUT_DBG_DEEP, "%s: %02x", __func__, chk);
	return chk;
}

/** @brief Set serial control lines (clear DTR and set RTS, by default, or viceversa). */
static void	shut_setline(
	int	fd,	/**< [in] file descriptor of an already opened device */
	int	reverse	/**< [in] whether (non-zero) or not (0), the default behaviour has to be reversed (i.e. set DTR, clear RTS) */
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %d)", __func__, fd, reverse);

	if (fd < 1)
		return;

	if (reverse) {
		ser_set_dtr(fd, 1);
		ser_set_rts(fd, 0);
	} else {
		ser_set_dtr(fd, 0);
		ser_set_rts(fd, 1);
	}
}

/** @brief Initiate/Restore communication with a device, while setting a @ref shut_notification_level.
 * @note At the moment, notification is always set to @ref SHUT_NOTIFICATION_OFF.
 * @todo Re-enable notification support.
 * @todo Process any notification unexpectedly received while attempting a synchronisation.
 * @return @ref SHUT_SUCCESS, on success,
 * @return @ref SHUT_ERROR_TIMEOUT, if no errors occurred while communicating with the device but synchronisation could not be performed in @ref SHUT_MAX_TRIES attempts,
 * @return @ref SHUT_ERROR_NO_MEM, on memory allocation errors,
 * @return a @ref shut_error "SHUT_ERROR" code, on other errors. */
static int	shut_synchronise(
	int	fd	/**< [in] file descriptor of an already opened device */
) {
	int		ret,
			try,
			notification_level = SHUT_NOTIFICATION_OFF;
#if 0
	static int	notification_level = SHUT_NOTIFICATION_DEFAULT,
			user_notification_level = 0;
#endif
	uint8_t		sync_type;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d)", __func__, fd);

#if 0
	/* Get user-provided value for notification level, if not already done */
	if (!user_notification_level) {
		const char	*notification_string = getval("notification");

		if (notification_string) {
			if (!str_to_int(notification_string, &user_notification_level, 10))
				upslogx(LOG_WARNING, "%s: could not convert to an int the provided value (%s) for 'notification' (%s).", __func__, notification_string, strerror(errno));
			else if (user_notification_level < SHUT_NOTIFICATION_OFF || user_notification_level > SHUT_NOTIFICATION_COMPLETE)
				upslogx(LOG_WARNING, "%s: invalid value (%d) for 'notification'.", __func__, user_notification_level);
			else
				notification_level = user_notification_level;
		}

		/* Done, don't do this again */
		user_notification_level = notification_level;
	}
#endif

	switch (notification_level)
	{
	case SHUT_NOTIFICATION_OFF:
		sync_type = SHUT_PKT_SB_SYNC_OFF;
		break;
	case SHUT_NOTIFICATION_LIGHT:
		sync_type = SHUT_PKT_SB_SYNC_LIGHT;
		break;
	case SHUT_NOTIFICATION_COMPLETE:
	default:
		sync_type = SHUT_PKT_SB_SYNC;
		break;
	}

	/* Sync with device according to notification */
	upsdebugx(SHUT_DBG_SHUT, "%s: sync'ing communication with a %s.", __func__, shut_strpackettype(sync_type));
	for (try = 1; try <= SHUT_MAX_TRIES; try++) {
		shut_data_t	shut_data = { 0 };

		upsdebugx(SHUT_DBG_SHUT, "%s: sync'ing communication (try #%d/%d).", __func__, try, SHUT_MAX_TRIES);

		if ((ret = shut_send_one(fd, sync_type)) != SHUT_SUCCESS) {
			upsdebugx(SHUT_DBG_SHUT, "%s: error while sending packet to the device (%s).", __func__, shut_strerror(ret));
			continue;
		}

		if ((ret = shut_receive_data(fd, &shut_data)) != SHUT_SUCCESS) {
			upsdebugx(SHUT_DBG_SHUT, "%s: error while receiving packet from the device (%s).", __func__, shut_strerror(ret));
			if (ret == SHUT_ERROR_NO_MEM)
				return ret;
			continue;
		}

		if (shut_data.type == sync_type) {
			upsdebugx(SHUT_DBG_SHUT, "%s: sync'ing and notification setting done.", __func__);
			/* There should be no data, but still... */
			free(shut_data.data);
			return SHUT_SUCCESS;
		}

		/* TODO: process any notification */
		upsdebugx(SHUT_DBG_SHUT, "%s: unexpectedly got a %s from the device.", __func__, shut_strpackettype(shut_data.type));
		free(shut_data.data);
	}

	upsdebugx(SHUT_DBG_SHUT, "%s: could not perform sync'ing and notification setting.", __func__);
	if (ret != SHUT_SUCCESS)
		return ret;
	return SHUT_ERROR_TIMEOUT;
}

/** @brief Send one byte of data to a device.
 * @return @ref SHUT_SUCCESS, on success,
 * @return a @ref shut_error "SHUT_ERROR" code, on errors. */
static int	shut_send_one(
	int		fd,	/**< [in] file descriptor of an already opened device */
	unsigned char	byte	/**< [in] data that has to be sent */
) {
	int	ret;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %x)", __func__, fd, byte);

	if ((ret = ser_send_char(fd, byte)) <= 0) {
		upsdebugx(SHUT_DBG_IO, "%s: could not send data to the device (%s).", __func__, ret ? strerror(errno) : "timeout");
		return shut_error_from_errno(errno);
	}

	return SHUT_SUCCESS;
}

/** @brief Send one or more bytes of data to a device.
 * @return @ref SHUT_SUCCESS, on success,
 * @return a @ref shut_error "SHUT_ERROR" code, on errors. */
static int	shut_send_more(
	int			 fd,	/**< [in] file descriptor of an already opened device */
	const unsigned char	*bytes,	/**< [in] data that has to be sent */
	size_t			 size	/**< [in] number of bytes of *bytes* that have to be sent (*bytes* should be at least this size) */
) {
	int	ret;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %p, %lu)", __func__, fd, bytes, size);

	if ((ret = ser_send_buf(fd, bytes, size)) <= 0) {
		upsdebugx(SHUT_DBG_IO, "%s: could not send data to the device (%s).", __func__, ret ? strerror(errno) : "timeout");
		return shut_error_from_errno(errno);
	}

	return SHUT_SUCCESS;
}

/** @brief Receive data from a device and store it in *data*.
 * @return @ref SHUT_SUCCESS with *data* filled (don't forget to free() *data*'s shut_data_t::data, when done with it), on success,
 * @return @ref SHUT_ERROR_TIMEOUT, if data could not be received in @ref SHUT_MAX_TRIES attempts,
 * @return @ref SHUT_ERROR_NO_MEM, on memory allocation errors,
 * @return a @ref shut_error "SHUT_ERROR" code, on other errors. */
static int	shut_receive_data(
	int		 fd,	/**< [in] file descriptor of an already opened device */
	shut_data_t	*data	/**< [out] storage location for the retrieved data */
) {
	shut_data_t	buf = { 0 };
	int		try = 1;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %p)", __func__, fd, (void *)data);

	while (try <= SHUT_MAX_TRIES) {
		shut_packet_t	packet = { 0 };
		size_t		received, size;
		int		ret;

		/* Packet type */
		if ((ret = ser_get_char(fd, &packet.bType, SHUT_TIMEOUT / 1000, (SHUT_TIMEOUT % 1000) * 1000)) <= 0) {
			upsdebugx(SHUT_DBG_IO, "%s: could not get packet type (%s).", __func__, ret ? strerror(errno) : "timeout");
			goto comm_error;
		}
		/* Something unexpected while receiving multi-packet data */
		if (buf.type && (buf.type != (packet.bType & ~SHUT_PKT_MB_LAST_MASK))) {
			upsdebugx(SHUT_DBG_IO, "%s: packets mismatch (receiving %s).", __func__, shut_strpackettype(packet.bType));
			goto packet_error;
		}
		switch (packet.bType)
		{
		/* Single byte packets (tokens) */
		case SHUT_PKT_SB_SYNC:
		case SHUT_PKT_SB_SYNC_LIGHT:
		case SHUT_PKT_SB_SYNC_OFF:
		case SHUT_PKT_SB_ACK:
		case SHUT_PKT_SB_NAK:
			upsdebugx(SHUT_DBG_IO, "%s: received %s.", __func__, shut_strpackettype(packet.bType));
			goto return_data;
		/* Multi-byte packets */
		case SHUT_PKT_MB_NOTIFY:
		case SHUT_PKT_MB_NOTIFY | SHUT_PKT_MB_LAST_MASK:
		case SHUT_PKT_MB_RESPONSE:
		case SHUT_PKT_MB_RESPONSE | SHUT_PKT_MB_LAST_MASK:
			upsdebugx(SHUT_DBG_IO, "%s: receiving %s.", __func__, shut_strpackettype(packet.bType));
			break;
		default:
			upsdebugx(SHUT_DBG_IO, "%s: unexpected/unknown token/type of packet received (%s).", __func__, shut_strpackettype(packet.bType));
			goto packet_error;
		}

		/* Data length */
		if ((ret = ser_get_char(fd, &packet.bLength, SHUT_TIMEOUT / 1000, (SHUT_TIMEOUT % 1000) * 1000)) <= 0) {
			upsdebugx(SHUT_DBG_IO, "%s: could not get data length (%s).", __func__, ret ? strerror(errno) : "timeout");
			goto comm_error;
		}
		size = packet.bLength & 0x0F;
		if (size != (packet.bLength >> 4)) {
			upsdebugx(SHUT_DBG_IO, "%s: got an invalid length (%x).", __func__, packet.bLength);
			goto packet_error;
		}
		if (size > sizeof(packet.data)) {
			upsdebugx(SHUT_DBG_IO, "%s: invalid frame size (%lu).", __func__, size);
			goto packet_error;
		}
		/* highly unlikely... */
		if (buf.length > SIZE_MAX - size) {
			upsdebugx(SHUT_DBG_IO, "%s: receiving data would cause an overflow (%lu + %lu > %lu).", __func__, buf.length, size, SIZE_MAX);
			free(buf.data);
			return SHUT_ERROR_OVERFLOW;
		}

		/* Data */
		for (received = 0; received < size; received++) {
			if ((ret = ser_get_char(fd, &packet.data[received], SHUT_TIMEOUT / 1000, (SHUT_TIMEOUT % 1000) * 1000)) > 0)
				continue;
			upsdebugx(SHUT_DBG_IO, "%s: could not retrieve data, only got %lu bytes out of the %lu expected (%s).", __func__, received, size, ret ? strerror(errno) : "timeout");
			if (received)
				upsdebug_hex(SHUT_DBG_DEEP, "received data", packet.data, received);
			goto comm_error;
		}
		upsdebug_hex(SHUT_DBG_DEEP, "received data", packet.data, size);

		/* Checksum */
		if ((ret = ser_get_char(fd, &packet.bChecksum, SHUT_TIMEOUT / 1000, (SHUT_TIMEOUT % 1000) * 1000)) <= 0) {
			upsdebugx(SHUT_DBG_IO, "%s: could not retrieve checksum (%s).", __func__, ret ? strerror(errno) : "timeout");
			goto comm_error;
		}
		if (packet.bChecksum != shut_checksum(packet.data, size)) {
			upsdebugx(SHUT_DBG_IO, "%s: failed to validate checksum (%02x).", __func__, packet.bChecksum);
			goto packet_error;
		}

		/* Store our precious data */
		buf.data = realloc(buf.data, buf.length + size);
		if (buf.data == NULL)
			return SHUT_ERROR_NO_MEM;
		memcpy(&buf.data[buf.length], packet.data, size);
		buf.length += size;

		/* Ack the device about the received data */
		if ((ret = ser_send_char(fd, SHUT_PKT_SB_ACK)) <= 0) {
			upsdebugx(SHUT_DBG_IO, "%s: could not send ACK to the device (%s).", __func__, ret ? strerror(errno) : "timeout");
			goto comm_error;
		}

		/* All is well! Reset our failure counter */
		try = 1;

		/* Check if there is more data to be received */
		if (packet.bType & SHUT_PKT_MB_LAST_MASK) {
			upsdebugx(SHUT_DBG_IO, "%s: expecting more data to follow.", __func__);
			buf.type = packet.bType;
			continue;
		}

	return_data:
		data->type = packet.bType & ~SHUT_PKT_MB_LAST_MASK;
		data->length = buf.length;
		data->data = buf.data;
		return SHUT_SUCCESS;

	packet_error:
		try++;
		/* Send a NAK to get a resend from the device */
		if ((ret = ser_send_char(fd, SHUT_PKT_SB_NAK) > 0))
			continue;
		upsdebugx(SHUT_DBG_IO, "%s: could not send NAK to the device (%s).", __func__, ret ? strerror(errno) : "timeout");
	comm_error:
		free(buf.data);
		return shut_error_from_errno(errno);
	}

	upsdebugx(SHUT_DBG_IO, "%s: failed to retrieve data from the device.", __func__);
	free(buf.data);
	return SHUT_ERROR_TIMEOUT;
}

/** @brief Take care of SHUT control transfers.
 *
 * The direction of the transfer is inferred from the *bmRequestType* field of the setup packet.
 *
 * @todo Process any notification unexpectedly received while waiting for an ACK after sending data to the device.
 *
 * @return the number of bytes actually transferred, on success,
 * @return @ref SHUT_ERROR_TIMEOUT, if the transfer could not be performed in the given *timeout* or in @ref SHUT_MAX_TRIES attempts,
 * @return @ref SHUT_ERROR_NO_MEM, on memory allocation errors,
 * @return @ref SHUT_ERROR_OVERFLOW, if the received data exceeds *data*'s size (*wLength*),
 * @return a @ref shut_error "SHUT_ERROR" code, on other errors. */
static int	shut_control_transfer(
	int		 fd,		/**< [in] file descriptor of an already opened device */
	uint8_t		 bmRequestType,	/**< [in] request type field for the setup packet */
	uint8_t		 bRequest,	/**< [in] request field for the setup packet */
	uint16_t	 wValue,	/**< [in] value field for the setup packet */
	uint16_t	 wIndex,	/**< [in] index field for the setup packet */
	unsigned char	*data,		/**< [in,out] storage location for input or output data */
	uint16_t	 wLength,	/**< [in] length field for the setup packet (*data* should be at least this size) */
	unsigned int	 timeout	/**< [in] allowed timeout (ms) for the operation (for an unlimited timeout, use value 0) */
) {
	int		try = 1;
	struct timeval	exp;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %x, %x, %x, %u, %p, %u, %u)", __func__, fd, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout);

	if (timeout) {
		gettimeofday(&exp, NULL);
		exp.tv_sec += timeout / 1000;
		exp.tv_usec += (timeout % 1000) * 1000;
		while (exp.tv_usec >= 1000000) {
			exp.tv_usec -= 1000000;
			exp.tv_sec++;
		}
	}

	while (try <= SHUT_MAX_TRIES) {

		size_t	remaining_size,
			sent_data_size = 0;
		int	packet = 1,
			ret,
			read_try,
			write_try = 1;

		if (timeout) {
			struct timeval	now;

			gettimeofday(&now, NULL);
			if (
				exp.tv_sec > now.tv_sec ||
				(exp.tv_sec == now.tv_sec && exp.tv_usec > now.tv_usec)
			) {
				upsdebugx(SHUT_DBG_SHUT, "%s: could not perform the requested transfer in the allowed time.", __func__);
				return SHUT_ERROR_TIMEOUT;
			}
		}

		/* Send request/all data */
		do {

			unsigned char		 shut_pkt[SHUT_PKT_MAX_PKT_SIZE],
						*pkt_data;
			size_t			 pkt_data_size,
						 shut_pkt_size;
			usb_control_setup_t	 ctrl;

			if (packet == 1) {
				/* Build the control request */
				ctrl.bmRequestType	= bmRequestType;
				ctrl.bRequest		= bRequest;
				ctrl.wValue		= wValue;
				ctrl.wIndex		= wIndex;
				ctrl.wLength		= wLength;

				shut_align_request(&ctrl);
				pkt_data = (unsigned char *)&ctrl;

				pkt_data_size = sizeof(ctrl);

				/* For SET requests, this is an additional initial SHUT packet for the request,
				 * while data will follow in subsequent packet(s) */
				if ((bmRequestType & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT)
					remaining_size = wLength + pkt_data_size;
				/* For GET requests, we don't have to send any other data */
				else
					remaining_size = pkt_data_size;
			} else {
				pkt_data_size = remaining_size < SHUT_PKT_MAX_DATA_SIZE ? remaining_size : SHUT_PKT_MAX_DATA_SIZE;
				pkt_data = data + sent_data_size;
				upsdebug_hex(SHUT_DBG_DEEP, "data to be sent", pkt_data, pkt_data_size);
			}
			shut_pkt_size = pkt_data_size + SHUT_PKT_GUTS_SIZE;

			/* Forge the SHUT frame */
			shut_pkt[0] = SHUT_PKT_MB_REQUEST | (remaining_size > SHUT_PKT_MAX_DATA_SIZE ? 0 : SHUT_PKT_MB_LAST_MASK);
			shut_pkt[1] = (pkt_data_size << 4) | pkt_data_size;
			memcpy(&shut_pkt[2], pkt_data, pkt_data_size);
			shut_pkt[shut_pkt_size - 1] = shut_checksum(&shut_pkt[2], pkt_data_size);
			upsdebug_hex(SHUT_DBG_DEEP, "SHUT packet to be sent", shut_pkt, shut_pkt_size);

			/* Send packet. */
			if ((ret = shut_send_more(fd, shut_pkt, shut_pkt_size)) != SHUT_SUCCESS) {
				upsdebugx(SHUT_DBG_SHUT, "%s: could not send packet to the device (%s).", __func__, shut_strerror(ret));
				return ret;
			}

			/* Get reply */
			for (read_try = 1; read_try <= SHUT_MAX_TRIES; read_try++) {
				shut_data_t	shut_data = { 0 };

				if ((ret = shut_receive_data(fd, &shut_data)) != SHUT_SUCCESS) {
					upsdebugx(SHUT_DBG_SHUT, "%s: could not receive packet from the device (%s).", __func__, shut_strerror(ret));
					return ret;
				}

				switch (shut_data.type)
				{
				case SHUT_PKT_SB_ACK:
					remaining_size -= pkt_data_size;
					if (packet != 1)
						sent_data_size += pkt_data_size;
					/* There should be no data, but still... */
					free(shut_data.data);
					goto send_next_packet;
				case SHUT_PKT_SB_NAK:
					/* Resend this packet, if device requests so */
					upsdebugx(SHUT_DBG_SHUT, "%s: device has requested a resend.", __func__);
					/* There should be no data, but still... */
					free(shut_data.data);
					goto send_retry;
				case SHUT_PKT_MB_NOTIFY:
					upsdebugx(SHUT_DBG_SHUT, "%s: unexpected notification from the device.", __func__);
					/* TODO: process any notification */
					free(shut_data.data);
					goto resync_and_restart;
				default:
					upsdebugx(SHUT_DBG_SHUT, "%s: unexpected reply from the device (type: %s).", __func__, shut_strpackettype(shut_data.type));
					free(shut_data.data);
					/* At this point, it's likely a misread mistaken for a SB packet (token): try sending a NAK to get a resend from the device */
					if ((ret = shut_send_one(fd, SHUT_PKT_SB_NAK)) == SHUT_SUCCESS)
						continue;
					upsdebugx(SHUT_DBG_SHUT, "%s: could not send NAK to the device (%s).", __func__, shut_strerror(ret));
					return ret;
				}
			}
			upsdebugx(SHUT_DBG_SHUT, "%s: could not get an ACK from the device.", __func__);
			goto resync_and_restart;

		send_retry:
			write_try++;
			continue;

		send_next_packet:
			write_try = 1;
			packet++;

		} while (remaining_size > 0 && write_try <= SHUT_MAX_TRIES);

		if (remaining_size) {
			upsdebugx(SHUT_DBG_SHUT, "%s: failed to send request to the device.", __func__);
			return SHUT_ERROR_TIMEOUT;
		}

		/* Done for SET requests: return the length of the provided data sent to the device */
		if ((bmRequestType & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT)
			return wLength;

		/* Now receive data */
		memset(data, 0, wLength);
		for (read_try = 1; read_try <= SHUT_MAX_TRIES; read_try++) {

			shut_data_t	shut_data = { 0 };

			if ((ret = shut_receive_data(fd, &shut_data)) != SHUT_SUCCESS) {
				upsdebugx(SHUT_DBG_SHUT, "%s: could not receive packet from the device (%s).", __func__, shut_strerror(ret));
				return ret;
			}

			if (shut_data.type == SHUT_PKT_MB_RESPONSE) {
				if (shut_data.length > wLength) {
					upsdebugx(SHUT_DBG_SHUT, "%s: received data (%lu bytes) exceeds provided buffer's size (%u).", __func__, shut_data.length, wLength);
					free(shut_data.data);
					return SHUT_ERROR_OVERFLOW;
				}

				memcpy(data, shut_data.data, shut_data.length);
				free(shut_data.data);
				return shut_data.length;
			}

			upsdebugx(SHUT_DBG_SHUT, "%s: unexpected reply from the device (type: %s).", __func__, shut_strpackettype(shut_data.type));
			free(shut_data.data);
			/* At this point, it's likely a misread mistaken for a SB packet (token): try sending a NAK to get a resend from the device */
			if ((ret = shut_send_one(fd, SHUT_PKT_SB_NAK)) == SHUT_SUCCESS)
				continue;
			upsdebugx(SHUT_DBG_SHUT, "%s: could not send NAK to the device (%s).", __func__, shut_strerror(ret));
			return ret;

		}
		upsdebugx(SHUT_DBG_SHUT, "%s: could not read data from the device.", __func__);

	resync_and_restart:
		upsdebugx(SHUT_DBG_SHUT, "%s: trying to resync and restarting over.", __func__);
		if ((ret = shut_synchronise(fd)) != SHUT_SUCCESS) {
			upsdebugx(SHUT_DBG_SHUT, "%s: could not synchronise with the device (%s).", __func__, shut_strerror(ret));
			return ret;
		}
		try++;
		continue;

	}

	upsdebugx(SHUT_DBG_SHUT, "%s: could not perform the requested transfer.", __func__);
	return SHUT_ERROR_TIMEOUT;
}

/** @brief Take care of SHUT interrupt transfers.
 *
 * The direction of the transfer is inferred from the direction bits of the *endpoint* address.
 *
 * @note This function is not implemented yet and, as such, it always returns @ref SHUT_ERROR_NOT_SUPPORTED, without doing anything.
 * @todo Actually implement this function.
 *
 * @return @ref SHUT_SUCCESS, with *transferred* filled, on success,
 * @return @ref SHUT_ERROR_TIMEOUT, if the transfer could not be performed in the given *timeout* or in @ref SHUT_MAX_TRIES attempts,
 * @return @ref SHUT_ERROR_OVERFLOW, if the received data exceeds *data*'s size (*length*),
 * @return a @ref shut_error "SHUT_ERROR" code, on other errors. */
static int	shut_interrupt_transfer(
	int		 fd,		/**< [in] file descriptor of an already opened device */
	unsigned char	 endpoint,	/**< [in] address of a valid endpoint to communicate with */
	unsigned char	*data,		/**< [in,out] storage location for input or output data */
	int		 length,	/**< [in] number of bytes to be sent or maximum number of bytes to receive (*data* should be at least this size) */
	int		*transferred,	/**< [out] storage location for the number of bytes actually transferred (or `NULL`, if not desired) */
	unsigned int	 timeout	/**< [in] allowed timeout (ms) for the operation (for an unlimited timeout, use value 0) */
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %x, %p, %d, %p, %u)", __func__, fd, endpoint, data, length, (void *)transferred, timeout);

	return SHUT_ERROR_NOT_SUPPORTED;
}

/** @brief Retrieve a string descriptor in C style ASCII using the first language supported by a device.
 * @return the number of bytes returned in data, on success,
 * @return @ref SHUT_ERROR_NO_MEM, on memory allocation errors,
 * @return a @ref shut_error "SHUT_ERROR" code, on other errors. */
static int	shut_get_string_descriptor_ascii(
	int		 fd,		/**< [in] file descriptor of an already opened device */
	uint8_t		 desc_index,	/**< [in] index of the descriptor to retrieve */
	unsigned char	*data,		/**< [out] storage location for the retrieved data */
	int		 length		/**< [in] size of *data* */
) {
	unsigned char	tbuf[255];	/* Some devices choke on size > 255 */
	int		ret, si, di;
	uint16_t	langid;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %u, %p, %d)", __func__, fd, desc_index, data, length);

	/* Asking for the zero'th index is special - it returns a string descriptor that contains all the language IDs supported by the device.
	 * Typically there aren't many - often only one.
	 * Language IDs are 16 bit numbers, and they start at the third byte in the descriptor.
	 * There's also no point in trying to read descriptor 0 with this function.
	 * See USB 2.0 specification section 9.6.7 for more information. */
	if (desc_index == 0)
		return SHUT_ERROR_INVALID_PARAM;

	/* Get language ID */
	ret = shut_control_transfer(
		fd,
		USB_ENDPOINT_IN,
		USB_REQUEST_GET_DESCRIPTOR,
		(USB_DT_STRING << 8) | 0,
		0,
		tbuf,
		sizeof(tbuf),
		SHUT_TIMEOUT
	);
	if (ret < 0)
		return ret;
	if (ret < 4)
		return SHUT_ERROR_IO;
	langid = tbuf[2] | (tbuf[3] << 8);

	/* Get string */
	ret = shut_control_transfer(
		fd,
		USB_ENDPOINT_IN,
		USB_REQUEST_GET_DESCRIPTOR,
		(USB_DT_STRING << 8) | desc_index,
		langid,
		tbuf,
		sizeof(tbuf),
		SHUT_TIMEOUT
	);
	if (ret < 0)
		return ret;
	if (tbuf[1] != USB_DT_STRING)
		return SHUT_ERROR_IO;
	if (tbuf[0] > ret)
		return SHUT_ERROR_IO;

	/* Convert from unicode (UTF-16LE) to ASCII */
	for (di = 0, si = 2; si < tbuf[0]; si += 2) {
		if (di >= (length - 1))
			break;
		if (
			tbuf[si + 1] ||		/* high byte set: unicode */
			tbuf[si] & 0x80		/* 8th bit set: non-ASCII */
		)
			data[di++] = '?';
		else				/* ASCII */
			data[di++] = tbuf[si];
	}
	data[di] = 0;
	return di;
}

/** @} ************************************************************************/


/** @name Communication subdriver implementation
 * @{ *************************************************************************/

/** @brief See shut_communication_subdriver_t::open().
 * @todo Add variable baudrate detection/negotiation. */
static int	libshut_open(
	int		 *fd,
	SHUTDevice_t	 *curDevice,
	char		 *device_path,
	int		(*callback)(
		int		 fd,
		SHUTDevice_t	*hd,
		unsigned char	*rdbuf,
		int		 rdlen
	)
) {
	int			 ret;
	char			 string[SHUT_MAX_STRING_SIZE];

	/* DEVICE descriptor */
	unsigned char		 device_desc_buf[USB_DT_DEVICE_SIZE];
	usb_device_descriptor_t	*device_desc;

	/* HID descriptor */
	unsigned char		 hid_desc_buf[HID_DT_HID_SIZE];
	/* All devices use HID descriptor at index 0.
	 * However, some newer Eaton units have a light HID descriptor at index 0,
	 * and the full version is at index 1 (in which case, bcdDevice == 0x0202). */
	int			 hid_desc_index = 0;

	/* REPORT descriptor */
	unsigned char		 report_desc_buf[HID_DT_REPORT_SIZE_MAX];
	uint16_t		 report_desc_len;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%p, %p, %s, %p)", __func__, (void *)fd, (void *)curDevice, device_path, (void *)callback);
	upsdebugx(SHUT_DBG_DEVICE, "%s: using port '%s'.", __func__, device_path);

	/* If device is still open, close it */
	if (*fd > 0)
		ser_close(*fd, device_path);

	/* Initialise serial port */
	*fd = ser_open(device_path);
	/* TODO: add variable baudrate detection */
	ser_set_speed(*fd, device_path, B2400);
	shut_setline(*fd, 0);

	/* Initialise communication */
	if ((ret = shut_synchronise(*fd)) != SHUT_SUCCESS) {
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		upsdebugx(SHUT_DBG_DEVICE, "%s: no communication with device (%s).", __func__, shut_strerror(ret));
		goto no_device;
	}
	upsdebugx(SHUT_DBG_DEVICE, "%s: communication with device established.", __func__);

	/* Done, if no callback is provided */
	if (!callback)
		return SHUT_SUCCESS;

	/* Get DEVICE descriptor */
	device_desc = (usb_device_descriptor_t *)device_desc_buf;
	ret = shut_control_transfer(
		*fd,
		USB_ENDPOINT_IN | USB_REQUEST_TYPE_STANDARD | USB_RECIPIENT_DEVICE,
		USB_REQUEST_GET_DESCRIPTOR,
		(USB_DT_DEVICE << 8) | 0,
		0,
		device_desc_buf,
		sizeof(device_desc_buf),
		SHUT_TIMEOUT
	);

	if (ret < 0) {
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		upsdebugx(SHUT_DBG_DEVICE, "%s: unable to get DEVICE descriptor (%s).", __func__, shut_strerror(ret));
		goto no_device;
	}

	if ((size_t)ret < sizeof(device_desc_buf)) {
		upsdebugx(SHUT_DBG_DEVICE, "%s: DEVICE descriptor too short (expected %lu bytes, only got %d).", __func__, sizeof(device_desc_buf), ret);
		goto no_device;
	}

	/* Collect the identifying information of this device. */

	free(curDevice->Vendor);
	free(curDevice->Product);
	free(curDevice->Serial);
	free(curDevice->Bus);
	memset(curDevice, 0, sizeof(*curDevice));

	curDevice->VendorID = device_desc->idVendor;
	curDevice->ProductID = device_desc->idProduct;
	curDevice->Bus = xstrdup("serial");
	curDevice->bcdDevice = device_desc->bcdDevice;

	if (device_desc->iManufacturer) {
		ret = shut_get_string_descriptor_ascii(*fd, device_desc->iManufacturer, (unsigned char *)string, sizeof(string));
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		if (ret > 0)
			curDevice->Vendor = xstrdup(string);
	}

	if (device_desc->iProduct) {
		ret = shut_get_string_descriptor_ascii(*fd, device_desc->iProduct, (unsigned char *)string, sizeof(string));
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		if (ret > 0)
			curDevice->Product = xstrdup(string);
	}

	if (device_desc->iSerialNumber) {
		ret = shut_get_string_descriptor_ascii(*fd, device_desc->iSerialNumber, (unsigned char *)string, sizeof(string));
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		if (ret > 0)
			curDevice->Serial = xstrdup(string);
	}

	upsdebugx(SHUT_DBG_DEVICE, "%s: - VendorID: %04x.", __func__, curDevice->VendorID);
	upsdebugx(SHUT_DBG_DEVICE, "%s: - ProductID: %04x.", __func__, curDevice->ProductID);
	upsdebugx(SHUT_DBG_DEVICE, "%s: - Manufacturer: %s.", __func__, curDevice->Vendor ? curDevice->Vendor : "unknown");
	upsdebugx(SHUT_DBG_DEVICE, "%s: - Product: %s.", __func__, curDevice->Product ? curDevice->Product : "unknown");
	upsdebugx(SHUT_DBG_DEVICE, "%s: - Serial Number: %s.", __func__, curDevice->Serial ? curDevice->Serial : "unknown");
	upsdebugx(SHUT_DBG_DEVICE, "%s: - Bus: %s.", __func__, curDevice->Bus);
	upsdebugx(SHUT_DBG_DEVICE, "%s: - Device release number: %04x.", __func__, curDevice->bcdDevice);

	/* FIXME: extend to Eaton OEMs (HP, IBM, ...) */
	if ((curDevice->VendorID == 0x463) && (curDevice->bcdDevice == 0x0202)) {
		upsdebugx(SHUT_DBG_DEVICE, "%s: Eaton device v2.02. Using full report descriptor.", __func__);
		hid_desc_index = 1;
	}

	/* Get HID descriptor */
	ret = shut_control_transfer(
		*fd,
		USB_ENDPOINT_IN | USB_REQUEST_TYPE_STANDARD | USB_RECIPIENT_INTERFACE,
		USB_REQUEST_GET_DESCRIPTOR,
		(USB_DT_HID << 8) | hid_desc_index,
		SHUT_INTERFACE_NUMBER,
		hid_desc_buf,
		sizeof(hid_desc_buf),
		SHUT_TIMEOUT
	);

	if (ret < 0) {
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		upsdebugx(SHUT_DBG_DEVICE, "%s: unable to get HID descriptor (%s).", __func__, shut_strerror(ret));
		goto no_device;
	}

	if ((size_t)ret < sizeof(hid_desc_buf)) {
		upsdebugx(SHUT_DBG_DEVICE, "%s: HID descriptor too short (expected %lu bytes, only got %d).", __func__, sizeof(hid_desc_buf), ret);
		goto no_device;
	}

	report_desc_len = hid_desc_buf[7] | (hid_desc_buf[8] << 8);
	upsdebugx(SHUT_DBG_DEVICE, "%s: HID descriptor retrieved (REPORT descriptor length: %u).", __func__, report_desc_len);

	if (report_desc_len > sizeof(report_desc_buf)) {
		upsdebugx(SHUT_DBG_DEVICE, "%s: REPORT descriptor too long (max %lu).", __func__, sizeof(report_desc_buf));
		ret = SHUT_ERROR_OVERFLOW;
		goto no_device;
	}

	/* Get REPORT descriptor */
	ret = shut_control_transfer(
		*fd,
		USB_ENDPOINT_IN | USB_REQUEST_TYPE_STANDARD | USB_RECIPIENT_INTERFACE,
		USB_REQUEST_GET_DESCRIPTOR,
		(USB_DT_REPORT << 8) | hid_desc_index,
		SHUT_INTERFACE_NUMBER,
		report_desc_buf,
		report_desc_len,
		SHUT_TIMEOUT
	);

	if (ret < 0) {
		if (ret == SHUT_ERROR_NO_MEM)
			goto oom_error;
		upsdebugx(SHUT_DBG_DEVICE, "%s: unable to get REPORT descriptor (%s).", __func__, shut_strerror(ret));
		goto no_device;
	}

	if (ret < report_desc_len) {
		upsdebugx(SHUT_DBG_DEVICE, "%s: warning! REPORT descriptor too short (only got %d bytes).", __func__, ret);
		/* Correct length, if necessary */
		report_desc_len = ret;
	}

	upsdebugx(SHUT_DBG_DEVICE, "%s: REPORT descriptor retrieved (length: %u bytes).", __func__, report_desc_len);

	if (!callback(*fd, curDevice, report_desc_buf, report_desc_len)) {
		upsdebugx(SHUT_DBG_DEVICE, "%s: caller doesn't like this device.", __func__);
		goto no_device;
	}

	upsdebugx(SHUT_DBG_DEVICE, "%s: HID device found.", __func__);
	fflush(stdout);

	return SHUT_SUCCESS;

no_device:	/* Return 'ret', if it is negative (it must contain the shut_error code to return), or SHUT_ERROR_OTHER, if not. */
	upsdebugx(SHUT_DBG_DEVICE, "%s: no appropriate HID device found.", __func__);
	fflush(stdout);

	if (ret < 0)
		return ret;
	return SHUT_ERROR_OTHER;

oom_error:
	fatalx(EXIT_FAILURE, "Out of memory.");
}

/** @brief See shut_communication_subdriver_t::close(). */
static void	libshut_close(
	int	fd
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d)", __func__, fd);

	if (fd < 1)
		return;

	ser_close(fd, NULL);
}

/** @brief See shut_communication_subdriver_t::get_report(). */
static int	libshut_get_report(
	int		 fd,
	int		 ReportId,
	unsigned char	*raw_buf,
	int		 ReportSize
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %x, %p, %d)", __func__, fd, ReportId, raw_buf, ReportSize);

	if (fd < 1)
		return SHUT_ERROR_INVALID_PARAM;

	return shut_control_transfer(
		fd,
		USB_ENDPOINT_IN | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
		HID_REQUEST_GET_REPORT,
		(HID_RT_FEATURE << 8) | ReportId,
		SHUT_INTERFACE_NUMBER,
		raw_buf,
		ReportSize,
		SHUT_TIMEOUT
	);
}

/** @brief See shut_communication_subdriver_t::set_report(). */
static int	libshut_set_report(
	int		 fd,
	int		 ReportId,
	unsigned char	*raw_buf,
	int		 ReportSize
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %x, %p, %d)", __func__, fd, ReportId, raw_buf, ReportSize);

	if (fd < 1)
		return SHUT_ERROR_INVALID_PARAM;

	return shut_control_transfer(
		fd,
		USB_ENDPOINT_OUT | USB_REQUEST_TYPE_CLASS | USB_RECIPIENT_INTERFACE,
		HID_REQUEST_SET_REPORT,
		(HID_RT_FEATURE << 8) | ReportId,
		SHUT_INTERFACE_NUMBER,
		raw_buf,
		ReportSize,
		SHUT_TIMEOUT
	);
}

/** @brief See shut_communication_subdriver_t::get_string(). */
static int	libshut_get_string(
	int	 fd,
	int	 StringIdx,
	char	*buf,
	size_t	 buflen
) {
	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %d, %p, %lu)", __func__, fd, StringIdx, buf, buflen);

	if (fd < 1)
		return SHUT_ERROR_INVALID_PARAM;

	return shut_get_string_descriptor_ascii(fd, StringIdx, (unsigned char *)buf, buflen);
}

/** @brief See shut_communication_subdriver_t::get_interrupt(). */
static int	libshut_get_interrupt(
	int		 fd,
	unsigned char	*buf,
	int		 bufsize,
	int		 timeout
) {
	int	ret;

	upsdebugx(SHUT_DBG_FUNCTION_CALLS, "%s(%d, %p, %d, %d)", __func__, fd, buf, bufsize, timeout);

	if (fd < 1)
		return SHUT_ERROR_INVALID_PARAM;

	/* FIXME: hardcoded interrupt EP => need to get EP descr for IF descr */
	ret = shut_interrupt_transfer(fd, USB_ENDPOINT_IN | 1, buf, bufsize, &bufsize, timeout);

	if (ret != SHUT_SUCCESS)
		return ret;

	return bufsize;
}

shut_communication_subdriver_t	shut_subdriver = {
	SHUT_DRIVER_NAME,
	SHUT_DRIVER_VERSION,
	libshut_open,
	libshut_close,
	libshut_get_report,
	libshut_set_report,
	libshut_get_string,
	libshut_get_interrupt
};

/** @} ************************************************************************/
