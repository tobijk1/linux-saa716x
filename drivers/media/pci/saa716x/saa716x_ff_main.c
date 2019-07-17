// SPDX-License-Identifier: GPL-2.0+

#include <linux/firmware.h>
#include <linux/videodev2.h>
#include <linux/dvb/video.h>
#include <linux/dvb/audio.h>
#include <linux/dvb/osd.h>

#include "saa716x_mod.h"

#include "saa716x_dma_reg.h"
#include "saa716x_fgpi_reg.h"
#include "saa716x_greg_reg.h"
#include "saa716x_phi_reg.h"
#include "saa716x_spi_reg.h"
#include "saa716x_msi_reg.h"

#include "saa716x_adap.h"
#include "saa716x_boot.h"
#include "saa716x_pci.h"
#include "saa716x_gpio.h"
#include "saa716x_priv.h"

#include "saa716x_ff.h"
#include "saa716x_ff_cmd.h"
#include "saa716x_ff_phi.h"

#include "stv6110x.h"
#include "stv090x.h"
#include "isl6423.h"

unsigned int video_capture;
module_param(video_capture, int, 0644);
MODULE_PARM_DESC(video_capture, "capture digital video coming from STi7109: 0=off, 1=one-shot. default off");

#define DRIVER_NAME	"SAA716x FF"

static void saa716x_ff_spi_write(struct saa716x_dev *saa716x, const u8 *data,
				 int length)
{
	int i, rounds;

	for (i = 0; i < length; i++) {
		SAA716x_EPWR(SPI, SPI_DATA, data[i]);
		for (rounds = 0; rounds < 5000; rounds++)
			if (SAA716x_EPRD(SPI, SPI_STATUS) & SPI_TRANSFER_FLAG)
				break;
	}
}

static int saa716x_ff_fpga_init(struct saa716x_ff_dev *saa716x_ff)
{
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;
	int fpgaInit;
	int fpgaDone;
	int rounds;
	int ret;
	const struct firmware *fw;

	/* request the FPGA firmware */
	ret = request_firmware(&fw, "dvb-ttpremium-fpga-01.fw",
			       &saa716x->pdev->dev);
	if (ret) {
		if (ret == -ENOENT)
			pci_err(saa716x->pdev, "dvb-ttpremium: could not find FPGA firmware: dvb-ttpremium-fpga-01.fw");
		else
			pci_err(saa716x->pdev, "dvb-ttpremium: FPGA firmware request failed (error %i)", ret);
		return -EINVAL;
	}

	/* set FPGA PROGRAMN high */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_FPGA_PROGRAMN, 1);
	msleep(10);

	/* set FPGA PROGRAMN low to set it into configuration mode */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_FPGA_PROGRAMN, 0);
	msleep(10);

	/* set FPGA PROGRAMN high to start configuration process */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_FPGA_PROGRAMN, 1);

	rounds = 0;
	fpgaInit = saa716x_gpio_read(saa716x, TT_PREMIUM_GPIO_FPGA_INITN);
	while (fpgaInit == 0 && rounds < 5000) {
		fpgaInit = saa716x_gpio_read(saa716x,
					     TT_PREMIUM_GPIO_FPGA_INITN);
		rounds++;
	}
	pci_dbg(saa716x->pdev, "FPGA INITN=%d, rounds=%d", fpgaInit, rounds);

	SAA716x_EPWR(SPI, SPI_CLOCK_COUNTER, 0x08);
	SAA716x_EPWR(SPI, SPI_CONTROL_REG, SPI_MODE_SELECT);

	msleep(10);

	fpgaDone = saa716x_gpio_read(saa716x, TT_PREMIUM_GPIO_FPGA_DONE);
	pci_dbg(saa716x->pdev, "FPGA DONE=%d", fpgaDone);
	pci_dbg(saa716x->pdev, "FPGA write bitstream");
	saa716x_ff_spi_write(saa716x, fw->data, fw->size);
	pci_dbg(saa716x->pdev, "FPGA write bitstream done");
	fpgaDone = saa716x_gpio_read(saa716x, TT_PREMIUM_GPIO_FPGA_DONE);
	pci_dbg(saa716x->pdev, "FPGA DONE=%d", fpgaDone);

	msleep(10);

	release_firmware(fw);

	if (!fpgaDone) {
		pci_err(saa716x->pdev, "FPGA is not responding, did you connect the power supply?");
		return -EINVAL;
	}

	sti7109->fpga_version = SAA716x_EPRD(PHI_1, FPGA_ADDR_VERSION);
	pci_info(saa716x->pdev, "FPGA version %X.%02X",
		sti7109->fpga_version >> 8, sti7109->fpga_version & 0xFF);

	return 0;
}

static int saa716x_ff_st7109_init(struct saa716x_ff_dev *saa716x_ff)
{
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;
	int i;
	int length;
	u32 requestedBlock;
	u32 writtenBlock;
	u32 numBlocks;
	u32 blockSize;
	u32 lastBlockSize;
	u64 startTime;
	u64 currentTime;
	u64 waitTime;
	int ret;
	const struct firmware *fw;
	u32 loaderVersion;
	u32 options;

	/* request the st7109 loader */
	ret = request_firmware(&fw, "dvb-ttpremium-loader-01.fw",
			       &saa716x->pdev->dev);
	if (ret) {
		if (ret == -ENOENT)
			pci_err(saa716x->pdev, "dvb-ttpremium: could not find ST7109 loader: dvb-ttpremium-loader-01.fw");
		else
			pci_err(saa716x->pdev, "dvb-ttpremium: loader firmware request failed (error %i)", ret);
		return -EINVAL;
	}
	loaderVersion = (fw->data[0x1385] << 8) | fw->data[0x1384];
	pci_info(saa716x->pdev, "loader version %X.%02X",
		loaderVersion >> 8, loaderVersion & 0xFF);

	saa716x_ff_phi_write(saa716x_ff, 0, fw->data, fw->size);
	msleep(10);

	release_firmware(fw);

	/* take ST out of reset */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_RESET_BACKEND, 1);

	startTime = jiffies;
	waitTime = 0;
	do {
		requestedBlock = SAA716x_EPRD(PHI_1, 0x3ffc);
		if (requestedBlock == 1)
			break;

		currentTime = jiffies;
		waitTime = currentTime - startTime;
	} while (waitTime < (1 * HZ));

	if (waitTime >= 1 * HZ) {
		pci_err(saa716x->pdev, "STi7109 seems to be DEAD!");
		return -1;
	}
	pci_dbg(saa716x->pdev, "STi7109 ready after %llu ticks", waitTime);

	/* request the st7109 firmware */
	ret = request_firmware(&fw, "dvb-ttpremium-st7109-01.fw",
			       &saa716x->pdev->dev);
	if (ret) {
		if (ret == -ENOENT)
			pci_err(saa716x->pdev, "dvb-ttpremium: could not find ST7109 firmware: dvb-ttpremium-st7109-01.fw");
		else
			pci_err(saa716x->pdev, "dvb-ttpremium: ST7109 firmware request failed (error %i)", ret);
		return -EINVAL;
	}

	pci_dbg(saa716x->pdev, "download ST7109 firmware");
	writtenBlock = 0;
	blockSize = 0x3c00;
	length = fw->size;
	numBlocks = length / blockSize;
	lastBlockSize = length % blockSize;
	for (i = 0; i < length; i += blockSize) {
		writtenBlock++;
		/* write one block (last may differ from blockSize) */
		if (lastBlockSize && writtenBlock == (numBlocks + 1))
			saa716x_ff_phi_write(saa716x_ff, 0, &fw->data[i],
					     lastBlockSize);
		else
			saa716x_ff_phi_write(saa716x_ff, 0, &fw->data[i],
					     blockSize);

		SAA716x_EPWR(PHI_1, 0x3ff8, writtenBlock);
		startTime = jiffies;
		waitTime = 0;
		do {
			requestedBlock = SAA716x_EPRD(PHI_1, 0x3ffc);
			if (requestedBlock == (writtenBlock + 1))
				break;

			currentTime = jiffies;
			waitTime = currentTime - startTime;
		} while (waitTime < (1 * HZ));

		if (waitTime >= 1 * HZ) {
			pci_err(saa716x->pdev, "STi7109 seems to be DEAD!");
			release_firmware(fw);
			return -1;
		}
	}

	/* disable frontend support through ST firmware */
	options = 0x00000001;
	/* enable faster EMI timings in ST firmware for FPGA 1.10 and later */
	if (sti7109->fpga_version >= 0x110)
		options |= 0x00000002;
	SAA716x_EPWR(PHI_1, 0x3ff4, options);

	/* indicate end of transfer */
	writtenBlock++;
	writtenBlock |= 0x80000000;
	SAA716x_EPWR(PHI_1, 0x3ff8, writtenBlock);

	pci_dbg(saa716x->pdev, "download ST7109 firmware done");

	release_firmware(fw);

	return 0;
}

static int saa716x_usercopy(struct dvb_device *dvbdev,
			    unsigned int cmd, unsigned long arg,
			    int (*func)(struct dvb_device *dvbdev,
			    unsigned int cmd, void *arg))
{
	char    sbuf[128];
	void    *mbuf = NULL;
	void    *parg = NULL;
	int     err  = -EINVAL;

	/*  Copy arguments into temp kernel buffer  */
	switch (_IOC_DIR(cmd)) {
	case _IOC_NONE:
		/*
		 * For this command, the pointer is actually an integer
		 * argument.
		 */
		parg = (void *) arg;
		break;
	case _IOC_READ: /* some v4l ioctls are marked wrong ... */
	case _IOC_WRITE:
	case (_IOC_WRITE | _IOC_READ):
		if (_IOC_SIZE(cmd) <= sizeof(sbuf)) {
			parg = sbuf;
		} else {
			/* too big to allocate from stack */
			mbuf = kmalloc(_IOC_SIZE(cmd), GFP_KERNEL);
			if (!mbuf)
				return -ENOMEM;
			parg = mbuf;
		}

		err = -EFAULT;
		if (copy_from_user(parg, (void __user *)arg, _IOC_SIZE(cmd)))
			goto out;
		break;
	}

	/* call driver */
	err = func(dvbdev, cmd, parg);
	if (err == -ENOIOCTLCMD)
		err = -EINVAL;

	if (err < 0)
		goto out;

	/*  Copy results into user buffer  */
	switch (_IOC_DIR(cmd)) {
	case _IOC_READ:
	case (_IOC_WRITE | _IOC_READ):
		if (copy_to_user((void __user *)arg, parg, _IOC_SIZE(cmd)))
			err = -EFAULT;
		break;
	}

out:
	kfree(mbuf);
	return err;
}

static long dvb_osd_ioctl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	struct dvb_device *dvbdev = file->private_data;
	struct sti7109_dev *sti7109 = dvbdev->priv;
	int err = -EINVAL;

	if (!dvbdev)
		return -ENODEV;

	if (cmd == OSD_RAW_CMD) {
		osd_raw_cmd_t raw_cmd;
		u8 hdr[4];

		err = -EFAULT;
		if (copy_from_user(&raw_cmd, (void __user *)arg,
				   _IOC_SIZE(cmd)))
			goto out;

		if (copy_from_user(hdr, (void __user *)raw_cmd.cmd_data, 4))
			goto out;

		if (hdr[3] == 4)
			err = sti7109_raw_osd_cmd(sti7109, &raw_cmd);
		else
			err = sti7109_raw_cmd(sti7109, &raw_cmd);

		if (err)
			goto out;

		if (copy_to_user((void __user *)arg, &raw_cmd, _IOC_SIZE(cmd)))
			err = -EFAULT;
	} else if (cmd == OSD_RAW_DATA) {
		osd_raw_data_t raw_data;

		err = -EFAULT;
		if (copy_from_user(&raw_data, (void __user *)arg,
				   _IOC_SIZE(cmd)))
			goto out;

		err = sti7109_raw_data(sti7109, &raw_data);
		if (err)
			goto out;

		if (copy_to_user((void __user *)arg, &raw_data, _IOC_SIZE(cmd)))
			err = -EFAULT;
	}

out:
	return err;
}

static const struct file_operations dvb_osd_fops = {
	.unlocked_ioctl	= dvb_osd_ioctl,
	.open		= dvb_generic_open,
	.release	= dvb_generic_release,
};

static const struct dvb_device dvbdev_osd = {
	.priv		= NULL,
	.users		= 2,
	.writers	= 2,
	.fops		= &dvb_osd_fops,
	.kernel_ioctl	= NULL,
};

static int saa716x_ff_osd_exit(struct saa716x_ff_dev *saa716x_ff)
{
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	dvb_unregister_device(sti7109->osd_dev);
	return 0;
}

static int saa716x_ff_osd_init(struct saa716x_ff_dev *saa716x_ff)
{
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct saa716x_adapter *saa716x_adap = saa716x->saa716x_adap;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	dvb_register_device(&saa716x_adap->dvb_adapter,
			    &sti7109->osd_dev,
			    &dvbdev_osd,
			    sti7109,
			    DVB_DEVICE_OSD,
			    0);
	return 0;
}

static int do_dvb_audio_ioctl(struct dvb_device *dvbdev,
			      unsigned int cmd, void *parg)
{
	struct sti7109_dev *sti7109	= dvbdev->priv;
	//struct saa716x_dev *saa716x	= sti7109->dev;
	int ret = 0;

	switch (cmd) {
	case AUDIO_GET_PTS:
	{
		*(u64 *)parg = sti7109->audio_pts;
		break;
	}
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

static long dvb_audio_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct dvb_device *dvbdev = file->private_data;

	if (!dvbdev)
		return -ENODEV;

	return saa716x_usercopy(dvbdev, cmd, arg, do_dvb_audio_ioctl);
}

static const struct file_operations dvb_audio_fops = {
	.unlocked_ioctl	= dvb_audio_ioctl,
	.open		= dvb_generic_open,
	.release	= dvb_generic_release,
};

static const struct dvb_device dvbdev_audio = {
	.priv		= NULL,
	.users		= 1,
	.writers	= 1,
	.fops		= &dvb_audio_fops,
	.kernel_ioctl	= NULL,
};

static int saa716x_ff_audio_exit(struct saa716x_ff_dev *saa716x_ff)
{
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	dvb_unregister_device(sti7109->audio_dev);
	return 0;
}

static int saa716x_ff_audio_init(struct saa716x_ff_dev *saa716x_ff)
{
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct saa716x_adapter *saa716x_adap = saa716x->saa716x_adap;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	dvb_register_device(&saa716x_adap->dvb_adapter,
			    &sti7109->audio_dev,
			    &dvbdev_audio,
			    sti7109,
			    DVB_DEVICE_AUDIO,
			    0);
	return 0;
}

static ssize_t ringbuffer_write_user(struct dvb_ringbuffer *rbuf,
				     const u8 __user *buf, size_t len)
{
	size_t todo = len;
	size_t split;

	split = (rbuf->pwrite + len > rbuf->size) ?
		 rbuf->size - rbuf->pwrite : 0;

	if (split > 0) {
		if (copy_from_user(rbuf->data+rbuf->pwrite, buf, split))
			return -EFAULT;
		buf += split;
		todo -= split;
		rbuf->pwrite = 0;
	}
	if (copy_from_user(rbuf->data+rbuf->pwrite, buf, todo))
		return -EFAULT;
	rbuf->pwrite = (rbuf->pwrite + todo) % rbuf->size;

	return len;
}

static void ringbuffer_read_tofifo(struct dvb_ringbuffer *rbuf,
				   struct saa716x_ff_dev *saa716x_ff, int len)
{
	size_t todo = len;
	size_t split;

	split = (rbuf->pread + len > rbuf->size) ? rbuf->size - rbuf->pread : 0;
	if (split > 0) {
		saa716x_ff_phi_write_fifo(saa716x_ff, rbuf->data + rbuf->pread,
					  split);
		todo -= split;
		rbuf->pread = 0;
	}
	saa716x_ff_phi_write_fifo(saa716x_ff, rbuf->data + rbuf->pread, todo);

	rbuf->pread = (rbuf->pread + todo) % rbuf->size;
}

static void fifo_worker(struct work_struct *work)
{
	struct sti7109_dev *sti7109 =
			 container_of(work, struct sti7109_dev, fifo_work);
	struct saa716x_ff_dev *saa716x_ff =
			 container_of(sti7109, struct saa716x_ff_dev, sti7109);
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	u32 fifoStat;
	u16 fifoSize;
	u16 fifoUsage;
	u16 fifoFree;
	int ringbuffer_avail;
	int len;

	if (sti7109->tsout_stat == TSOUT_STAT_RESET)
		return;

	fifoStat = SAA716x_EPRD(PHI_1, FPGA_ADDR_FIFO_STAT);
	fifoSize = (u16) (fifoStat >> 16);
	fifoUsage = (u16) fifoStat;
	fifoFree = fifoSize - fifoUsage;
	ringbuffer_avail = dvb_ringbuffer_avail(&sti7109->tsout);

	len = (ringbuffer_avail < fifoFree) ? ringbuffer_avail : fifoFree;
	len = (len / 32) * 32;
	if (len) {
		ringbuffer_read_tofifo(&sti7109->tsout, saa716x_ff, len);
		wake_up(&sti7109->tsout.queue);
	}

	ringbuffer_avail = dvb_ringbuffer_avail(&sti7109->tsout);
	if (ringbuffer_avail < TSOUT_LEVEL_LOW) {
		spin_lock(&sti7109->tsout.lock);
		if (sti7109->tsout_stat != TSOUT_STAT_RESET)
			sti7109->tsout_stat = TSOUT_STAT_WAIT;
		spin_unlock(&sti7109->tsout.lock);
		return;
	}

	spin_lock(&sti7109->tsout.lock);
	if (sti7109->tsout_stat != TSOUT_STAT_RESET) {
		/* reenable fifo interrupt */
		sti7109->tsout_stat = TSOUT_STAT_RUN;
		SAA716x_EPWR(PHI_1, FPGA_ADDR_FIFO_CTRL,
			     FPGA_FIFO_CTRL_IE | FPGA_FIFO_CTRL_RUN);
	}
	spin_unlock(&sti7109->tsout.lock);
}

static void video_vip_worker(unsigned long data)
{
	struct saa716x_vip_stream_port *vip_entry =
				 (struct saa716x_vip_stream_port *)data;
	struct saa716x_dev *saa716x = vip_entry->saa716x;
	u32 vip_index;
	u32 write_index;

	vip_index = vip_entry->dma_channel[0];
	if (vip_index != 0) {
		pci_err(saa716x->pdev, "%s: unexpected channel %u",
		       __func__, vip_entry->dma_channel[0]);
		return;
	}

	write_index = saa716x_vip_get_write_index(saa716x, vip_index);
	if (write_index < 0)
		return;

	pci_dbg(saa716x->pdev, "dma buffer = %d", write_index);

	if (write_index == vip_entry->read_index) {
		pci_dbg(saa716x->pdev,
			"%s: called but nothing to do", __func__);
		return;
	}

	do {
		pci_dma_sync_sg_for_cpu(saa716x->pdev,
			vip_entry->dma_buf[0][vip_entry->read_index].sg_list,
			vip_entry->dma_buf[0][vip_entry->read_index].list_len,
			PCI_DMA_FROMDEVICE);
		if (vip_entry->dual_channel) {
			pci_dma_sync_sg_for_cpu(saa716x->pdev,
			  vip_entry->dma_buf[1][vip_entry->read_index].sg_list,
			  vip_entry->dma_buf[1][vip_entry->read_index].list_len,
			  PCI_DMA_FROMDEVICE);
		}

		vip_entry->read_index = (vip_entry->read_index + 1) & 7;
	} while (write_index != vip_entry->read_index);
}

static int video_vip_get_stream_params(struct vip_stream_params *params,
				       u32 mode)
{
	switch (mode) {
	case 4:  /* 1280x720p60 */
	case 19: /* 1280x720p50 */
		params->bits		= 16;
		params->samples		= 1280;
		params->lines		= 720;
		params->pitch		= 1280 * 2;
		params->offset_x	= 32;
		params->offset_y	= 30;
		params->stream_flags	= VIP_HD;
		break;

	case 5:  /* 1920x1080i60 */
	case 20: /* 1920x1080i50 */
		params->bits		= 16;
		params->samples		= 1920;
		params->lines		= 1080;
		params->pitch		= 1920 * 2;
		params->offset_x	= 0;
		params->offset_y	= 20;
		params->stream_flags	= VIP_ODD_FIELD
					| VIP_EVEN_FIELD
					| VIP_INTERLACED
					| VIP_HD
					| VIP_NO_SCALER;
		break;

	case 32: /* 1920x1080p24 */
	case 33: /* 1920x1080p25 */
	case 34: /* 1920x1080p30 */
		params->bits		= 16;
		params->samples		= 1920;
		params->lines		= 1080;
		params->pitch		= 1920 * 2;
		params->offset_x	= 0;
		params->offset_y	= 0;
		params->stream_flags	= VIP_HD;
		break;

	default:
		return -1;
	}
	return 0;
}

static ssize_t video_vip_read(struct sti7109_dev *sti7109,
			      struct vip_stream_params *stream_params,
			      char __user *buf, size_t count)
{
	struct saa716x_ff_dev *saa716x_ff =
			 container_of(sti7109, struct saa716x_ff_dev, sti7109);
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct v4l2_pix_format pix_format;
	int one_shot = 0;
	size_t num_bytes;
	size_t copy_bytes;
	u32 read_index;
	u8 *data;
	int err = 0;

	if (sti7109->video_capture == VIDEO_CAPTURE_ONE_SHOT)
		one_shot = 1;

	/* put a v4l2_pix_format header at the beginning of the returned data */
	memset(&pix_format, 0, sizeof(pix_format));
	pix_format.width	= stream_params->samples;
	pix_format.height	= stream_params->lines;
	pix_format.pixelformat	= V4L2_PIX_FMT_UYVY;
	pix_format.bytesperline	= stream_params->pitch;
	pix_format.sizeimage	= stream_params->lines * stream_params->pitch;

	if (count > (sizeof(pix_format) + pix_format.sizeimage))
		count = sizeof(pix_format) + pix_format.sizeimage;

	if (count < sizeof(pix_format)) {
		err = -EFAULT;
		goto out;
	}

	saa716x_vip_start(saa716x, 0, one_shot, stream_params);
	/*
	 * Sleep long enough to be sure to capture at least one frame.
	 * TODO: Change this in a way that it just waits the required time.
	 */
	msleep(100);
	saa716x_vip_stop(saa716x, 0);

	read_index = saa716x->vip[0].read_index;
	if ((stream_params->stream_flags & VIP_INTERLACED) &&
	    (stream_params->stream_flags & VIP_ODD_FIELD) &&
	    (stream_params->stream_flags & VIP_EVEN_FIELD)) {
		read_index = read_index & ~1;
		read_index = (read_index + 7) & 7;
		read_index = read_index / 2;
	} else {
		read_index = (read_index + 7) & 7;
	}

	if (copy_to_user((void __user *)buf, &pix_format, sizeof(pix_format))) {
		err = -EFAULT;
		goto out;
	}
	num_bytes = sizeof(pix_format);

	copy_bytes = count - num_bytes;
	if (copy_bytes > (SAA716x_PAGE_SIZE / 8 * SAA716x_PAGE_SIZE))
		copy_bytes = SAA716x_PAGE_SIZE / 8 * SAA716x_PAGE_SIZE;
	data = (u8 *)saa716x->vip[0].dma_buf[0][read_index].mem_virt;
	if (copy_to_user((void __user *)(buf + num_bytes), data, copy_bytes)) {
		err = -EFAULT;
		goto out;
	}
	num_bytes += copy_bytes;
	if (saa716x->vip[0].dual_channel &&
	    count - num_bytes > 0) {
		copy_bytes = count - num_bytes;
		if (copy_bytes > (SAA716x_PAGE_SIZE / 8 * SAA716x_PAGE_SIZE))
			copy_bytes = SAA716x_PAGE_SIZE / 8 * SAA716x_PAGE_SIZE;
		data = (u8 *)saa716x->vip[0].dma_buf[1][read_index].mem_virt;
		if (copy_to_user((void __user *)(buf + num_bytes), data,
				 copy_bytes)) {
			err = -EFAULT;
			goto out;
		}
		num_bytes += copy_bytes;
	}

	return num_bytes;

out:
	return err;
}

static ssize_t dvb_video_read(struct file *file, char __user *buf,
			      size_t count, loff_t *ppos)
{
	struct dvb_device *dvbdev	= file->private_data;
	struct sti7109_dev *sti7109	= dvbdev->priv;
	struct vip_stream_params stream_params;
	ssize_t ret = -ENODATA;

	if ((file->f_flags & O_ACCMODE) == O_WRONLY)
		return -EPERM;

	mutex_lock(&sti7109->video_lock);

	if (sti7109->video_capture != VIDEO_CAPTURE_OFF) {
		if (video_vip_get_stream_params(&stream_params,
						sti7109->video_format) == 0) {
			ret = video_vip_read(sti7109, &stream_params,
					     buf, count);
		}
	}

	mutex_unlock(&sti7109->video_lock);
	return ret;
}

#define FREE_COND_TS (dvb_ringbuffer_free(&sti7109->tsout) >= TS_SIZE)

static ssize_t dvb_video_write(struct file *file, const char __user *buf,
			       size_t count, loff_t *ppos)
{
	struct dvb_device *dvbdev = file->private_data;
	struct sti7109_dev *sti7109 = dvbdev->priv;
	struct saa716x_ff_dev *saa716x_ff =
			 container_of(sti7109, struct saa716x_ff_dev, sti7109);
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	unsigned long todo = count;
	int ringbuffer_avail;

	if ((file->f_flags & O_ACCMODE) == O_RDONLY)
		return -EPERM;

	if (sti7109->tsout_stat == TSOUT_STAT_RESET)
		return count;

	if ((file->f_flags & O_NONBLOCK) && !FREE_COND_TS)
		return -EWOULDBLOCK;

	while (todo >= TS_SIZE) {
		if (!FREE_COND_TS) {
			if (file->f_flags & O_NONBLOCK)
				break;
			if (wait_event_interruptible(sti7109->tsout.queue,
						     FREE_COND_TS))
				break;
		}
		ringbuffer_write_user(&sti7109->tsout, buf, TS_SIZE);
		todo -= TS_SIZE;
		buf += TS_SIZE;
	}

	ringbuffer_avail = dvb_ringbuffer_avail(&sti7109->tsout);
	spin_lock(&sti7109->tsout.lock);
	if ((sti7109->tsout_stat == TSOUT_STAT_FILL) &&
	    (ringbuffer_avail > TSOUT_LEVEL_FILL)) {
		sti7109->tsout_stat = TSOUT_STAT_RUN;
		SAA716x_EPWR(PHI_1, FPGA_ADDR_FIFO_CTRL,
			     FPGA_FIFO_CTRL_IE | FPGA_FIFO_CTRL_RUN);
	} else if ((sti7109->tsout_stat == TSOUT_STAT_WAIT) &&
		   (ringbuffer_avail > TSOUT_LEVEL_HIGH)) {
		sti7109->tsout_stat = TSOUT_STAT_RUN;
		queue_work(sti7109->fifo_workq, &sti7109->fifo_work);
	}
	spin_unlock(&sti7109->tsout.lock);

	return count - todo;
}

static unsigned int dvb_video_poll(struct file *file, poll_table *wait)
{
	struct dvb_device *dvbdev	= file->private_data;
	struct sti7109_dev *sti7109	= dvbdev->priv;
	unsigned int mask = 0;

	if ((file->f_flags & O_ACCMODE) != O_RDONLY) {
		poll_wait(file, &sti7109->tsout.queue, wait);

		if (FREE_COND_TS)
			mask |= (POLLOUT | POLLWRNORM);
	}

	return mask;
}

static int do_dvb_video_ioctl(struct dvb_device *dvbdev,
			      unsigned int cmd, void *parg)
{
	struct sti7109_dev *sti7109  = dvbdev->priv;
	struct saa716x_ff_dev *saa716x_ff =
			 container_of(sti7109, struct saa716x_ff_dev, sti7109);
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	int ret = 0;

	switch (cmd) {
	case VIDEO_SELECT_SOURCE:
	{
		video_stream_source_t stream_source;

		stream_source = (video_stream_source_t) parg;
		if (stream_source == VIDEO_SOURCE_DEMUX) {
			/* stop and reset FIFO 1 */
			spin_lock(&sti7109->tsout.lock);
			SAA716x_EPWR(PHI_1, FPGA_ADDR_FIFO_CTRL,
				     FPGA_FIFO_CTRL_RESET);
			sti7109->tsout_stat = TSOUT_STAT_RESET;
			spin_unlock(&sti7109->tsout.lock);
			break;
		}
		/* fall through */
	}
	case VIDEO_CLEAR_BUFFER:
	{
		/* reset FIFO 1 */
		spin_lock(&sti7109->tsout.lock);
		SAA716x_EPWR(PHI_1, FPGA_ADDR_FIFO_CTRL, FPGA_FIFO_CTRL_RESET);
		sti7109->tsout_stat = TSOUT_STAT_RESET;
		spin_unlock(&sti7109->tsout.lock);
		msleep(50);
		cancel_work_sync(&sti7109->fifo_work);
		/* start FIFO 1 */
		spin_lock(&sti7109->tsout.lock);
		SAA716x_EPWR(PHI_1, FPGA_ADDR_FIFO_CTRL, 0);
		/* TODO convert to dvb_ringbuffer_reset(&sti7109->tsout); */
		sti7109->tsout.pread = sti7109->tsout.pwrite = 0;
		sti7109->tsout_stat = TSOUT_STAT_FILL;
		spin_unlock(&sti7109->tsout.lock);
		wake_up(&sti7109->tsout.queue);
		break;
	}
	case VIDEO_GET_PTS:
	{
		*(u64 *)parg = sti7109->video_pts;
		break;
	}
	case VIDEO_GET_SIZE:
	{
		ret = sti7109_cmd_get_video_format(sti7109,
						   (video_size_t *) parg);
		break;
	}
	default:
		ret = -ENOIOCTLCMD;
		break;
	}
	return ret;
}

static long dvb_video_ioctl(struct file *file, unsigned int cmd,
			    unsigned long arg)
{
	struct dvb_device *dvbdev = file->private_data;

	if (!dvbdev)
		return -ENODEV;

	return saa716x_usercopy(dvbdev, cmd, arg, do_dvb_video_ioctl);
}

static const struct file_operations dvb_video_fops = {
	.read		= dvb_video_read,
	.write		= dvb_video_write,
	.unlocked_ioctl	= dvb_video_ioctl,
	.open		= dvb_generic_open,
	.release	= dvb_generic_release,
	.poll		= dvb_video_poll,
};

static const struct dvb_device dvbdev_video = {
	.priv		= NULL,
	.users		= 5,
	.readers	= 5,
	.writers	= 1,
	.fops		= &dvb_video_fops,
	.kernel_ioctl	= NULL,
};

static int saa716x_ff_video_exit(struct saa716x_ff_dev *saa716x_ff)
{
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	if (sti7109->video_capture != VIDEO_CAPTURE_OFF)
		saa716x_vip_exit(saa716x, 0);

	cancel_work_sync(&sti7109->fifo_work);
	destroy_workqueue(sti7109->fifo_workq);
	dvb_unregister_device(sti7109->video_dev);
	return 0;
}

static int saa716x_ff_video_init(struct saa716x_ff_dev *saa716x_ff)
{
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct saa716x_adapter *saa716x_adap = saa716x->saa716x_adap;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	dvb_ringbuffer_init(&sti7109->tsout, sti7109->iobuf, TSOUT_LEN);

	dvb_register_device(&saa716x_adap->dvb_adapter,
			    &sti7109->video_dev,
			    &dvbdev_video,
			    sti7109,
			    DVB_DEVICE_VIDEO,
			    0);

	sti7109->fifo_workq = alloc_workqueue("saa716x_fifo_wq", WQ_UNBOUND, 1);
	INIT_WORK(&sti7109->fifo_work, fifo_worker);

	if (sti7109->video_capture != VIDEO_CAPTURE_OFF) {
		/* enable FGPI0 for video capture inputs */
		SAA716x_EPWR(GREG, GREG_VI_CTRL, 0x2C689F04);
		SAA716x_EPWR(GREG, GREG_VIDEO_IN_CTRL, 0xC0);
		saa716x_vip_init(saa716x, 0, video_vip_worker);
	}

	return 0;
}

static int saa716x_ff_pci_probe(struct pci_dev *pdev,
				const struct pci_device_id *pci_id)
{
	struct saa716x_ff_dev *saa716x_ff;
	struct saa716x_dev *saa716x;
	struct sti7109_dev *sti7109;
	int err = 0;
	u32 value;
	unsigned long timeout;

	saa716x_ff = kzalloc(sizeof(struct saa716x_ff_dev), GFP_KERNEL);
	if (saa716x_ff == NULL) {
		err = -ENOMEM;
		goto fail0;
	}

	saa716x = &saa716x_ff->saa716x;
	sti7109 = &saa716x_ff->sti7109;

	saa716x->pdev = pdev;
	saa716x->module = THIS_MODULE;
	saa716x->config = (struct saa716x_config *) pci_id->driver_data;

	err = saa716x_pci_init(saa716x);
	if (err) {
		pci_err(saa716x->pdev, "PCI Initialization failed");
		goto fail1;
	}

	err = saa716x_cgu_init(saa716x);
	if (err) {
		pci_err(saa716x->pdev, "CGU Init failed");
		goto fail1;
	}

	SAA716x_EPWR(PHI_0, PHI_SW_RST, 1);

	err = saa716x_jetpack_init(saa716x);
	if (err) {
		pci_err(saa716x->pdev, "Jetpack core initialization failed");
		goto fail2;
	}

	err = saa716x_i2c_init(saa716x);
	if (err) {
		pci_err(saa716x->pdev, "I2C Initialization failed");
		goto fail3;
	}

	saa716x_gpio_init(saa716x);

	sti7109->iobuf = vmalloc(TSOUT_LEN + MAX_DATA_LEN);
	if (!sti7109->iobuf)
		goto fail4;

	err = saa716x_ff_phi_init(saa716x_ff);
	if (err)
		goto fail4;

	sti7109_cmd_init(sti7109);

	sti7109->video_capture = video_capture;
	mutex_init(&sti7109->video_lock);

	saa716x_gpio_set_output(saa716x, TT_PREMIUM_GPIO_POWER_ENABLE);
	saa716x_gpio_set_output(saa716x, TT_PREMIUM_GPIO_RESET_BACKEND);
	saa716x_gpio_set_output(saa716x, TT_PREMIUM_GPIO_FPGA_CS0);
	saa716x_gpio_set_mode(saa716x, TT_PREMIUM_GPIO_FPGA_CS0, 1);
	saa716x_gpio_set_output(saa716x, TT_PREMIUM_GPIO_FPGA_CS1);
	saa716x_gpio_set_mode(saa716x, TT_PREMIUM_GPIO_FPGA_CS1, 1);
	saa716x_gpio_set_output(saa716x, TT_PREMIUM_GPIO_FPGA_CS2);
	saa716x_gpio_set_mode(saa716x, TT_PREMIUM_GPIO_FPGA_CS2, 1);
	saa716x_gpio_set_output(saa716x, TT_PREMIUM_GPIO_FPGA_PROGRAMN);
	saa716x_gpio_set_input(saa716x, TT_PREMIUM_GPIO_FPGA_DONE);
	saa716x_gpio_set_input(saa716x, TT_PREMIUM_GPIO_FPGA_INITN);

	/* hold ST in reset */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_RESET_BACKEND, 0);

	/* enable board power */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_POWER_ENABLE, 1);
	msleep(100);

	err = saa716x_ff_fpga_init(saa716x_ff);
	if (err) {
		pci_err(saa716x->pdev, "FPGA Initialization failed");
		goto fail5;
	}

	/* configure TS muxer */
	if (sti7109->fpga_version < 0x110) {
		/* select FIFO 1 for TS mux 3 */
		SAA716x_EPWR(PHI_1, FPGA_ADDR_TSR_MUX3, 4);
	} else {
		/* select FIFO 1 for TS mux 3 */
		SAA716x_EPWR(PHI_1, FPGA_ADDR_TSR_MUX3, 1);
	}

	/* enable interrupts from ST7109 -> PC */
	SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICTRL, 0x3);

	value = SAA716x_EPRD(MSI, MSI_CONFIG33);
	value &= 0xFCFFFFFF;
	value |= MSI_INT_POL_EDGE_FALL;
	SAA716x_EPWR(MSI, MSI_CONFIG33, value);
	SAA716x_EPWR(MSI, MSI_INT_ENA_SET_H, MSI_INT_EXTINT_0);

	/* enable tuner reset */
	SAA716x_EPWR(PHI_1, FPGA_ADDR_PIO_CTRL, 0);
	msleep(50);
	/* disable tuner reset */
	SAA716x_EPWR(PHI_1, FPGA_ADDR_PIO_CTRL, 1);

	err = saa716x_ff_st7109_init(saa716x_ff);
	if (err) {
		pci_err(saa716x->pdev, "STi7109 initialization failed");
		goto fail5;
	}

	err = saa716x_dvb_init(saa716x);
	if (err) {
		pci_err(saa716x->pdev, "DVB initialization failed");
		goto fail6;
	}

	/* wait a maximum of 10 seconds for the STi7109 to boot */
	timeout = 10 * HZ;
	timeout = wait_event_interruptible_timeout(sti7109->boot_finish_wq,
						   sti7109->boot_finished == 1,
						   timeout);

	if (timeout == -ERESTARTSYS || sti7109->boot_finished == 0) {
		if (timeout == -ERESTARTSYS) {
			/* a signal arrived */
			goto fail6;
		}
		pci_err(saa716x->pdev, "timed out waiting for boot finish");
		err = -1;
		goto fail6;
	}
	pci_dbg(saa716x->pdev, "STi7109 finished booting");

	err = saa716x_ff_video_init(saa716x_ff);
	if (err) {
		pci_err(saa716x->pdev, "VIDEO initialization failed");
		goto fail7;
	}

	err = saa716x_ff_audio_init(saa716x_ff);
	if (err) {
		pci_err(saa716x->pdev, "AUDIO initialization failed");
		goto fail8;
	}

	err = saa716x_ff_osd_init(saa716x_ff);
	if (err) {
		pci_err(saa716x->pdev, "OSD initialization failed");
		goto fail9;
	}

	err = sti7109_cmd_get_fw_version(sti7109, &sti7109->fw_version);
	if (!err) {
		pci_info(saa716x->pdev, "firmware version %d.%d.%d",
			(sti7109->fw_version >> 16) & 0xFF,
			(sti7109->fw_version >> 8) & 0xFF,
			sti7109->fw_version & 0xFF);
	}

	err = saa716x_ir_init(saa716x_ff);
	if (err)
		goto fail9;

	return 0;

fail9:
	saa716x_ff_osd_exit(saa716x_ff);
fail8:
	saa716x_ff_audio_exit(saa716x_ff);
fail7:
	saa716x_ff_video_exit(saa716x_ff);
fail6:
	saa716x_dvb_exit(saa716x);
fail5:
	SAA716x_EPWR(MSI, MSI_INT_ENA_CLR_H, MSI_INT_EXTINT_0);

	/* disable board power */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_POWER_ENABLE, 0);

	vfree(sti7109->iobuf);
fail4:
	saa716x_ff_phi_exit(saa716x_ff);
fail3:
	saa716x_i2c_exit(saa716x);
fail2:
	saa716x_pci_exit(saa716x);
fail1:
	kfree(saa716x_ff);
fail0:
	return err;
}

static void saa716x_ff_pci_remove(struct pci_dev *pdev)
{
	struct saa716x_ff_dev *saa716x_ff = pci_get_drvdata(pdev);
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;

	saa716x_ir_exit(saa716x_ff);

	saa716x_ff_osd_exit(saa716x_ff);

	saa716x_ff_audio_exit(saa716x_ff);

	saa716x_ff_video_exit(saa716x_ff);

	saa716x_dvb_exit(saa716x);

	SAA716x_EPWR(MSI, MSI_INT_ENA_CLR_H, MSI_INT_EXTINT_0);

	/* disable board power */
	saa716x_gpio_write(saa716x, TT_PREMIUM_GPIO_POWER_ENABLE, 0);

	vfree(sti7109->iobuf);

	saa716x_ff_phi_exit(saa716x_ff);

	saa716x_i2c_exit(saa716x);
	saa716x_pci_exit(saa716x);
	kfree(saa716x_ff);
}

static irqreturn_t saa716x_ff_pci_irq(int irq, void *dev_id)
{
	struct saa716x_ff_dev *saa716x_ff = (struct saa716x_ff_dev *) dev_id;
	struct saa716x_dev *saa716x = &saa716x_ff->saa716x;
	struct sti7109_dev *sti7109 = &saa716x_ff->sti7109;
	u32 msiStatusL;
	u32 msiStatusH;
	u32 phiISR;

	msiStatusL = SAA716x_EPRD(MSI, MSI_INT_STATUS_L);
	SAA716x_EPWR(MSI, MSI_INT_STATUS_CLR_L, msiStatusL);
	msiStatusH = SAA716x_EPRD(MSI, MSI_INT_STATUS_H);
	SAA716x_EPWR(MSI, MSI_INT_STATUS_CLR_H, msiStatusH);

	if (msiStatusL & MSI_INT_TAGACK_VI0_0)
		tasklet_schedule(&saa716x->vip[0].tasklet);
	if (msiStatusL & MSI_INT_TAGACK_FGPI_2)
		tasklet_schedule(&saa716x->fgpi[2].tasklet);
	if (msiStatusL & MSI_INT_TAGACK_FGPI_3)
		tasklet_schedule(&saa716x->fgpi[3].tasklet);

	if (msiStatusH & MSI_INT_I2CINT_0) {
		saa716x->i2c[0].i2c_op = 0;
		wake_up(&saa716x->i2c[0].i2c_wq);
	}
	if (msiStatusH & MSI_INT_I2CINT_1) {
		saa716x->i2c[1].i2c_op = 0;
		wake_up(&saa716x->i2c[1].i2c_wq);
	}

	if (msiStatusH & MSI_INT_EXTINT_0) {

		phiISR = SAA716x_EPRD(PHI_1, FPGA_ADDR_EMI_ISR);
		if (phiISR & ISR_CMD_MASK) {

			u32 value;
			u32 length;

			value = SAA716x_EPRD(PHI_1, ADDR_CMD_DATA);
			value = __cpu_to_be32(value);
			length = (value >> 16) + 2;

			if (length > MAX_RESULT_LEN) {
				pci_err(saa716x->pdev, "CMD length %d > %d",
					length, MAX_RESULT_LEN);
				length = MAX_RESULT_LEN;
			}

			saa716x_ff_phi_read(saa716x_ff, ADDR_CMD_DATA,
					    sti7109->result_data, length);
			sti7109->result_len = length;
			sti7109->result_avail = 1;
			wake_up(&sti7109->result_avail_wq);

			phiISR &= ~ISR_CMD_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR, ISR_CMD_MASK);
		}

		if (phiISR & ISR_READY_MASK) {
			sti7109->cmd_ready = 1;
			wake_up(&sti7109->cmd_ready_wq);
			phiISR &= ~ISR_READY_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR, ISR_READY_MASK);
		}

		if (phiISR & ISR_OSD_CMD_MASK) {

			u32 value;
			u32 length;

			value = SAA716x_EPRD(PHI_1, ADDR_OSD_CMD_DATA);
			value = __cpu_to_be32(value);
			length = (value >> 16) + 2;

			if (length > MAX_RESULT_LEN) {
				pci_err(saa716x->pdev, "OSD CMD length %d > %d",
					length, MAX_RESULT_LEN);
				length = MAX_RESULT_LEN;
			}

			saa716x_ff_phi_read(saa716x_ff, ADDR_OSD_CMD_DATA,
					    sti7109->osd_result_data, length);
			sti7109->osd_result_len = length;
			sti7109->osd_result_avail = 1;
			wake_up(&sti7109->osd_result_avail_wq);

			phiISR &= ~ISR_OSD_CMD_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_OSD_CMD_MASK);
		}

		if (phiISR & ISR_OSD_READY_MASK) {
			sti7109->osd_cmd_ready = 1;
			wake_up(&sti7109->osd_cmd_ready_wq);
			phiISR &= ~ISR_OSD_READY_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_OSD_READY_MASK);
		}

		if (phiISR & ISR_BLOCK_MASK) {
			sti7109->block_done = 1;
			wake_up(&sti7109->block_done_wq);
			phiISR &= ~ISR_BLOCK_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR, ISR_BLOCK_MASK);
		}

		if (phiISR & ISR_DATA_MASK) {
			sti7109->data_ready = 1;
			wake_up(&sti7109->data_ready_wq);
			phiISR &= ~ISR_DATA_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR, ISR_DATA_MASK);
		}

		if (phiISR & ISR_BOOT_FINISH_MASK) {
			sti7109->boot_finished = 1;
			wake_up(&sti7109->boot_finish_wq);
			phiISR &= ~ISR_BOOT_FINISH_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_BOOT_FINISH_MASK);
		}

		if (phiISR & ISR_AUDIO_PTS_MASK) {
			u8 data[8];

			saa716x_ff_phi_read(saa716x_ff, ADDR_AUDIO_PTS,
					    data, 8);
			sti7109->audio_pts = (((u64) data[3] & 0x01) << 32)
					    | ((u64) data[4] << 24)
					    | ((u64) data[5] << 16)
					    | ((u64) data[6] << 8)
					    | ((u64) data[7]);

			phiISR &= ~ISR_AUDIO_PTS_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_AUDIO_PTS_MASK);
		}

		if (phiISR & ISR_VIDEO_PTS_MASK) {
			u8 data[8];

			saa716x_ff_phi_read(saa716x_ff, ADDR_VIDEO_PTS,
					    data, 8);
			sti7109->video_pts = (((u64) data[3] & 0x01) << 32)
					    | ((u64) data[4] << 24)
					    | ((u64) data[5] << 16)
					    | ((u64) data[6] << 8)
					    | ((u64) data[7]);

			phiISR &= ~ISR_VIDEO_PTS_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_VIDEO_PTS_MASK);
		}

		if (phiISR & ISR_CURRENT_STC_MASK) {
			u8 data[8];

			saa716x_ff_phi_read(saa716x_ff, ADDR_CURRENT_STC,
					    data, 8);
			sti7109->current_stc = (((u64) data[3] & 0x01) << 32)
					      | ((u64) data[4] << 24)
					      | ((u64) data[5] << 16)
					      | ((u64) data[6] << 8)
					      | ((u64) data[7]);

			phiISR &= ~ISR_CURRENT_STC_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_CURRENT_STC_MASK);
		}

		if (phiISR & ISR_REMOTE_EVENT_MASK) {
			u8 data[4];
			u32 remote_event;

			saa716x_ff_phi_read(saa716x_ff, ADDR_REMOTE_EVENT,
					    data, 4);
			remote_event = (data[3] << 24)
				     | (data[2] << 16)
				     | (data[1] << 8)
				     | (data[0]);
			memset(data, 0, sizeof(data));
			saa716x_ff_phi_write(saa716x_ff, ADDR_REMOTE_EVENT,
					     data, 4);

			phiISR &= ~ISR_REMOTE_EVENT_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_REMOTE_EVENT_MASK);

			if (remote_event == 0) {
				pci_err(saa716x->pdev, "REMOTE EVENT ignored");
			} else {
				pci_dbg(saa716x->pdev, "REMOTE EVENT: %X",
					remote_event);
				saa716x_ir_handler(saa716x_ff, remote_event);
			}
		}

		if (phiISR & ISR_DVO_FORMAT_MASK) {
			u8 data[4];
			u32 format;

			saa716x_ff_phi_read(saa716x_ff, ADDR_DVO_FORMAT,
					    data, 4);
			format = (data[0] << 24)
			       | (data[1] << 16)
			       | (data[2] << 8)
			       | (data[3]);

			phiISR &= ~ISR_DVO_FORMAT_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_DVO_FORMAT_MASK);

			pci_dbg(saa716x->pdev, "DVO FORMAT CHANGE: %u", format);
			sti7109->video_format = format;
		}

		if (phiISR & ISR_LOG_MESSAGE_MASK) {
			char message[SIZE_LOG_MESSAGE_DATA];

			saa716x_ff_phi_read(saa716x_ff, ADDR_LOG_MESSAGE,
					    message, SIZE_LOG_MESSAGE_DATA);

			phiISR &= ~ISR_LOG_MESSAGE_MASK;
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR,
				     ISR_LOG_MESSAGE_MASK);

			pci_dbg(saa716x->pdev, "LOG MESSAGE: %.*s",
				SIZE_LOG_MESSAGE_DATA, message);
		}

		if (phiISR & ISR_FIFO1_EMPTY_MASK) {
			/* clear FPGA_FIFO_CTRL_IE */
			SAA716x_EPWR(PHI_1, FPGA_ADDR_FIFO_CTRL,
				     FPGA_FIFO_CTRL_RUN);
			queue_work(sti7109->fifo_workq, &sti7109->fifo_work);
			phiISR &= ~ISR_FIFO1_EMPTY_MASK;
		}

		if (phiISR) {
			pci_dbg(saa716x->pdev, "unknown interrupt source");
			SAA716x_EPWR(PHI_1, FPGA_ADDR_EMI_ICLR, phiISR);
		}
	}

	return IRQ_HANDLED;
}

#define SAA716x_MODEL_S2_6400_DUAL	"Technotrend S2 6400 Dual S2 Premium"
#define SAA716x_DEV_S2_6400_DUAL	"2x DVB-S/S2 + Hardware decode"

static const struct isl6423_config tt6400_isl6423_config[2] = {
	{
		.current_max		= SEC_CURRENT_515m,
		.curlim			= SEC_CURRENT_LIM_ON,
		.mod_extern		= 1,
		.addr			= 0x09,
	},
	{
		.current_max		= SEC_CURRENT_515m,
		.curlim			= SEC_CURRENT_LIM_ON,
		.mod_extern		= 1,
		.addr			= 0x08,
	}
};


static int saa716x_s26400_frontend_attach(struct saa716x_adapter *adapter,
					  int count)
{
	struct saa716x_dev *saa716x = adapter->saa716x;
	struct saa716x_i2c *i2c = saa716x->i2c;
	struct i2c_adapter *i2c_adapter = &i2c[SAA716x_I2C_BUS_A].i2c_adapter;
	struct stv6110x_devctl *ctl;
	int ret = 0;

	pci_dbg(saa716x->pdev, "Adapter (%d) SAA716x frontend Init", count);
	pci_dbg(saa716x->pdev, "Adapter (%d) Device ID=%02x", count,
		saa716x->pdev->subsystem_device);

	if (count == 0 || count == 1) {
		struct stv090x_config *stv090x_config = NULL;
		struct stv6110x_config *stv6110x_config = NULL;

		stv090x_config = kzalloc(sizeof(*stv090x_config), GFP_KERNEL);
		if (!stv090x_config) {
			ret = -ENOMEM;
			goto exit;
		}

		stv6110x_config = kzalloc(sizeof(*stv6110x_config), GFP_KERNEL);
		if (!stv6110x_config) {
			ret = -ENOMEM;
			goto exit;
		}

		stv090x_config->device		= STV0900;
		stv090x_config->demod_mode	= STV090x_DUAL;
		stv090x_config->clk_mode	= STV090x_CLK_EXT;
		stv090x_config->demod =
			(count == 1) ? STV090x_DEMODULATOR_1 : STV090x_DEMODULATOR_0;
		stv090x_config->demod		= STV090x_DEMODULATOR_1;
		stv090x_config->xtal		= 13500000;
		stv090x_config->address		= 0x68;
		stv090x_config->ts1_mode	= STV090x_TSMODE_SERIAL_CONTINUOUS;
		stv090x_config->ts2_mode	= STV090x_TSMODE_SERIAL_CONTINUOUS;
		stv090x_config->ts1_clk		= 135000000;
		stv090x_config->ts2_clk		= 135000000;
		stv090x_config->repeater_level	= STV090x_RPTLEVEL_16;

		adapter->fe_config = stv090x_config;

		adapter->i2c_client_demod =
			dvb_module_probe("stv090x", NULL,
					 i2c_adapter,
					 stv090x_config->address,
					 stv090x_config);
		adapter->fe =
			stv090x_config->get_dvb_frontend(adapter->i2c_client_demod);

		if (!adapter->fe) {
			ret = -ENODEV;
			goto exit;
		}

		stv6110x_config->addr		= 0x60;
		stv6110x_config->refclk		= 27000000;
		stv6110x_config->clk_div	= 2;
		stv6110x_config->frontend	= adapter->fe;

		adapter->ctl_config = stv6110x_config;

		adapter->i2c_client_tuner =
		dvb_module_probe("stv6110x", NULL,
				 i2c_adapter,
				 stv6110x_config->addr,
				 stv6110x_config);

		ctl = stv6110x_config->get_devctl(adapter->i2c_client_tuner);

		stv090x_config->tuner_init		= ctl->tuner_init;
		stv090x_config->tuner_sleep		= ctl->tuner_sleep;
		stv090x_config->tuner_set_mode		= ctl->tuner_set_mode;
		stv090x_config->tuner_set_frequency	= ctl->tuner_set_frequency;
		stv090x_config->tuner_get_frequency	= ctl->tuner_get_frequency;
		stv090x_config->tuner_set_bandwidth	= ctl->tuner_set_bandwidth;
		stv090x_config->tuner_get_bandwidth	= ctl->tuner_get_bandwidth;
		stv090x_config->tuner_set_bbgain	= ctl->tuner_set_bbgain;
		stv090x_config->tuner_get_bbgain	= ctl->tuner_get_bbgain;
		stv090x_config->tuner_set_refclk	= ctl->tuner_set_refclk;
		stv090x_config->tuner_get_status	= ctl->tuner_get_status;

		if (count == 1) {
			/* Call the init function once to initialize
			 * tuner's clock output divider and demod's
			 * master clock.
			 * The second tuner drives the STV0900, so
			 * call it only for adapter 1
			 */
			if (adapter->fe->ops.init)
				adapter->fe->ops.init(adapter->fe);
		}

		dvb_attach(isl6423_attach,
			   adapter->fe,
			   i2c_adapter,
			   &tt6400_isl6423_config[count]);

		if (adapter->fe->ops.sleep)
			adapter->fe->ops.sleep(adapter->fe);

		return ret;
	}

exit:
	pci_err(saa716x->pdev, "Frontend attach failed");
	return ret;
}

static const struct saa716x_config saa716x_s26400_config = {
	.model_name		= SAA716x_MODEL_S2_6400_DUAL,
	.dev_type		= SAA716x_DEV_S2_6400_DUAL,
	.adapters		= 2,
	.frontend_attach	= saa716x_s26400_frontend_attach,
	.irq_handler		= saa716x_ff_pci_irq,
	.i2c_rate		= SAA716x_I2C_RATE_100,
	.i2c_mode		= SAA716x_I2C_MODE_IRQ_BUFFERED,

	.adap_config		= {
		{
			/* Adapter 0 */
			.ts_vp   = 1,
			.ts_fgpi = 2
		},
		{
			/* Adapter 1 */
			.ts_vp   = 2,
			.ts_fgpi = 3
		}
	}
};


static const struct pci_device_id saa716x_ff_pci_table[] = {
	MAKE_ENTRY(TECHNOTREND, S2_6400_DUAL_S2_PREMIUM_DEVEL, SAA7160, &saa716x_s26400_config),
	MAKE_ENTRY(TECHNOTREND, S2_6400_DUAL_S2_PREMIUM_PROD, SAA7160, &saa716x_s26400_config),
	{ }
};
MODULE_DEVICE_TABLE(pci, saa716x_ff_pci_table);

static struct pci_driver saa716x_ff_pci_driver = {
	.name			= DRIVER_NAME,
	.id_table		= saa716x_ff_pci_table,
	.probe			= saa716x_ff_pci_probe,
	.remove			= saa716x_ff_pci_remove,
};

static int __init saa716x_ff_init(void)
{
	return pci_register_driver(&saa716x_ff_pci_driver);
}

static void __exit saa716x_ff_exit(void)
{
	return pci_unregister_driver(&saa716x_ff_pci_driver);
}

module_init(saa716x_ff_init);
module_exit(saa716x_ff_exit);

MODULE_DESCRIPTION("SAA716x FF driver");
MODULE_AUTHOR("Manu Abraham");
MODULE_LICENSE("GPL");
