/*
 * Core functions for libusb
 * Copyright (C) 2007-2008 Daniel Drake <dsd@gentoo.org>
 * Copyright (c) 2001 Johannes Erdfelt <johannes@erdfelt.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libusb.h"
#include "libusbi.h"

#ifdef OS_LINUX
const struct usbi_os_backend * const usbi_backend = &linux_usbfs_backend;
#else
#error "Unsupported OS"
#endif

static struct list_head usb_devs;
static pthread_mutex_t usb_devs_lock = PTHREAD_MUTEX_INITIALIZER;

struct list_head usbi_open_devs;
pthread_mutex_t usbi_open_devs_lock = PTHREAD_MUTEX_INITIALIZER;

/**
 * \mainpage libusb-1.0 API Reference
 * libusb is an open source library that allows you to communicate with USB
 * devices from userspace. For more info, see the
 * <a href="http://libusb.sourceforge.net">libusb homepage</a>.
 *
 * This documentation is aimed at application developers wishing to
 * communicate with USB peripherals from their own software. After reviewing
 * this documentation, feedback and questions can be sent to the
 * <a href="http://sourceforge.net/mail/?group_id=1674">libusb-devel mailing
 * list</a>.
 *
 * This documentation assumes knowledge of how to operate USB devices from
 * a software standpoint (descriptors, configurations, interfaces, endpoints,
 * control/bulk/interrupt/isochronous transfers, etc). Full information
 * can be found in the <a href="http://www.usb.org/developers/docs/">USB 2.0
 * Specification</a> which is available for free download. You can probably
 * find less verbose introductions by searching the web.
 *
 * To begin reading the API documentation, start with the Modules page which
 * links to the different categories of libusb's functionality.
 *
 * libusb does have imperfections. The \ref caveats "caveats" page attempts
 * to document these.
 */

/**
 * \page caveats Caveats
 *
 * \section devresets Device resets
 *
 * The libusb_reset_device() function allows you to reset a device. If your
 * program has to call such a function, it should obviously be aware that
 * the reset will cause device state to change (e.g. register values may be
 * reset).
 *
 * The problem is that any other program could reset the device your program
 * is working with, at any time. libusb does not offer a mechanism to inform
 * you when this has happened, so it will not be clear to your own program
 * why the device state has changed.
 *
 * Ultimately, this is a limitation of writing drivers in userspace.
 * Separation from the USB stack in the underlying kernel makes it difficult
 * for the operating system to deliver such notifications to your program.
 * The Linux kernel USB stack allows such reset notifications to be delivered
 * to in-kernel USB drivers, but it is not clear how such notifications could
 * be delivered to second-class drivers that live in userspace.
 *
 * \section blockonly Blocking-only functionality
 *
 * The functionality listed below is only available through synchronous,
 * blocking functions. There are no asynchronous/non-blocking alternatives,
 * and no clear ways of implementing these.
 *
 * - Configuration activation (libusb_set_configuration())
 * - Interface/alternate setting activation (libusb_set_interface_alt_setting())
 * - Clearing of halt/stall condition (libusb_clear_halt())
 * - Device resets (libusb_reset_device())
 */

/**
 * @defgroup lib Library initialization/deinitialization
 * This page details how to initialize and deinitialize libusb. Initialization
 * must be performed before using any libusb functionality, and similarly you
 * must not call any libusb functions after deinitialization.
 */

/**
 * @defgroup dev Device handling and enumeration
 * The functionality documented below is designed to help with the following
 * operations:
 * - Enumerating the USB devices currently attached to the system
 * - Choosing a device to operate from your software
 * - Opening and closing the chosen device
 *
 * \section nutshell In a nutshell...
 *
 * The description below really makes things sound more complicated than they
 * actually are. The following sequence of function calls will be suitable
 * for almost all scenarios and does not require you to have such a deep
 * understanding of the resource management issues:
 * \code
// discover devices
libusb_device **list;
libusb_device *found = NULL;
size_t cnt = libusb_get_device_list(&list);
size_t i = 0;
if (cnt < 0)
	error();

for (i = 0; i < cnt; i++) {
	libusb_device *device = list[i];
	if (is_interesting(device)) {
		found = device;
		break;
	}
}

if (found) {
	libusb_device_handle *handle = libusb_open(found);
	// etc
}

libusb_free_device_list(list, 1);
\endcode
 *
 * The two important points:
 * - You asked libusb_free_device_list() to unreference the devices (2nd
 *   parameter)
 * - You opened the device before freeing the list and unreferencing the
 *   devices
 *
 * If you ended up with a handle, you can now proceed to perform I/O on the
 * device.
 *
 * \section devshandles Devices and device handles
 * libusb has a concept of a USB device, represented by the
 * \ref libusb_device opaque type. A device represents a USB device that
 * is currently or was previously connected to the system. Using a reference
 * to a device, you can determine certain information about the device (e.g.
 * you can read the descriptor data).
 *
 * The libusb_get_device_list() function can be used to obtain a list of
 * devices currently connected to the system. This is known as device
 * discovery.
 *
 * Just because you have a reference to a device does not mean it is
 * necessarily usable. The device may have been unplugged, you may not have
 * permission to operate such device, or another program or driver may be
 * using the device.
 *
 * When you've found a device that you'd like to operate, you must ask
 * libusb to open the device using the libusb_open() function. Assuming
 * success, libusb then returns you a <em>device handle</em>
 * (a \ref libusb_device_handle pointer). All "real" I/O operations then
 * operate on the handle rather than the original device pointer.
 *
 * \section devref Device discovery and reference counting
 *
 * Device discovery (i.e. calling libusb_get_device_list()) returns a
 * freshly-allocated list of devices. The list itself must be freed when
 * you are done with it. libusb also needs to know when it is OK to free
 * the contents of the list - the devices themselves.
 *
 * To handle these issues, libusb provides you with two separate items:
 * - A function to free the list itself
 * - A reference counting system for the devices inside
 *
 * New devices presented by the libusb_get_device_list() function all have a
 * reference count of 1. You can increase and decrease reference count using
 * libusb_ref_device() and libusb_unref_device(). A device is destroyed when
 * it's reference count reaches 0.
 *
 * With the above information in mind, the process of opening a device can
 * be viewed as follows:
 * -# Discover devices using libusb_get_device_list().
 * -# Choose the device that you want to operate, and call libusb_open().
 * -# Unref all devices in the discovered device list.
 * -# Free the discovered device list.
 *
 * The order is important - you must not unreference the device before
 * attempting to open it, because unreferencing it may destroy the device.
 *
 * For convenience, the libusb_free_device_list() function includes a
 * parameter to optionally unreference all the devices in the list before
 * freeing the list itself. This combines steps 3 and 4 above.
 *
 * As an implementation detail, libusb_open() actually adds a reference to
 * the device in question. This is because the device remains available
 * through the handle via libusb_get_device(). The reference is deleted during
 * libusb_close().
 */

/**
 * @defgroup misc Miscellaneous structures and constants
 * This page documents structures and constants that don't belong anywhere
 * else
 */

/* we traverse usbfs without knowing how many devices we are going to find.
 * so we create this discovered_devs model which is similar to a linked-list
 * which grows when required. it can be freed once discovery has completed,
 * eliminating the need for a list node in the libusb_device structure
 * itself. */
#define DISCOVERED_DEVICES_SIZE_STEP 8

static struct discovered_devs *discovered_devs_alloc(void)
{
	struct discovered_devs *ret =
		malloc(sizeof(*ret) + (sizeof(void *) * DISCOVERED_DEVICES_SIZE_STEP));

	if (ret) {
		ret->len = 0;
		ret->capacity = DISCOVERED_DEVICES_SIZE_STEP;
	}
	return ret;
}

/* append a device to the discovered devices collection. may realloc itself,
 * returning new discdevs. returns NULL on realloc failure. */
struct discovered_devs *discovered_devs_append(
	struct discovered_devs *discdevs, struct libusb_device *dev)
{
	size_t len = discdevs->len;
	size_t capacity;

	/* if there is space, just append the device */
	if (len < discdevs->capacity) {
		discdevs->devices[len] = libusb_ref_device(dev);
		discdevs->len++;
		return discdevs;
	}

	/* exceeded capacity, need to grow */
	usbi_dbg("need to increase capacity");
	capacity = discdevs->capacity + DISCOVERED_DEVICES_SIZE_STEP;
	discdevs = realloc(discdevs,
		sizeof(*discdevs) + (sizeof(void *) * capacity));
	if (discdevs) {
		discdevs->capacity = capacity;
		discdevs->devices[len] = libusb_ref_device(dev);
		discdevs->len++;
	}

	return discdevs;
}

static void discovered_devs_free(struct discovered_devs *discdevs)
{
	size_t i;

	for (i = 0; i < discdevs->len; i++)
		libusb_unref_device(discdevs->devices[i]);

	free(discdevs);
}

/* allocate a new device with reference count 1 */
struct libusb_device *usbi_alloc_device(unsigned long session_id)
{
	size_t priv_size = usbi_backend->device_priv_size;
	struct libusb_device *dev = malloc(sizeof(*dev) + priv_size);
	int r;

	if (!dev)
		return NULL;

	r = pthread_mutex_init(&dev->lock, NULL);
	if (r)
		return NULL;

	dev->refcnt = 1;
	dev->session_data = session_id;
	memset(&dev->os_priv, 0, priv_size);

	pthread_mutex_lock(&usb_devs_lock);
	list_add(&dev->list, &usb_devs);
	pthread_mutex_unlock(&usb_devs_lock);
	return dev;
}

/* call the OS discovery routines to populate descriptors etc */
int usbi_discover_device(struct libusb_device *dev)
{
	int r;
	int i;
	void *user_data;
	unsigned char raw_desc[DEVICE_DESC_LENGTH];
	size_t alloc_size;

	dev->config = NULL;

	r = usbi_backend->begin_discovery(dev, &user_data);
	if (r < 0)
		return r;
	
	r = usbi_backend->get_device_descriptor(dev, raw_desc, user_data);
	if (r < 0)
		goto err;

	usbi_parse_descriptor(raw_desc, "bbWbbbbWWWbbbb", &dev->desc);

	if (dev->desc.bNumConfigurations > USB_MAXCONFIG) {
		usbi_err("too many configurations");
		r = LIBUSB_ERROR_IO;
		goto err;
	}

	if (dev->desc.bNumConfigurations < 1) {
		usbi_dbg("no configurations?");
		r = LIBUSB_ERROR_IO;
		goto err;
	}

	alloc_size = dev->desc.bNumConfigurations
		* sizeof(struct libusb_config_descriptor);
	dev->config = malloc(alloc_size);
	if (!dev->config) {
		r = LIBUSB_ERROR_NO_MEM;
		goto err;
	}

	memset(dev->config, 0, alloc_size);
	for (i = 0; i < dev->desc.bNumConfigurations; i++) {
		unsigned char tmp[8];
		unsigned char *bigbuffer;
		struct libusb_config_descriptor config;

		r = usbi_backend->get_config_descriptor(dev, i, tmp, sizeof(tmp),
			user_data);
		if (r < 0)
			goto err;

		usbi_parse_descriptor(tmp, "bbw", &config);

		bigbuffer = malloc(config.wTotalLength);
		if (!bigbuffer) {
			r = LIBUSB_ERROR_NO_MEM;
			goto err;
		}

		r = usbi_backend->get_config_descriptor(dev, i, bigbuffer,
			config.wTotalLength, user_data);
		if (r < 0) {
			free(bigbuffer);
			goto err;
		}

		r = usbi_parse_configuration(&dev->config[i], bigbuffer);
		free(bigbuffer);
		if (r < 0) {
			usbi_err("parse_configuration failed with code %d", r);
			goto err;
		} else if (r > 0) {
			usbi_warn("descriptor data still left\n");
		}
	}

	usbi_backend->end_discovery(dev, user_data);
	return 0;

err:
	if (dev->config) {
		usbi_clear_configurations(dev);
		free(dev->config);
		dev->config = NULL;
	}

	usbi_backend->end_discovery(dev, user_data);
	return r;
}

struct libusb_device *usbi_get_device_by_session_id(unsigned long session_id)
{
	struct libusb_device *dev;
	struct libusb_device *ret = NULL;

	pthread_mutex_lock(&usb_devs_lock);
	list_for_each_entry(dev, &usb_devs, list)
		if (dev->session_data == session_id) {
			ret = dev;
			break;
		}
	pthread_mutex_unlock(&usb_devs_lock);

	return ret;
}

/** @ingroup dev
 * Returns a list of USB devices currently attached to the system. This is
 * your entry point into finding a USB device to operate.
 *
 * You are expected to unreference all the devices when you are done with
 * them, and then free the list with libusb_free_device_list(). Note that
 * libusb_free_device_list() can unref all the devices for you. Be careful
 * not to unreference a device you are about to open until after you have
 * opened it.
 *
 * This return value of this function indicates the number of devices in
 * the resultant list. The list is actually one element larger, as it is
 * NULL-terminated.
 *
 * \param list output location for a list of devices. Must be later freed with
 * libusb_free_device_list().
 * \returns the number of devices in the outputted list, or LIBUSB_ERROR_NO_MEM
 * on memory allocation failure.
 */
API_EXPORTED size_t libusb_get_device_list(libusb_device ***list)
{
	struct discovered_devs *discdevs = discovered_devs_alloc();
	struct libusb_device **ret;
	int r = 0;
	size_t i;
	size_t len;
	usbi_dbg("");

	if (!discdevs)
		return LIBUSB_ERROR_NO_MEM;

	r = usbi_backend->get_device_list(&discdevs);
	if (r < 0) {
		len = r;
		goto out;
	}

	/* convert discovered_devs into a list */
	len = discdevs->len;
	ret = malloc(sizeof(void *) * (len + 1));
	if (!ret) {
		len = LIBUSB_ERROR_NO_MEM;
		goto out;
	}

	ret[len] = NULL;
	for (i = 0; i < len; i++) {
		struct libusb_device *dev = discdevs->devices[i];
		ret[i] = libusb_ref_device(dev);
	}
	*list = ret;

out:
	discovered_devs_free(discdevs);
	return len;
}

/** \ingroup dev
 * Frees a list of devices previously discovered using
 * libusb_get_device_list(). If the unref_devices parameter is set, the
 * reference count of each device in the list is decremented by 1.
 * \param list the list to free
 * \param unref_devices whether to unref the devices in the list
 */
API_EXPORTED void libusb_free_device_list(libusb_device **list,
	int unref_devices)
{
	if (!list)
		return;

	if (unref_devices) {
		int i = 0;
		struct libusb_device *dev;

		while ((dev = list[i++]) != NULL)
			libusb_unref_device(dev);
	}
	free(list);
}

/** \ingroup dev
 * Get the number of the bus that a device is connected to.
 * \param dev a device
 * \returns the bus number
 */
API_EXPORTED uint8_t libusb_get_bus_number(libusb_device *dev)
{
	return dev->bus_number;
}

/** \ingroup dev
 * Get the address of the device on the bus it is connected to.
 * \param dev a device
 * \returns the device address
 */
API_EXPORTED uint8_t libusb_get_device_address(libusb_device *dev)
{
	return dev->device_address;
}

/** \ingroup dev
 * Convenience function to retrieve the wMaxPacketSize value for a particular
 * endpoint. This is useful for setting up isochronous transfers.
 *
 * \param dev a device
 * \param endpoint address of the endpoint in question
 * \returns the wMaxPacketSize value, or LIBUSB_ERROR_NOT_FOUND if the endpoint
 * does not exist.
 */
API_EXPORTED int libusb_get_max_packet_size(libusb_device *dev,
	unsigned char endpoint)
{
	int iface_idx;
	/* FIXME: active config considerations? */
	struct libusb_config_descriptor *config = dev->config;

	for (iface_idx = 0; iface_idx < config->bNumInterfaces; iface_idx++) {
		const struct libusb_interface *iface = &config->interface[iface_idx];
		int altsetting_idx;

		for (altsetting_idx = 0; altsetting_idx < iface->num_altsetting;
				altsetting_idx++) {
			const struct libusb_interface_descriptor *altsetting
				= &iface->altsetting[altsetting_idx];
			int ep_idx;

			for (ep_idx = 0; ep_idx < altsetting->bNumEndpoints; ep_idx++) {
				const struct libusb_endpoint_descriptor *ep =
					&altsetting->endpoint[ep_idx];
				if (ep->bEndpointAddress == endpoint)
					return ep->wMaxPacketSize;
			}
		}
	}

	return LIBUSB_ERROR_NOT_FOUND;
}

/** \ingroup dev
 * Increment the reference count of a device.
 * \param dev the device to reference
 * \returns the same device
 */
API_EXPORTED libusb_device *libusb_ref_device(libusb_device *dev)
{
	pthread_mutex_lock(&dev->lock);
	dev->refcnt++;
	pthread_mutex_unlock(&dev->lock);
	return dev;
}

/** \ingroup dev
 * Decrement the reference count of a device. If the decrement operation
 * causes the reference count to reach zero, the device shall be destroyed.
 * \param dev the device to unreference
 */
API_EXPORTED void libusb_unref_device(libusb_device *dev)
{
	int refcnt;

	if (!dev)
		return;

	pthread_mutex_lock(&dev->lock);
	refcnt = --dev->refcnt;
	pthread_mutex_unlock(&dev->lock);

	if (refcnt == 0) {
		usbi_dbg("destroy device %04x:%04x", dev->desc.idVendor,
			dev->desc.idProduct);

		if (usbi_backend->destroy_device)
			usbi_backend->destroy_device(dev);

		pthread_mutex_lock(&usb_devs_lock);
		list_del(&dev->list);
		pthread_mutex_unlock(&usb_devs_lock);

		if (dev->config) {
			usbi_clear_configurations(dev);
			free(dev->config);
		}
		free(dev);
	}
}

/** \ingroup dev
 * Open a device and obtain a device handle. A handle allows you to perform
 * I/O on the device in question.
 *
 * Internally, this function adds a reference to the device and makes it
 * available to you through libusb_get_device(). This reference is removed
 * during libusb_close().
 *
 * This is a non-blocking function; no requests are sent over the bus.
 *
 * \param dev the device to open
 * \returns a handle for the device, or NULL on error
 */
API_EXPORTED libusb_device_handle *libusb_open(libusb_device *dev)
{
	struct libusb_device_handle *handle;
	size_t priv_size = usbi_backend->device_handle_priv_size;
	int r;
	usbi_dbg("open %04x:%04x", dev->desc.idVendor, dev->desc.idProduct);

	handle = malloc(sizeof(*handle) + priv_size);
	if (!handle)
		return NULL;

	r = pthread_mutex_init(&handle->lock, NULL);
	if (r)
		return NULL;

	handle->dev = libusb_ref_device(dev);
	handle->claimed_interfaces = 0;
	memset(&handle->os_priv, 0, priv_size);

	r = usbi_backend->open(handle);
	if (r < 0) {
		libusb_unref_device(dev);
		free(handle);
		return NULL;
	}

	pthread_mutex_lock(&usbi_open_devs_lock);
	list_add(&handle->list, &usbi_open_devs);
	pthread_mutex_unlock(&usbi_open_devs_lock);
	return handle;
}

/** \ingroup dev
 * Convenience function for finding a device with a particular
 * <tt>idVendor</tt>/<tt>idProduct</tt> combination. This function is intended
 * for those scenarios where you are using libusb to knock up a quick test
 * application - it allows you to avoid calling libusb_get_device_list() and
 * worrying about traversing/freeing the list.
 *
 * This function has limitations and is hence not intended for use in real
 * applications: if multiple devices have the same IDs it will only
 * give you the first one, etc.
 *
 * \param vendor_id the idVendor value to search for
 * \param product_id the idProduct value to search for
 * \returns a handle for the first found device, or NULL on error or if the
 * device could not be found. */
API_EXPORTED libusb_device_handle *libusb_open_device_with_vid_pid(
	uint16_t vendor_id, uint16_t product_id)
{
	struct libusb_device **devs;
	struct libusb_device *found = NULL;
	struct libusb_device *dev;
	struct libusb_device_handle *handle = NULL;
	size_t i = 0;

	if (libusb_get_device_list(&devs) < 0)
		return NULL;

	while ((dev = devs[i++]) != NULL) {
		const struct libusb_device_descriptor *desc =
			libusb_get_device_descriptor(dev);
		if (desc->idVendor == vendor_id && desc->idProduct == product_id) {
			found = dev;
			break;
		}
	}

	if (found)
		handle = libusb_open(found);

	libusb_free_device_list(devs, 1);
	return handle;
}

static void do_close(struct libusb_device_handle *dev_handle)
{
	usbi_backend->close(dev_handle);
	libusb_unref_device(dev_handle->dev);
}

/** \ingroup dev
 * Close a device handle. Should be called on all open handles before your
 * application exits.
 *
 * Internally, this function destroys the reference that was added by
 * libusb_open() on the given device.
 *
 * This is a non-blocking function; no requests are sent over the bus.
 *
 * \param dev_handle the handle to close
 */
API_EXPORTED void libusb_close(libusb_device_handle *dev_handle)
{
	if (!dev_handle)
		return;
	usbi_dbg("");

	pthread_mutex_lock(&usbi_open_devs_lock);
	list_del(&dev_handle->list);
	pthread_mutex_unlock(&usbi_open_devs_lock);

	do_close(dev_handle);
	free(dev_handle);
}

/** \ingroup dev
 * Get the underlying device for a handle. This function does not modify
 * the reference count of the returned device, so do not feel compelled to
 * unreference it when you are done.
 * \param dev_handle a device handle
 * \returns the underlying device
 */
API_EXPORTED libusb_device *libusb_get_device(libusb_device_handle *dev_handle)
{
	return dev_handle->dev;
}

/** \ingroup dev
 * Set the active configuration for a device. The operating system may have
 * already set an active configuration on the device, but for portability
 * reasons you should use this function to select the configuration you want
 * before claiming any interfaces.
 *
 * If you wish to change to another configuration at some later time, you
 * must release all claimed interfaces using libusb_release_interface() before
 * setting a new active configuration.
 *
 * You should always use this function rather than formulating your own
 * SET_CONFIGURATION control request. This is because the underlying operating
 * system needs to know when such changes happen.
 *
 * This is a blocking function.
 *
 * \param dev a device handle
 * \param configuration the bConfigurationValue of the configuration you
 * wish to activate
 * \returns 0 on success
 * \returns LIBUSB_ERROR_NOT_FOUND if the requested configuration does not exist
 * \returns LIBUSB_ERROR_BUSY if interfaces are currently claimed
 * \returns another LIBUSB_ERROR code on other failure
 */
API_EXPORTED int libusb_set_configuration(libusb_device_handle *dev,
	int configuration)
{
	usbi_dbg("configuration %d", configuration);
	return usbi_backend->set_configuration(dev, configuration);
}

/** \ingroup dev
 * Claim an interface on a given device handle. You must claim the interface
 * you wish to use before you can perform I/O on any of its endpoints.
 *
 * It is legal to attempt to claim an already-claimed interface, in which
 * case libusb just returns 0 without doing anything.
 *
 * Claiming of interfaces is a purely logical operation; it does not cause
 * any requests to be sent over the bus. Interface claiming is used to
 * instruct the underlying operating system that your application wishes
 * to take ownership of the interface.
 *
 * This is a non-blocking function.
 *
 * \param dev a device handle
 * \param interface_number the <tt>bInterfaceNumber</tt> of the interface you
 * wish to claim
 * \returns 0 on success
 * \returns LIBUSB_ERROR_NOT_FOUND if the requested interface does not exist
 * \returns LIBUSB_ERROR_BUSY if another program or driver has claimed the
 * interface
 * \returns a LIBUSB_ERROR code on other failure
 */
API_EXPORTED int libusb_claim_interface(libusb_device_handle *dev,
	int interface_number)
{
	int r = 0;

	usbi_dbg("interface %d", interface_number);
	if (interface_number >= sizeof(dev->claimed_interfaces) * 8)
		return LIBUSB_ERROR_INVALID_PARAM;

	pthread_mutex_lock(&dev->lock);
	if (dev->claimed_interfaces & (1 << interface_number))
		goto out;

	r = usbi_backend->claim_interface(dev, interface_number);
	if (r == 0)
		dev->claimed_interfaces |= 1 << interface_number;

out:
	pthread_mutex_unlock(&dev->lock);
	return r;
}

/** \ingroup dev
 * Release an interface previously claimed with libusb_claim_interface(). You
 * should release all claimed interfaces before closing a device handle.
 *
 * This is a non-blocking function which does not generate any bus requests.
 *
 * \param dev a device handle
 * \param interface_number the <tt>bInterfaceNumber</tt> of the
 * previously-claimed interface
 * \returns 0 on success, or a LIBUSB_ERROR code on failure.
 * LIBUSB_ERROR_NOT_FOUND indicates that the interface was not claimed.
 */
API_EXPORTED int libusb_release_interface(libusb_device_handle *dev,
	int interface_number)
{
	int r;

	usbi_dbg("interface %d", interface_number);
	if (interface_number >= sizeof(dev->claimed_interfaces) * 8)
		return LIBUSB_ERROR_INVALID_PARAM;

	pthread_mutex_lock(&dev->lock);
	if (!(dev->claimed_interfaces & (1 << interface_number))) {
		r = LIBUSB_ERROR_NOT_FOUND;
		goto out;
	}

	r = usbi_backend->release_interface(dev, interface_number);
	if (r == 0)
		dev->claimed_interfaces &= ~(1 << interface_number);

out:
	pthread_mutex_unlock(&dev->lock);
	return r;
}

/** \ingroup dev
 * Activate an alternate setting for an interface. The interface must have
 * been previously claimed with libusb_claim_interface().
 *
 * You should always use this function rather than formulating your own
 * SET_INTERFACE control request. This is because the underlying operating
 * system needs to know when such changes happen.
 *
 * This is a blocking function.
 *
 * \param dev a device handle
 * \param interface_number the <tt>bInterfaceNumber</tt> of the
 * previously-claimed interface
 * \param alternate_setting the <tt>bAlternateSetting</tt> of the alternate
 * setting to activate
 * \returns 0 on success
 * \returns LIBUSB_ERROR_NOT_FOUND if the interface was not claimed, or the
 * requested alternate setting does not exist
 * \returns another LIBUSB_ERROR code on other failure
 */
API_EXPORTED int libusb_set_interface_alt_setting(libusb_device_handle *dev,
	int interface_number, int alternate_setting)
{
	usbi_dbg("interface %d altsetting %d", interface_number, alternate_setting);
	if (interface_number >= sizeof(dev->claimed_interfaces) * 8)
		return LIBUSB_ERROR_INVALID_PARAM;

	pthread_mutex_lock(&dev->lock);
	if (!(dev->claimed_interfaces & (1 << interface_number))) {
		pthread_mutex_unlock(&dev->lock);	
		return LIBUSB_ERROR_NOT_FOUND;
	}
	pthread_mutex_unlock(&dev->lock);

	return usbi_backend->set_interface_altsetting(dev, interface_number,
		alternate_setting);
}

/** \ingroup dev
 * Clear the halt/stall condition for an endpoint. Endpoints with halt status
 * are unable to receive or transmit data until the halt condition is stalled.
 *
 * You should cancel all pending transfers before attempting to clear the halt
 * condition.
 *
 * This is a blocking function.
 *
 * \param dev a device handle
 * \param endpoint the endpoint to clear halt status
 * \returns 0 on success
 * \returns LIBUSB_ERROR_NOT_FOUND if the endpoint does not exist
 * \returns another LIBUSB_ERROR code on other failure
 */
API_EXPORTED int libusb_clear_halt(libusb_device_handle *dev,
	unsigned char endpoint)
{
	usbi_dbg("endpoint %x", endpoint);
	return usbi_backend->clear_halt(dev, endpoint);
}

/** \ingroup dev
 * Perform a USB port reset to reinitialize a device. The system will attempt
 * to restore the previous configuration and alternate settings after the
 * reset has completed.
 *
 * If the reset fails, the descriptors change, or the previous state cannot be
 * restored, the device will appear to be disconnected and reconnected. This
 * means that the device handle is no longer valid (you should close it) and
 * rediscover the device. A return code of LIBUSB_ERROR_NOT_FOUND indicates
 * when this is the case.
 *
 * This is a blocking function which usually incurs a noticeable delay.
 *
 * \param dev a handle of the device to reset
 * \returns 0 on success
 * \returns LIBUSB_ERROR_NOT_FOUND if re-enumeration is required
 * \returns another LIBUSB_ERROR code on other failure
 */
API_EXPORTED int libusb_reset_device(libusb_device_handle *dev)
{
	usbi_dbg("");
	return usbi_backend->reset_device(dev);
}

/** \ingroup dev
 * Determine if a kernel driver is active on an interface. If a kernel driver
 * is active, you cannot claim the interface, and libusb will be unable to
 * perform I/O.
 *
 * \param dev a device handle
 * \param interface the interface to check
 * \returns 0 if no kernel driver is active
 * \returns 1 if a kernel driver is active
 * \returns LIBUSB_ERROR code on failure
 * \see libusb_detach_kernel_driver()
 */
API_EXPORTED int libusb_kernel_driver_active(libusb_device_handle *dev,
	int interface)
{
	usbi_dbg("interface %d", interface);
	if (usbi_backend->kernel_driver_active)
		return usbi_backend->kernel_driver_active(dev, interface);
	else
		return LIBUSB_ERROR_NOT_SUPPORTED;
}

/** \ingroup dev
 * Detach a kernel driver from an interface. If successful, you will then be
 * able to claim the interface and perform I/O.
 *
 * \param dev a device handle
 * \param interface the interface to detach the driver from
 * \returns 0 on success
 * \returns LIBUSB_ERROR_NOT_FOUND if no kernel driver was active
 * \returns LIBUSB_ERROR_INVALID_PARAM if the interface does not exist
 * \returns another LIBUSB_ERROR code on other failure
 * \see libusb_kernel_driver_active()
 */
API_EXPORTED int libusb_detach_kernel_driver(libusb_device_handle *dev,
	int interface)
{
	usbi_dbg("interface %d", interface);
	if (usbi_backend->detach_kernel_driver)
		return usbi_backend->detach_kernel_driver(dev, interface);
	else
		return LIBUSB_ERROR_NOT_SUPPORTED;
}

/** \ingroup lib
 * Initialize libusb. This function must be called before calling any other
 * libusb function.
 * \returns 0 on success, or a LIBUSB_ERROR code on failure
 */
API_EXPORTED int libusb_init(void)
{
	usbi_dbg("");

	if (usbi_backend->init) {
		int r = usbi_backend->init();
		if (r)
			return r;
	}

	list_init(&usb_devs);
	list_init(&usbi_open_devs);
	usbi_io_init();
	return 0;
}

/** \ingroup lib
 * Deinitialize libusb. Should be called after closing all open devices and
 * before your application terminates.
 */
API_EXPORTED void libusb_exit(void)
{
	usbi_dbg("");

	pthread_mutex_lock(&usbi_open_devs_lock);
	if (!list_empty(&usbi_open_devs)) {
		struct libusb_device_handle *devh;
		struct libusb_device_handle *tmp;

		usbi_dbg("naughty app left some devices open!");
		list_for_each_entry_safe(devh, tmp, &usbi_open_devs, list) {
			list_del(&devh->list);
			do_close(devh);
			free(devh);
		}
	}
	pthread_mutex_unlock(&usbi_open_devs_lock);

	if (usbi_backend->exit)
		usbi_backend->exit();
}

void usbi_log(enum usbi_log_level level, const char *function,
	const char *format, ...)
{
	va_list args;
	FILE *stream = stdout;
	const char *prefix;

	switch (level) {
	case LOG_LEVEL_INFO:
		prefix = "info";
		break;
	case LOG_LEVEL_WARNING:
		stream = stderr;
		prefix = "warning";
		break;
	case LOG_LEVEL_ERROR:
		stream = stderr;
		prefix = "error";
		break;
	case LOG_LEVEL_DEBUG:
		stream = stderr;
		prefix = "debug";
		break;
	default:
		stream = stderr;
		prefix = "unknown";
		break;
	}

	fprintf(stream, "libusb:%s [%s] ", prefix, function);

	va_start (args, format);
	vfprintf(stream, format, args);
	va_end (args);

	fprintf(stream, "\n");
}

