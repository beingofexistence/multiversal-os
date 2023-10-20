/*-
 * This header is BSD licensed so anyone can use the definitions to implement
 * compatible drivers/servers.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of IBM nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL IBM OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/virtio/virtio.h,v 1.2 2011/12/06 06:28:32 grehan Exp $
 */

#ifndef _VIRTIO_H_
#define _VIRTIO_H_

#include <sys/types.h>
#include <sys/serialize.h>

struct vq_alloc_info;

/* VirtIO device IDs. */
#define VIRTIO_ID_NETWORK	0x01
#define VIRTIO_ID_BLOCK		0x02
#define VIRTIO_ID_CONSOLE	0x03
#define VIRTIO_ID_ENTROPY	0x04
#define VIRTIO_ID_BALLOON	0x05
#define VIRTIO_ID_IOMEMORY	0x06
#define VIRTIO_ID_SCSI		0x08
#define VIRTIO_ID_9P		0x09

/* Status byte for guest to report progress. */
#define VIRTIO_CONFIG_STATUS_RESET		0x00
#define VIRTIO_CONFIG_STATUS_ACK		0x01
#define VIRTIO_CONFIG_STATUS_DRIVER		0x02
#define VIRTIO_CONFIG_STATUS_DRIVER_OK		0x04
#define VIRTIO_CONFIG_STATUS_DEVICE_NEEDS_RESET	0x40
#define VIRTIO_CONFIG_STATUS_FAILED		0x80

/*
 * Generate interrupt when the virtqueue ring is
 * completely used, even if we've suppressed them.
 */
#define VIRTIO_F_NOTIFY_ON_EMPTY (1 << 24)

/* The device accepts arbitrary descriptor layouts */
#define VIRTIO_F_ANY_LAYOUT (1 << 27)

/*
 * The guest should never negotiate this feature; it
 * is used to detect faulty drivers.
 */
#define VIRTIO_F_BAD_FEATURE (1 << 30)

/*
 * Some VirtIO feature bits (currently bits 28 through 31) are
 * reserved for the transport being used (eg. virtio_ring), the
 * rest are per-device feature bits.
 */
#define VIRTIO_TRANSPORT_F_START	28
#define VIRTIO_TRANSPORT_F_END		32

/*
 * Maximum number of virtqueues per device.
 */
#define VIRTIO_MAX_VIRTQUEUES 8

/*
 * XXX malloc(9) comment not correct on DragonFly
 * Each virtqueue indirect descriptor list must be physically contiguous.
 * To allow us to malloc(9) each list individually, limit the number
 * supported to what will fit in one page. With 4KB pages, this is a limit
 * of 256 descriptors. If there is ever a need for more, we can switch to
 * contigmalloc(9) for the larger allocations, similar to what
 * bus_dmamem_alloc(9) does.
 *
 * Note the sizeof(struct vring_desc) is 16 bytes.
 */
#define VIRTIO_MAX_INDIRECT ((int) (PAGE_SIZE / 16))

/*
 * VirtIO instance variables indices.
 */
#define VIRTIO_IVAR_DEVTYPE		1
#define VIRTIO_IVAR_FEATURE_DESC	2

struct virtio_feature_desc {
	uint64_t	 vfd_val;
	const char	*vfd_str;
};

const char *virtio_device_name(uint16_t devid);
int	 virtio_get_device_type(device_t dev);
void	 virtio_set_feature_desc(device_t dev,
	     struct virtio_feature_desc *feature_desc);
void	 virtio_describe(device_t dev, const char *msg,
	     uint64_t features, struct virtio_feature_desc *feature_desc);

/*
 * VirtIO Bus Methods.
 */
uint64_t virtio_negotiate_features(device_t dev, uint64_t child_features);
int	 virtio_alloc_virtqueues(device_t dev, int nvqs,
	     struct vq_alloc_info *info);
/* Allocate the interrupt resources. */
/*
 * The cpus array must be allocated, and contains -1 or cpuid for each IRQ.
 *
 * This will either allocate all the irqs, and fill in the actual cpus, and
 * return 0, or it will fail and return the number of irq vectors that it
 * managed to get before aborting in "int *cnt".
 * The driver is supposed to check whether the chosen cpu cores match
 * the expectations.
 *
 * Driver should specify use_config as 1, if a configuration should be
 * preferred, where the configuration change notification can be handled
 * efficiently. This only takes effect when a more efficient
 *
 * Fails if any interrupts are already allocated.
 * Caller should check *cnt value after call, to check if all requested IRQS
 * were actually allocated.
 */
int	 virtio_intr_alloc(device_t dev, int *cnt, int use_config, int *cpus);
/* Release all the interrupts, fails if any is currently in use */
int	 virtio_intr_release(device_t dev);
/* Activate a hardware interrupt. */
int	 virtio_setup_intr(device_t dev, uint irq, lwkt_serialize_t);
int	 virtio_teardown_intr(device_t dev, uint irq);
/*
 * Bind the config-notification (-1), or a virtqueue (>= 0) to the irq.
 *
 * If the IRQ is an MSI-X, which is also mapped to a virtqueue, the driver
 * will get a notification callback on each interrupt, whereas if irq is a
 * legacy IRQ, the virtio device can check the ISR to determine if the
 * configuration was updated.
 */
int	 virtio_bind_intr(device_t dev, uint irq, int what,
	     driver_intr_t handler, void *arg);
/* Similarly, -1 is the notification IRQ, >= 0 are the virtqueues. */
int	 virtio_unbind_intr(device_t dev, int what);
int	 virtio_with_feature(device_t dev, uint64_t feature);
/* Get number of interrupts that can probably be allocated. */
int	 virtio_intr_count(device_t dev);
void	 virtio_stop(device_t dev);
int	 virtio_reinit(device_t dev, uint64_t features);
void	 virtio_reinit_complete(device_t dev);

/*
 * Read/write a variable amount from the device specific (ie, network)
 * configuration region. This region is encoded in the same endian as
 * the guest.
 */
void	 virtio_read_device_config(device_t dev, bus_size_t offset,
	     void *dst, int length);
void	 virtio_write_device_config(device_t dev, bus_size_t offset,
	     void *src, int length);

/* Inlined device specific read/write functions for common lengths. */
#define VIRTIO_RDWR_DEVICE_CONFIG(size, type)				\
static inline type							\
__CONCAT(virtio_read_dev_config_,size)(device_t dev,			\
    bus_size_t offset)							\
{									\
	type val;							\
	virtio_read_device_config(dev, offset, &val, sizeof(type));	\
	return (val);							\
}									\
									\
static inline void							\
__CONCAT(virtio_write_dev_config_,size)(device_t dev,			\
    bus_size_t offset, type val)					\
{									\
	virtio_write_device_config(dev, offset, &val, sizeof(type));	\
}

VIRTIO_RDWR_DEVICE_CONFIG(1, uint8_t);
VIRTIO_RDWR_DEVICE_CONFIG(2, uint16_t);
VIRTIO_RDWR_DEVICE_CONFIG(4, uint32_t);

#endif /* _VIRTIO_H_ */
