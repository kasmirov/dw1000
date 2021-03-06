/*
 * DecaWave DW1000 IEEE 802.15.4 UWB wireless driver
 *
 * Copyright (C) 2018 Michael Brown <mbrown@fensystems.co.uk>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <net/mac802154.h>
#include "dw1000.h"

/******************************************************************************
 *
 * Enumeration conversions
 *
 */

/* Pulse repetition frequency values */
static const unsigned int dw1000_prfs[] = {
	[DW1000_PRF_16M] = 16,
	[DW1000_PRF_64M] = 64,
};

/* Data rate values */
static const unsigned int dw1000_rates[] = {
	[DW1000_RATE_110K] = 110,
	[DW1000_RATE_850K] = 850,
	[DW1000_RATE_6800K] = 6800,
};

/* Transmit preamble symbol repetitions values */
static const unsigned int dw1000_txpsrs[] = {
	[DW1000_TXPSR_DEFAULT] = 0,
	[DW1000_TXPSR_64] = 64,
	[DW1000_TXPSR_128] = 128,
	[DW1000_TXPSR_256] = 256,
	[DW1000_TXPSR_512] = 512,
	[DW1000_TXPSR_1024] = 1024,
	[DW1000_TXPSR_2048] = 2048,
	[DW1000_TXPSR_4096] = 4096,
};

/**
 * Look up pulse repetition frequency
 *
 * @prf:		Pulse repetition frequency in MHz
 * @return:		Pulse repetition frequency enumeration or -errno
 */
static int dw1000_lookup_prf(unsigned int prf)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dw1000_prfs); i++) {
		if (dw1000_prfs[i] && (dw1000_prfs[i] == prf))
			return i;
	}
	return -EINVAL;
}

/**
 * Look up data rate
 *
 * @rate:		Data rate in kbps
 * @return:		Data rate enumeration or -errno
 */
static int dw1000_lookup_rate(unsigned int rate)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dw1000_rates); i++) {
		if (dw1000_rates[i] && (dw1000_rates[i] == rate))
			return i;
	}
	return -EINVAL;
}

/**
 * Look up preamble symbol repetitions
 *
 * @txpsr:		Preamble symbol repetitions
 * @return:		Preamble symbol repetitions enumeration or -errno
 */
static int dw1000_lookup_txpsr(unsigned int txpsr)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(dw1000_txpsrs); i++) {
		if (dw1000_txpsrs[i] == txpsr)
			return i;
	}
	return -EINVAL;
}

/******************************************************************************
 *
 * Register access
 *
 */

/**
 * dw1000_init_transfers() - Initialise transfer set
 *
 * @msg:		SPI message
 * @xfers:		SPI transfer set
 * @file:		Register file
 * @offset:		Offset within register file
 */
static void dw1000_init_transfers(struct spi_message *msg,
				  struct dw1000_spi_transfers *xfers,
				  unsigned int file, unsigned int offset)
{
	struct spi_transfer *previous;

	/* Construct transfers */
	xfers->header.tx_buf = &xfers->command;
	xfers->command.file = file;
	if (likely(offset == 0)) {
		xfers->header.len = offsetofend(typeof(xfers->command), file);
	} else if (offset < DW1000_EXTENDED) {
		xfers->command.file |= DW1000_OFFSET;
		xfers->command.low = offset;
		xfers->header.len = offsetofend(typeof(xfers->command), low);
	} else {
		xfers->command.file |= DW1000_OFFSET;
		xfers->command.low = (offset | DW1000_EXTENDED);
		xfers->command.high = (offset >> DW1000_EXTENDED_SHIFT);
		xfers->header.len = offsetofend(typeof(xfers->command), high);
	}

	/* Toggle chip select for this transfer set if required */
	if (!list_empty(&msg->transfers)) {
		previous = list_last_entry(&msg->transfers, struct spi_transfer,
					   transfer_list);
		previous->cs_change = 1;
	}

	/* Add transfers to message */
	spi_message_add_tail(&xfers->header, msg);
	spi_message_add_tail(&xfers->data, msg);
}

/**
 * dw1000_init_read() - Initialise read transfer set
 *
 * @msg:		SPI message
 * @xfers:		SPI transfer set
 * @file:		Register file
 * @offset:		Offset within register file
 * @data:		Data buffer
 * @len:		Length of data
 */
static void dw1000_init_read(struct spi_message *msg,
			     struct dw1000_spi_transfers *xfers,
			     unsigned int file, unsigned int offset,
			     void *data, size_t len)
{
	/* Initialise transfers */
	dw1000_init_transfers(msg, xfers, file, offset);

	/* Construct data transfer */
	xfers->data.rx_buf = data;
	xfers->data.len = len;
}

/**
 * dw1000_init_write() - Initialise write transfer set
 *
 * @msg:		SPI message
 * @xfers:		SPI transfer set
 * @file:		Register file
 * @offset:		Offset within register file
 * @data:		Data buffer
 * @len:		Length of data
 */
static void dw1000_init_write(struct spi_message *msg,
			      struct dw1000_spi_transfers *xfers,
			      unsigned int file, unsigned int offset,
			      const void *data, size_t len)
{
	/* Initialise transfers */
	dw1000_init_transfers(msg, xfers, file, offset);
	xfers->command.file |= DW1000_WRITE;

	/* Construct data transfer */
	xfers->data.tx_buf = data;
	xfers->data.len = len;
}

/**
 * dw1000_read() - Read register(s)
 *
 * @dw:			DW1000 device
 * @file:		Register file
 * @offset:		Offset within register file
 * @data:		Raw register value
 * @len:		Length of register value
 * @return:		0 on success or -errno
 */
static int dw1000_read(struct dw1000 *dw, unsigned int file,
		       unsigned int offset, void *data, size_t len)
{
	struct {
		struct spi_message msg;
		struct dw1000_spi_transfers xfers;
	} read;

	/* Construct message */
	memset(&read, 0, sizeof(read));
	spi_message_init_no_memset(&read.msg);
	dw1000_init_read(&read.msg, &read.xfers, file, offset, data, len);

	return spi_sync(dw->spi, &read.msg);
}

/**
 * dw1000_write() - Write register(s)
 *
 * @dw:			DW1000 device
 * @file:		Register file
 * @offset:		Offset within register file
 * @data:		Raw register value
 * @len:		Length of register value
 * @return:		0 on success or -errno
 */
static int dw1000_write(struct dw1000 *dw, unsigned int file,
			unsigned int offset, const void *data, size_t len)
{
	struct {
		struct spi_message msg;
		struct dw1000_spi_transfers xfers;
	} write;

	/* Construct message */
	memset(&write, 0, sizeof(write));
	spi_message_init_no_memset(&write.msg);
	dw1000_init_write(&write.msg, &write.xfers, file, offset, data, len);

	return spi_sync(dw->spi, &write.msg);
}

/******************************************************************************
 *
 * Register map abstraction
 *
 */

/**
 * dw1000_regmap_write() - Write register(s)
 *
 * @context:		DW1000 register map
 * @data:		Buffer containing register offset and raw register value
 * @len:		Length of buffer
 * @return:		0 on success or -errno
 */
static int dw1000_regmap_write(void *context, const void *data, size_t len)
{
	struct dw1000_regmap *map = context;
	const uint16_t *offset;

	/* Extract offset */
	if (len < sizeof(*offset))
		return -EINVAL;
	offset = data;
	data += sizeof(*offset);
	len -= sizeof(*offset);

	return dw1000_write(map->dw, map->config->file, *offset, data, len);
}

/**
 * dw1000_regmap_read() - Read register(s)
 *
 * @context:		DW1000 register map
 * @reg:		Buffer containing register offset
 * @reg_len:		Length of register offset
 * @val:		Buffer to contain raw register value
 * @val_len:		Length of raw register value
 * @return:		0 on success or -errno
 */
static int dw1000_regmap_read(void *context, const void *reg, size_t reg_len,
			      void *val, size_t val_len)
{
	struct dw1000_regmap *map = context;
	const uint16_t *offset;

	/* Extract offset */
	if (reg_len != sizeof(*offset))
		return -EINVAL;
	offset = reg;

	return dw1000_read(map->dw, map->config->file, *offset, val, val_len);
}

/* Register map bus */
static const struct regmap_bus dw1000_regmap_bus = {
	.write = dw1000_regmap_write,
	.read = dw1000_regmap_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_LITTLE,
};

/* Register map configurations */
#define DW1000_REGMAP(_name, _file, _len, _type, ...)			\
	static const struct dw1000_regmap_config dw1000_ ## _name = {	\
		.offset = offsetof(struct dw1000, _name),		\
		.file =	_file,						\
		.shift = ilog2(sizeof(_type)),				\
		.config = {						\
			.name = #_name,					\
			.reg_bits = DW1000_REG_BITS,			\
			.reg_stride = sizeof(_type),			\
			.val_bits = (sizeof(_type) * BITS_PER_BYTE),	\
			.max_register = ((_len) - sizeof(_type)),	\
			__VA_ARGS__					\
		},							\
	}
DW1000_REGMAP(dev_id, DW1000_DEV_ID, DW1000_DEV_ID_LEN, uint16_t);
DW1000_REGMAP(eui, DW1000_EUI, DW1000_EUI_LEN, uint8_t);
DW1000_REGMAP(panadr, DW1000_PANADR, DW1000_PANADR_LEN, uint16_t);
DW1000_REGMAP(sys_cfg, DW1000_SYS_CFG, DW1000_SYS_CFG_LEN, uint32_t);
DW1000_REGMAP(sys_time, DW1000_SYS_TIME, DW1000_SYS_TIME_LEN, uint8_t);
DW1000_REGMAP(tx_fctrl, DW1000_TX_FCTRL, DW1000_TX_FCTRL_LEN, uint8_t);
DW1000_REGMAP(tx_buffer, DW1000_TX_BUFFER, DW1000_TX_BUFFER_LEN, uint8_t);
DW1000_REGMAP(dx_time, DW1000_DX_TIME, DW1000_DX_TIME_LEN, uint8_t);
DW1000_REGMAP(rx_fwto, DW1000_RX_FWTO, DW1000_RX_FWTO_LEN, uint16_t);
DW1000_REGMAP(sys_ctrl, DW1000_SYS_CTRL, DW1000_SYS_CTRL_LEN, uint8_t);
DW1000_REGMAP(sys_mask, DW1000_SYS_MASK, DW1000_SYS_MASK_LEN, uint32_t);
DW1000_REGMAP(sys_status, DW1000_SYS_STATUS, DW1000_SYS_STATUS_LEN, uint32_t);
DW1000_REGMAP(rx_finfo, DW1000_RX_FINFO, DW1000_RX_FINFO_LEN, uint32_t);
DW1000_REGMAP(rx_buffer, DW1000_RX_BUFFER, DW1000_RX_BUFFER_LEN, uint8_t);
DW1000_REGMAP(rx_fqual, DW1000_RX_FQUAL, DW1000_RX_FQUAL_LEN, uint16_t);
DW1000_REGMAP(rx_ttcki, DW1000_RX_TTCKI, DW1000_RX_TTCKI_LEN, uint32_t);
DW1000_REGMAP(rx_ttcko, DW1000_RX_TTCKO, DW1000_RX_TTCKO_LEN, uint8_t);
DW1000_REGMAP(rx_time, DW1000_RX_TIME, DW1000_RX_TIME_LEN, uint8_t);
DW1000_REGMAP(tx_time, DW1000_TX_TIME, DW1000_TX_TIME_LEN, uint8_t);
DW1000_REGMAP(tx_antd, DW1000_TX_ANTD, DW1000_TX_ANTD_LEN, uint16_t);
DW1000_REGMAP(sys_state, DW1000_SYS_STATE, DW1000_SYS_STATE_LEN, uint8_t);
DW1000_REGMAP(ack_resp_t, DW1000_ACK_RESP_T, DW1000_ACK_RESP_T_LEN, uint32_t);
DW1000_REGMAP(rx_sniff, DW1000_RX_SNIFF, DW1000_RX_SNIFF_LEN, uint32_t);
DW1000_REGMAP(tx_power, DW1000_TX_POWER, DW1000_TX_POWER_LEN, uint8_t);
DW1000_REGMAP(chan_ctrl, DW1000_CHAN_CTRL, DW1000_CHAN_CTRL_LEN, uint32_t);
DW1000_REGMAP(usr_sfd, DW1000_USR_SFD, DW1000_USR_SFD_LEN, uint8_t);
DW1000_REGMAP(agc_ctrl, DW1000_AGC_CTRL, DW1000_AGC_CTRL_LEN, uint8_t);
DW1000_REGMAP(ext_sync, DW1000_EXT_SYNC, DW1000_EXT_SYNC_LEN, uint32_t);
DW1000_REGMAP(acc_mem, DW1000_ACC_MEM, DW1000_ACC_MEM_LEN, uint32_t);
DW1000_REGMAP(gpio_ctrl, DW1000_GPIO_CTRL, DW1000_GPIO_CTRL_LEN, uint32_t);
DW1000_REGMAP(drx_conf, DW1000_DRX_CONF, DW1000_DRX_CONF_LEN, uint16_t);
DW1000_REGMAP(rf_conf, DW1000_RF_CONF, DW1000_RF_CONF_LEN, uint8_t);
DW1000_REGMAP(tx_cal, DW1000_TX_CAL, DW1000_TX_CAL_LEN, uint8_t);
DW1000_REGMAP(fs_ctrl, DW1000_FS_CTRL, DW1000_FS_CTRL_LEN, uint8_t);
DW1000_REGMAP(aon, DW1000_AON, DW1000_AON_LEN, uint8_t);
DW1000_REGMAP(otp_if, DW1000_OTP_IF, DW1000_OTP_IF_LEN, uint16_t);
DW1000_REGMAP(lde_if, DW1000_LDE_IF, DW1000_LDE_IF_LEN, uint8_t);
DW1000_REGMAP(dig_diag, DW1000_DIG_DIAG, DW1000_DIG_DIAG_LEN, uint16_t);
DW1000_REGMAP(pmsc, DW1000_PMSC, DW1000_PMSC_LEN, uint32_t);

static const struct dw1000_regmap_config *dw1000_configs[] = {
	&dw1000_dev_id,
	&dw1000_eui,
	&dw1000_panadr,
	&dw1000_sys_cfg,
	&dw1000_sys_time,
	&dw1000_tx_fctrl,
	&dw1000_tx_buffer,
	&dw1000_dx_time,
	&dw1000_rx_fwto,
	&dw1000_sys_ctrl,
	&dw1000_sys_mask,
	&dw1000_sys_status,
	&dw1000_rx_finfo,
	&dw1000_rx_buffer,
	&dw1000_rx_fqual,
	&dw1000_rx_ttcki,
	&dw1000_rx_ttcko,
	&dw1000_rx_time,
	&dw1000_tx_time,
	&dw1000_tx_antd,
	&dw1000_sys_state,
	&dw1000_ack_resp_t,
	&dw1000_rx_sniff,
	&dw1000_tx_power,
	&dw1000_chan_ctrl,
	&dw1000_usr_sfd,
	&dw1000_agc_ctrl,
	&dw1000_ext_sync,
	&dw1000_acc_mem,
	&dw1000_gpio_ctrl,
	&dw1000_drx_conf,
	&dw1000_rf_conf,
	&dw1000_tx_cal,
	&dw1000_fs_ctrl,
	&dw1000_aon,
	&dw1000_otp_if,
	&dw1000_lde_if,
	&dw1000_dig_diag,
	&dw1000_pmsc,
};

/**
 * dw1000_regmap_init() - Initialise register maps
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_regmap_init(struct dw1000 *dw)
{
	const struct dw1000_regmap_config *config;
	struct dw1000_regmap *map;
	unsigned int i;
	int rc;

	/* Initialise register maps */
	for (i = 0; i < ARRAY_SIZE(dw1000_configs); i++) {
		config = dw1000_configs[i];
		map = (void *)dw + config->offset;
		map->dw = dw;
		map->config = config;
		map->regs = devm_regmap_init(dw->dev, &dw1000_regmap_bus, map,
					     &config->config);
		if (IS_ERR(map->regs)) {
			rc = PTR_ERR(map->regs);
			dev_err(dw->dev, "could not allocate %s map: %d\n",
				config->config.name, rc);
			return rc;
		}
	}

	return 0;
}

/******************************************************************************
 *
 * One-time programmable memory (OTP) access
 *
 */

/**
 * dw1000_otp_read() - Read from one-time programmable memory
 *
 * @dw:			DW1000 device
 * @address:		OTP address
 * @data:		Data to fill in
 * @return:		0 on success or -errno
 */
static int dw1000_otp_read(struct dw1000 *dw, unsigned int address,
			   __le32 *data) {
	int otp_ctrl;
	int rc;

	/* Set OTP address */
	if ((rc = regmap_write(dw->otp_if.regs, DW1000_OTP_ADDR, address)) != 0)
		return rc;

	/* Initiate read */
	otp_ctrl = (DW1000_OTP_CTRL_OTPRDEN | DW1000_OTP_CTRL_OTPREAD);
	if ((rc = regmap_write(dw->otp_if.regs, DW1000_OTP_CTRL,
			       otp_ctrl)) != 0)
		return rc;

	/* Wait for read to complete */
	usleep_range(DW1000_OTP_WAIT_MIN_US, DW1000_OTP_WAIT_MAX_US);

	/* Check that read has completed */
	if ((rc = regmap_read(dw->otp_if.regs, DW1000_OTP_CTRL,
			      &otp_ctrl)) != 0)
		return rc;
	if (otp_ctrl & DW1000_OTP_CTRL_OTPREAD) {
		dev_err(dw->dev, "OTP read timed out\n");
		return -ETIMEDOUT;
	}

	/* Read data */
	if ((rc = regmap_raw_read(dw->otp_if.regs, DW1000_OTP_RDAT, data,
				  sizeof(*data))) != 0)
		return rc;

	/* Terminate read */
	if ((rc = regmap_write(dw->otp_if.regs, DW1000_OTP_CTRL, 0)) != 0)
		return rc;

	return 0;
}

/**
 * dw1000_otp_reg_read() - Read OTP register(s)
 *
 * @context:		DW1000 register map
 * @reg:		Buffer containing register offset
 * @reg_len:		Length of register offset
 * @val:		Buffer to contain raw register value
 * @val_len:		Length of raw register value
 * @return:		0 on success or -errno
 */
static int dw1000_otp_reg_read(void *context, unsigned int reg,
			       unsigned int *val)
{
	struct dw1000 *dw = context;
	__le32 data;
	int rc;

	/* Read register */
	if ((rc = dw1000_otp_read(dw, reg, &data)) != 0)
		return rc;

	/* Convert value */
	*val = le32_to_cpu(data);

	return 0;
}

/* OTP register map bus */
static const struct regmap_bus dw1000_otp_regmap_bus = {
	.reg_read = dw1000_otp_reg_read,
	.reg_format_endian_default = REGMAP_ENDIAN_NATIVE,
	.val_format_endian_default = REGMAP_ENDIAN_NATIVE,
};

/* OTP register map configuration */
static const struct regmap_config dw1000_otp_regmap_config = {
	.name = "otp",
	.val_bits = 32,
	.max_register = 0x1f,
};

/**
 * dw1000_otp_regmap_init() - Initialise OTP register map
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_otp_regmap_init(struct dw1000 *dw)
{
	int rc;

	/* Initialise register maps */
	dw->otp = devm_regmap_init(dw->dev, &dw1000_otp_regmap_bus, dw,
				   &dw1000_otp_regmap_config);
	if (IS_ERR(dw->otp)) {
		rc = PTR_ERR(dw->otp);
		dev_err(dw->dev, "could not allocate OTP map: %d\n", rc);
		return rc;
	}

	return 0;
}

/******************************************************************************
 *
 * Radio configuration
 *
 * All magic register values are taken directly from the datasheet.
 *
 */

/* Channel configurations */
static const struct dw1000_channel_config dw1000_channel_configs[] = {
	[1] = {
		.rf_txctrl = { 0x40, 0x5c, 0x00 },
		.rf_rxctrlh = 0xd8,
		.tc_pgdelay = 0xc9,
		.fs_pllcfg = { 0x07, 0x04, 0x00, 0x09 },
		.fs_plltune = 0x1e,
		.tx_power = {
			[DW1000_PRF_16M] = { 0x75, 0x55, 0x35, 0x15 },
			[DW1000_PRF_64M] = { 0x67, 0x47, 0x27, 0x07 },
		},
		.pcodes = {
			[DW1000_PRF_16M] = (BIT(1) | BIT(2)),
			[DW1000_PRF_64M] = (BIT(9) | BIT(10) | BIT(11) |
					    BIT(12)),
		},
	},
	[2] = {
		.rf_txctrl = { 0xa0, 0x5c, 0x04 },
		.rf_rxctrlh = 0xd8,
		.tc_pgdelay = 0xc2,
		.fs_pllcfg = { 0x08, 0x05, 0x40, 0x08 },
		.fs_plltune = 0x26,
		.tx_power = {
			[DW1000_PRF_16M] = { 0x75, 0x55, 0x35, 0x15 },
			[DW1000_PRF_64M] = { 0x67, 0x47, 0x27, 0x07 },
		},
		.pcodes = {
			[DW1000_PRF_16M] = (BIT(3) | BIT(4)),
			[DW1000_PRF_64M] = (BIT(9) | BIT(10) | BIT(11) |
					    BIT(12)),
		},
	},
	[3] = {
		.rf_txctrl = { 0xc0, 0x6c, 0x08 },
		.rf_rxctrlh = 0xd8,
		.tc_pgdelay = 0xc5,
		.fs_pllcfg = { 0x09, 0x10, 0x40, 0x08 },
		.fs_plltune = 0x56,
		.tx_power = {
			[DW1000_PRF_16M] = { 0x6f, 0x4f, 0x2f, 0x0f },
			[DW1000_PRF_64M] = { 0x8b, 0x6b, 0x4b, 0x2b },
		},
		.pcodes = {
			[DW1000_PRF_16M] = (BIT(5) | BIT(6)),
			[DW1000_PRF_64M] = (BIT(9) | BIT(10) | BIT(11) |
					    BIT(12)),
		},
	},
	[4] = {
		.rf_txctrl = { 0x80, 0x5c, 0x04 },
		.rf_rxctrlh = 0xbc,
		.tc_pgdelay = 0x95,
		.fs_pllcfg = { 0x08, 0x05, 0x40, 0x08 },
		.fs_plltune = 0x26,
		.tx_power = {
			[DW1000_PRF_16M] = { 0x5f, 0x3f, 0x1f, 0x1f },
			[DW1000_PRF_64M] = { 0x9a, 0x7a, 0x5a, 0x3a },
		},
		.pcodes = {
			[DW1000_PRF_16M] = (BIT(3) | BIT(4) | BIT(7) | BIT(8)),
			[DW1000_PRF_64M] = (BIT(9) | BIT(10) | BIT(11) |
					    BIT(12) | BIT(17) | BIT(18) |
					    BIT(19) | BIT(20)),
		},
	},
	[5] = {
		.rf_txctrl = { 0xe0, 0x3f, 0x1e },
		.rf_rxctrlh = 0xd8,
		.tc_pgdelay = 0xc0,
		.fs_pllcfg = { 0x1d, 0x04, 0x00, 0x08 },
		.fs_plltune = 0xbe,
		.tx_power = {
			[DW1000_PRF_16M] = { 0x48, 0x28, 0x08, 0x0e },
			[DW1000_PRF_64M] = { 0x85, 0x65, 0x45, 0x25 },
		},
		.pcodes = {
			[DW1000_PRF_16M] = (BIT(3) | BIT(4)),
			[DW1000_PRF_64M] = (BIT(9) | BIT(10) | BIT(11) |
					    BIT(12)),
		},
	},
	[7] = {
		.rf_txctrl = { 0xe0, 0x7d, 0x1e },
		.rf_rxctrlh = 0xbc,
		.tc_pgdelay = 0x93,
		.fs_pllcfg = { 0x1d, 0x04, 0x00, 0x08 },
		.fs_plltune = 0xbe,
		.tx_power = {
			[DW1000_PRF_16M] = { 0x92, 0x72, 0x52, 0x32 },
			[DW1000_PRF_64M] = { 0xd1, 0xb1, 0x71, 0x51 },
		},
		.pcodes = {
			[DW1000_PRF_16M] = (BIT(3) | BIT(4) | BIT(7) | BIT(8)),
			[DW1000_PRF_64M] = (BIT(9) | BIT(10) | BIT(11) |
					    BIT(12) | BIT(17) | BIT(18) |
					    BIT(19) | BIT(20)),
		},
	},
};

/* Preamble code configurations */
static const struct dw1000_pcode_config dw1000_pcode_configs[] = {
	[1]  = { .lde_repc = 0x5998 },
	[2]  = { .lde_repc = 0x5998 },
	[3]  = { .lde_repc = 0x51ea },
	[4]  = { .lde_repc = 0x428e },
	[5]  = { .lde_repc = 0x451e },
	[6]  = { .lde_repc = 0x2e14 },
	[7]  = { .lde_repc = 0x8000 },
	[8]  = { .lde_repc = 0x51ea },
	[9]  = { .lde_repc = 0x28f4 },
	[10] = { .lde_repc = 0x3332 },
	[11] = { .lde_repc = 0x3ae0 },
	[12] = { .lde_repc = 0x3d70 },
	[13] = { .lde_repc = 0x3ae0 },
	[14] = { .lde_repc = 0x35c2 },
	[15] = { .lde_repc = 0x2b84 },
	[16] = { .lde_repc = 0x35c2 },
	[17] = { .lde_repc = 0x3332 },
	[18] = { .lde_repc = 0x35c2 },
	[19] = { .lde_repc = 0x35c2 },
	[20] = { .lde_repc = 0x47ae },
	[21] = { .lde_repc = 0x3ae0 },
	[22] = { .lde_repc = 0x3850 },
	[23] = { .lde_repc = 0x30a2 },
	[24] = { .lde_repc = 0x3850 },
};

/* Pulse repetition frequency configurations */
static const struct dw1000_prf_config dw1000_prf_configs[] = {
	[DW1000_PRF_16M] = {
		.agc_tune1 = { 0x70, 0x88 },
		.drx_tune1a = 0x0087,
		.lde_cfg2 = { 0x07, 0x16 },
	},
	[DW1000_PRF_64M] = {
		.agc_tune1 = { 0x9b, 0x88 },
		.drx_tune1a = 0x008d,
		.lde_cfg2 = { 0x07, 0x06 },
	},
};

/* Data rate configurations */
static const struct dw1000_rate_config dw1000_rate_configs[] = {
	[DW1000_RATE_110K] = {
		.tdsym_ns = 8205,
		.txpsr = DW1000_TXPSR_4096,
		.drx_tune0b = 0x000a,
		.drx_tune1b = 0x0064,
		.drx_tune2 = {
			[DW1000_PRF_16M] = { 0x1d, 0x01, 0x1a, 0x37 },
			[DW1000_PRF_64M] = { 0x96, 0x02, 0x3b, 0x37 },
		},
	},
	[DW1000_RATE_850K] = {
		.tdsym_ns = 1025,
		.txpsr = DW1000_TXPSR_1024,
		.drx_tune0b = 0x0001,
		.drx_tune1b = 0x0020,
		.drx_tune2 = {
			[DW1000_PRF_16M] = { 0x9a, 0x00, 0x1a, 0x35 },
			[DW1000_PRF_64M] = { 0x5e, 0x01, 0x3b, 0x35 },
		},
	},
	[DW1000_RATE_6800K] = {
		.tdsym_ns = 128,
		.txpsr = DW1000_TXPSR_64,
		.drx_tune0b = 0x0001,
		.drx_tune1b = 0x0010,
		.drx_tune2 = {
			[DW1000_PRF_16M] = { 0x2d, 0x00, 0x1a, 0x31 },
			[DW1000_PRF_64M] = { 0x6b, 0x00, 0x3b, 0x31 },
		},
	},
};

/* Preamble symbol repetitions configurations */
static const struct dw1000_txpsr_config dw1000_txpsr_configs[] = {
	[DW1000_TXPSR_64]   = { .drx_tune4h = 0x0010 },
	[DW1000_TXPSR_128]  = { .drx_tune4h = 0x0028 },
	[DW1000_TXPSR_256]  = { .drx_tune4h = 0x0028 },
	[DW1000_TXPSR_512]  = { .drx_tune4h = 0x0028 },
	[DW1000_TXPSR_1024] = { .drx_tune4h = 0x0028 },
	[DW1000_TXPSR_2048] = { .drx_tune4h = 0x0028 },
	[DW1000_TXPSR_4096] = { .drx_tune4h = 0x0028 },
};

/* Fixed configurations */
static const struct dw1000_fixed_config dw1000_fixed_config = {
	.agc_tune2 = { 0x07, 0xa9, 0x02, 0x25 },
	.agc_tune3 = { 0x35, 0x00 },
};

/* Radio reconfiguration change bitmasks */
enum dw1000_configuration_change {
	DW1000_CONFIGURE_FIXED		= BIT(0),
	DW1000_CONFIGURE_CHANNEL	= BIT(1),
	DW1000_CONFIGURE_PCODE		= BIT(2),
	DW1000_CONFIGURE_PRF		= BIT(3),
	DW1000_CONFIGURE_ANTD		= BIT(4),
	DW1000_CONFIGURE_RATE		= BIT(5),
	DW1000_CONFIGURE_TXPSR		= BIT(6),
	DW1000_CONFIGURE_XTAL_TRIM	= BIT(7),
	DW1000_CONFIGURE_TX_POWER	= BIT(8),
	DW1000_CONFIGURE_SMART_POWER	= BIT(9),
};

/**
 * dw1000_pcode() - Calculate preamble code
 *
 * @dw:			DW1000 device
 * @return:		Preamble code
 */
static unsigned int dw1000_pcode(struct dw1000 *dw)
{
	unsigned int pcode = dw->pcode[dw->prf];
	unsigned long pcodes;

	/* Use explicitly set preamble code if configured */
	pcodes = dw1000_channel_configs[dw->channel].pcodes[dw->prf];
	if (BIT(pcode) & pcodes)
		return pcode;

	/* Otherwise, use highest supported preamble code */
	return (fls(pcodes) - 1);
}

/**
 * dw1000_txpsr() - Calculate preamble symbol repetitions
 *
 * @dw:			DW1000 device
 * @return:		Preamble symbol repetitions
 */
static enum dw1000_txpsr dw1000_txpsr(struct dw1000 *dw)
{
	/* Use explicitly set preamble symbol repetitions if configured */
	if (dw->txpsr != DW1000_TXPSR_DEFAULT)
		return dw->txpsr;

	/* Otherwise, use default for the current data rate */
	return dw1000_rate_configs[dw->rate].txpsr;
}

/**
 * dw1000_reconfigure() - Reconfigure radio parameters
 *
 * @dw:			DW1000 device
 * @changed:		Changed parameter bitmask
 * @return:		0 on success or -errno
 */
static int dw1000_reconfigure(struct dw1000 *dw, unsigned int changed)
{
	const struct dw1000_channel_config *channel_cfg;
	const struct dw1000_pcode_config *pcode_cfg;
	const struct dw1000_prf_config *prf_cfg;
	const struct dw1000_rate_config *rate_cfg;
	const struct dw1000_txpsr_config *txpsr_cfg;
	const struct dw1000_fixed_config *fixed_cfg;
	uint16_t lde_repc;
	uint16_t lde_rxantd;
	unsigned int channel;
	unsigned int pcode;
	unsigned int xtalt;
	enum dw1000_prf prf;
	enum dw1000_rate rate;
	enum dw1000_txpsr txpsr;
	bool smart_power;
	int mask;
	int value;
	int rc;

	/* Look up current configurations */
	xtalt = dw->xtalt;
	channel = dw->channel;
	prf = dw->prf;
	rate = dw->rate;
	pcode = dw1000_pcode(dw);
	txpsr = dw1000_txpsr(dw);
	smart_power = dw->smart_power;
	channel_cfg = &dw1000_channel_configs[channel];
	pcode_cfg = &dw1000_pcode_configs[pcode];
	prf_cfg = &dw1000_prf_configs[prf];
	rate_cfg = &dw1000_rate_configs[rate];
	txpsr_cfg = &dw1000_txpsr_configs[txpsr];
	fixed_cfg = &dw1000_fixed_config;

	/* Calculate inter-frame spacing */
	dw->phy->symbol_duration = (rate_cfg->tdsym_ns / 1000);
	if (!dw->phy->symbol_duration)
		dw->phy->symbol_duration = 1;
	dw->phy->lifs_period =
		((IEEE802154_LIFS_PERIOD * rate_cfg->tdsym_ns) / 1000);
	dw->phy->sifs_period =
		((IEEE802154_SIFS_PERIOD * rate_cfg->tdsym_ns) / 1000);

	/* SYS_CFG register */
	if (changed & (DW1000_CONFIGURE_RATE | DW1000_CONFIGURE_SMART_POWER)) {
		mask = (DW1000_SYS_CFG_DIS_STXP | DW1000_SYS_CFG_RXM110K);
		value = ((smart_power ? 0 : DW1000_SYS_CFG_DIS_STXP) |
			 ((rate == DW1000_RATE_110K) ?
			  DW1000_SYS_CFG_RXM110K : 0));
		if ((rc = regmap_update_bits(dw->sys_cfg.regs, 0, mask,
					     value)) != 0)
			return rc;
	}

	/* TX_FCTRL registers */
	if (changed & DW1000_CONFIGURE_RATE) {
		mask = DW1000_TX_FCTRL1_TXBR_MASK;
		value = DW1000_TX_FCTRL1_TXBR(rate);
		if ((rc = regmap_update_bits(dw->tx_fctrl.regs,
					     DW1000_TX_FCTRL1,
					     mask, value)) != 0)
			return rc;
	}
	if (changed & (DW1000_CONFIGURE_PRF | DW1000_CONFIGURE_RATE |
		       DW1000_CONFIGURE_TXPSR)) {
		mask = (DW1000_TX_FCTRL2_TXPRF_MASK |
			DW1000_TX_FCTRL2_TXPSR_MASK);
		value = (DW1000_TX_FCTRL2_TXPRF(prf) |
			 DW1000_TX_FCTRL2_TXPSR(txpsr));
		if ((rc = regmap_update_bits(dw->tx_fctrl.regs,
					     DW1000_TX_FCTRL2,
					     mask, value)) != 0)
			return rc;
	}

	/* TX_ANTD register */
	if (changed & (DW1000_CONFIGURE_PRF | DW1000_CONFIGURE_ANTD)) {
		if ((rc = regmap_write(dw->tx_antd.regs, 0,
				       dw->antd[prf])) != 0)
			return rc;
	}

	/* TX_POWER register */
	if (changed & (DW1000_CONFIGURE_CHANNEL | DW1000_CONFIGURE_PRF |
		       DW1000_CONFIGURE_SMART_POWER)) {
		if (smart_power) {
			memcpy(dw->txpwr, channel_cfg->tx_power[prf], sizeof(dw->txpwr));
		} else {
			dw->txpwr[0] = dw->txpwr[3] = 0;
			dw->txpwr[1] = dw->txpwr[2] = channel_cfg->tx_power[prf][0];
		}
	}
	if (changed & (DW1000_CONFIGURE_CHANNEL | DW1000_CONFIGURE_PRF |
		       DW1000_CONFIGURE_TX_POWER | DW1000_CONFIGURE_SMART_POWER)) {
		if ((rc = regmap_raw_write(dw->tx_power.regs, 0, dw->txpwr,
					   sizeof(dw->txpwr))) != 0)
			return rc;
	}

	/* CHAN_CTRL register */
	if (changed & (DW1000_CONFIGURE_CHANNEL | DW1000_CONFIGURE_PCODE |
		       DW1000_CONFIGURE_PRF)) {
		mask = (DW1000_CHAN_CTRL_TX_CHAN_MASK |
			DW1000_CHAN_CTRL_RX_CHAN_MASK |
			DW1000_CHAN_CTRL_RXPRF_MASK |
			DW1000_CHAN_CTRL_TX_PCODE_MASK |
			DW1000_CHAN_CTRL_RX_PCODE_MASK);
		value = (DW1000_CHAN_CTRL_TX_CHAN(channel) |
			 DW1000_CHAN_CTRL_RX_CHAN(channel) |
			 DW1000_CHAN_CTRL_RXPRF(prf) |
			 DW1000_CHAN_CTRL_TX_PCODE(pcode) |
			 DW1000_CHAN_CTRL_RX_PCODE(pcode));
		if ((rc = regmap_update_bits(dw->chan_ctrl.regs, 0, mask,
					     value)) != 0)
			return rc;
	}

	/* AGC_CTRL registers */
	if (changed & DW1000_CONFIGURE_PRF) {
		if ((rc = regmap_raw_write(dw->agc_ctrl.regs, DW1000_AGC_TUNE1,
					   prf_cfg->agc_tune1,
					   sizeof(prf_cfg->agc_tune1))) != 0)
			return rc;
	}
	if (changed & DW1000_CONFIGURE_FIXED) {
		if ((rc = regmap_raw_write(dw->agc_ctrl.regs, DW1000_AGC_TUNE2,
					   fixed_cfg->agc_tune2,
					   sizeof(fixed_cfg->agc_tune2))) != 0)
			return rc;
		if ((rc = regmap_raw_write(dw->agc_ctrl.regs, DW1000_AGC_TUNE3,
					   fixed_cfg->agc_tune3,
					   sizeof(fixed_cfg->agc_tune3))) != 0)
			return rc;
	}

	/* DRX_CONF registers */
	if (changed & DW1000_CONFIGURE_RATE) {
		if ((rc = regmap_write(dw->drx_conf.regs, DW1000_DRX_TUNE0B,
				       rate_cfg->drx_tune0b)) != 0)
			return rc;
	}
	if (changed & DW1000_CONFIGURE_PRF) {
		if ((rc = regmap_write(dw->drx_conf.regs, DW1000_DRX_TUNE1A,
				       prf_cfg->drx_tune1a)) != 0)
			return rc;
	}
	if (changed & DW1000_CONFIGURE_RATE) {
		if ((rc = regmap_write(dw->drx_conf.regs, DW1000_DRX_TUNE1B,
				       rate_cfg->drx_tune1b)) != 0)
			return rc;
	}
	if (changed & (DW1000_CONFIGURE_PRF | DW1000_CONFIGURE_RATE)) {
		if ((rc = regmap_raw_write(dw->drx_conf.regs, DW1000_DRX_TUNE2,
					   rate_cfg->drx_tune2[prf],
					   sizeof(rate_cfg->
						  drx_tune2[prf]))) != 0)
			return rc;
	}
	if (changed & DW1000_CONFIGURE_RATE) {
		if ((rc = regmap_write(dw->drx_conf.regs, DW1000_DRX_TUNE4H,
				       txpsr_cfg->drx_tune4h)) != 0)
			return rc;
	}

	/* RF_CONF registers */
	if (changed & DW1000_CONFIGURE_CHANNEL) {
		if ((rc = regmap_raw_write(dw->rf_conf.regs, DW1000_RF_TXCTRL,
					   channel_cfg->rf_txctrl,
					   sizeof(channel_cfg->rf_txctrl))) !=0)
			return rc;
		if ((rc = regmap_write(dw->rf_conf.regs, DW1000_RF_RXCTRLH,
				       channel_cfg->rf_rxctrlh)) != 0)
			return rc;
	}

	/* TX_CAL registers */
	if (changed & DW1000_CONFIGURE_CHANNEL) {
		if ((rc = regmap_write(dw->tx_cal.regs, DW1000_TC_PGDELAY,
				       channel_cfg->tc_pgdelay)) != 0)
			return rc;
	}

	/* FS_CTRL registers */
	if (changed & DW1000_CONFIGURE_CHANNEL) {
		if ((rc = regmap_raw_write(dw->fs_ctrl.regs, DW1000_FS_PLLCFG,
					   channel_cfg->fs_pllcfg,
					   sizeof(channel_cfg->fs_pllcfg))) !=0)
			return rc;
		if ((rc = regmap_write(dw->fs_ctrl.regs, DW1000_FS_PLLTUNE,
				       channel_cfg->fs_plltune)) != 0)
			return rc;
	}
	if (changed & DW1000_CONFIGURE_XTAL_TRIM) {
		if ((rc = regmap_update_bits(dw->fs_ctrl.regs, DW1000_FS_XTALT,
					     DW1000_FS_XTALT_XTALT_MASK,
					     xtalt)) != 0)
			return rc;
	}

	/* LDE_IF registers */
	if (changed & (DW1000_CONFIGURE_PRF | DW1000_CONFIGURE_ANTD)) {
		lde_rxantd = cpu_to_le16(dw->antd[prf]);
		if ((rc = regmap_raw_write(dw->lde_if.regs, DW1000_LDE_RXANTD,
					   &lde_rxantd,
					   sizeof(lde_rxantd))) != 0)
			return rc;
	}
	if (changed & DW1000_CONFIGURE_PRF) {
		if ((rc = regmap_raw_write(dw->lde_if.regs, DW1000_LDE_CFG2,
					   prf_cfg->lde_cfg2,
					   sizeof(prf_cfg->lde_cfg2))) != 0)
			return rc;
	}
	if (changed & (DW1000_CONFIGURE_PCODE | DW1000_CONFIGURE_RATE)) {
		lde_repc = pcode_cfg->lde_repc;
		if (rate == DW1000_RATE_110K)
			lde_repc >>= 3;
		cpu_to_le16s(&lde_repc);
		if ((rc = regmap_raw_write(dw->lde_if.regs, DW1000_LDE_REPC,
					   &lde_repc, sizeof(lde_repc))) != 0)
			return rc;
	}

	return 0;
}

/**
 * dw1000_configure_xtalt() - Configure XTAL trim
 *
 * @dw:			DW1000 device
 * @trim:		Trim value
 * @return:		0 on success or -errno
 */
static int dw1000_configure_xtalt(struct dw1000 *dw, unsigned int trim)
{
	int rc;

	/* Check that trim value is supported */
	if (trim & ~DW1000_FS_XTALT_XTALT_MASK)
		return -EINVAL;

	/* Assign new trim value */
	dw->xtalt = trim;

	/* Reconfigure channel */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_XTAL_TRIM)) != 0)
		return rc;

	dev_dbg(dw->dev, "set xtal trim %d\n", trim);
	return 0;
}

/**
 * dw1000_configure_channel() - Configure radio channel
 *
 * @dw:			DW1000 device
 * @channel:		Channel number
 * @return:		0 on success or -errno
 */
static int dw1000_configure_channel(struct dw1000 *dw, unsigned int channel)
{
	int rc;

	/* Check that channel is supported */
	if (!(BIT(channel) & DW1000_CHANNELS))
		return -EINVAL;

	/* Record channel */
	dw->channel = channel;

	/* Reconfigure channel */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_CHANNEL)) != 0)
		return rc;

	dev_dbg(dw->dev, "set channel %d\n", channel);
	return 0;
}

/**
 * dw1000_configure_pcode() - Configure preamble code
 *
 * @dw:			DW1000 device
 * @pcode:		Preamble code (or 0 for automatic selection)
 * @return:		0 on success or -errno
 */
static int dw1000_configure_pcode(struct dw1000 *dw, unsigned int pcode)
{
	unsigned long pcodes;
	int rc;

	/* Check that preamble code is valid */
	if (pcode) {
		pcodes = dw1000_channel_configs[dw->channel].pcodes[dw->prf];
		if (!(BIT(pcode) & pcodes))
			return -EINVAL;
	}

	/* Record preamble code */
	dw->pcode[dw->prf] = pcode;

	/* Reconfigure preamble code */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_PCODE)) != 0)
		return rc;

	dev_dbg(dw->dev, "set preamble code %d\n", pcode);
	return 0;
}

/**
 * dw1000_configure_prf() - Configure pulse repetition frequency
 *
 * @dw:			DW1000 device
 * @prf:		Pulse repetition frequency
 * @return:		0 on success or -errno
 */
static int dw1000_configure_prf(struct dw1000 *dw, enum dw1000_prf prf)
{
	int rc;

	/* Record pulse repetition frequency */
	dw->prf = prf;

	/* Reconfigure pulse repetition frequency */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_PRF)) != 0)
		return rc;

	dev_dbg(dw->dev, "set pulse repetition frequency %dMHz\n",
		dw1000_prfs[dw->prf]);
	return 0;
}

/**
 * dw1000_configure_antd() - Configure antenna delay
 *
 * @dw:			DW1000 device
 * @antd:		Antenna delay
 * @return:		0 on success or -errno
 */
static int dw1000_configure_antd(struct dw1000 *dw, unsigned int delay)
{
	int rc;

	/* Check value range */
	if (delay & ~DW1000_ANTD_MASK)
		return -EINVAL;

	/* Record new delay */
	dw->antd[dw->prf] = delay;

	/* Reconfigure antenna delay */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_ANTD)) != 0)
		return rc;

	dev_dbg(dw->dev, "set antenna delay 0x%4x\n", delay);
	return 0;
}

/**
 * dw1000_configure_rate() - Configure data rate
 *
 * @dw:			DW1000 device
 * @rate:		Data rate
 * @return:		0 on success or -errno
 */
static int dw1000_configure_rate(struct dw1000 *dw, enum dw1000_rate rate)
{
	int rc;

	/* Record data rate */
	dw->rate = rate;

	/* Reconfigure data rate */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_RATE)) != 0)
		return rc;

	dev_dbg(dw->dev, "set data rate %dkbps\n", dw1000_rates[dw->rate]);
	return 0;
}

/**
 * dw1000_configure_txpsr() - Configure transmit preamble symbol repetitions
 *
 * @dw:			DW1000 device
 * @txpsr:		Transmit preamble symbol repetitions
 * @return:		0 on success or -errno
 */
static int dw1000_configure_txpsr(struct dw1000 *dw, enum dw1000_txpsr txpsr)
{
	int rc;

	/* Record transmit preamble symbol repetitions */
	dw->txpsr = txpsr;

	/* Reconfigure transmit preamble symbol repetitions */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_TXPSR)) != 0)
		return rc;

	dev_dbg(dw->dev, "set %d preamble symbol repetitions\n",
		dw1000_txpsrs[dw1000_txpsr(dw)]);
	return 0;
}

/**
 * dw1000_configure_tx_power() - Configure tx power control
 *
 * @dw:			DW1000 device
 * @power:		Tx power level
 * @return:		0 on success or -errno
 */
static int dw1000_configure_tx_power(struct dw1000 *dw, uint32_t power)
{
	int rc;

	/* Record power level */
	dw->txpwr[0] = (power >>  0) & 0xff;
	dw->txpwr[1] = (power >>  8) & 0xff;
	dw->txpwr[2] = (power >> 16) & 0xff;
	dw->txpwr[3] = (power >> 24) & 0xff;

	/* Reconfigure transmit power */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_TX_POWER)) != 0)
		return rc;

	dev_dbg(dw->dev, "set tx power 0x%08x\n", power);
	return 0;
}

/**
 * dw1000_configure_smart_power() - Configure use of smart power control
 *
 * @dw:			DW1000 device
 * @smart_power:	Smart power control enabled
 * @return:		0 on success or -errno
 */
static int dw1000_configure_smart_power(struct dw1000 *dw, bool smart_power)
{
	int rc;

	/* Record smart power control */
	dw->smart_power = smart_power;

	/* Reconfigure transmit power */
	if ((rc = dw1000_reconfigure(dw, DW1000_CONFIGURE_SMART_POWER)) != 0)
		return rc;

	dev_dbg(dw->dev, "set smart power control %s\n",
		(smart_power ? "on" : "off"));
	return 0;
}

/******************************************************************************
 *
 * PTP hardware clock
 *
 */

/**
 * dw1000_cc_read() - Read underlying cycle counter
 *
 * @cc:			Cycle counter
 * @return:		Cycle count
 */
static uint64_t dw1000_ptp_cc_read(const struct hires_counter *tc)
{
	struct dw1000 *dw = container_of(tc, struct dw1000, ptp.tc);
	union dw1000_timestamp time;
	int rc;

	/* The documentation does not state whether or not reads from
	 * the SYS_TIME register are atomic.  At the rated 20MHz SPI
	 * bus speed, reading all 5 bytes will take 2us.  During this
	 * time the SYS_TIME value should have been incremented (in
	 * units of 512) around 250 times.
	 *
	 * We assume that Decawave has taken this problem into account
	 * and caused SYS_TIME reads to return a snapshot of the
	 * complete 40-bit timer at the point that the read was
	 * initiated.
	 *
	 * Use a low-level read to avoid any potential for the regmap
	 * abstraction layer deciding to split the read into multiple
	 * SPI transactions.
	 */
	time.cc = 0;
	if ((rc = dw1000_read(dw, DW1000_SYS_TIME, 0, &time.raw,
			      sizeof(time.raw))) != 0) {
		dev_err(dw->dev, "could not read timestamp: %d\n", rc);
		/* There is no way to indicate failure here */
		return 0;
	}

	return le64_to_cpu(time.cc);
}

/**
 * dw1000_ptp_worker() - Clock wraparound detection worker
 *
 * @work:		Worker
 */
static void dw1000_ptp_worker(struct work_struct *work)
{
	struct dw1000 *dw = container_of(to_delayed_work(work),
					 struct dw1000, ptp_work);
	struct hires_counter *tc = &dw->ptp.tc;

	/* Synchronise counter; this must be done at least
	 * twice per wraparound.
	 */
	mutex_lock(&dw->ptp.mutex);
	hires_counter_sync(tc);
	mutex_unlock(&dw->ptp.mutex);

	/* Reschedule worker */
	schedule_delayed_work(&dw->ptp_work, DW1000_PTP_WORK_DELAY);
}

/**
 * dw1000_ptp_adjfine() - Adjust frequency of hardware clock
 *
 * @ptp:		PTP clock information
 * @delta:		Frequency offset in ppm + 16bit fraction
 * @return:		0 on success or -errno
 */
static int dw1000_ptp_adjfine(struct ptp_clock_info *ptp, long delta)
{
	struct dw1000 *dw = container_of(ptp, struct dw1000, ptp.info);
	struct hires_counter *tc = &dw->ptp.tc;
	uint64_t mult;

	/* Calculate multiplier value */
	mult = (delta < 0) ? -delta : delta;
	/* mult *= DW1000_CYCLECOUNTER_MULT / 65536000000ULL; */
	mult = (mult * (DW1000_CYCLECOUNTER_MULT / 128000000ULL)) >> 9;
	mult = (delta < 0) ? DW1000_CYCLECOUNTER_MULT - mult:
			     DW1000_CYCLECOUNTER_MULT + mult;

	/* Adjust counter multiplier */
	mutex_lock(&dw->ptp.mutex);
	hires_counter_setmult(tc, mult);
	mutex_unlock(&dw->ptp.mutex);

	dev_dbg(dw->dev, "adjust frequency %+ld sppm: multiplier adj %llu\n",
		delta, (unsigned long long) mult);
	return 0;
}

/**
 * dw1000_ptp_adjtime() - Adjust time of hardware clock
 *
 * @ptp:		PTP clock information
 * @delta:		Change in nanoseconds
 * @return:		0 on success or -errno
 */
static int dw1000_ptp_adjtime(struct ptp_clock_info *ptp, int64_t delta)
{
	struct dw1000 *dw = container_of(ptp, struct dw1000, ptp.info);
	struct hires_counter *tc = &dw->ptp.tc;
	struct timehires hdelta = { delta, 0, };

	/* Adjust timecounter */
	mutex_lock(&dw->ptp.mutex);
	hires_counter_adjtime(tc, hdelta);
	mutex_unlock(&dw->ptp.mutex);

	dev_dbg(dw->dev, "adjust time %+lld ns\n", delta);
	return 0;
}

/**
 * dw1000_ptp_gettime() - Get time of hardware clock
 *
 * @ptp:		PTP clock information
 * @ts:			Time to fill in
 * @return:		0 on sucess or -errno
 */
static int dw1000_ptp_gettime(struct ptp_clock_info *ptp,
			      struct timespec64 *ts)
{
	struct dw1000 *dw = container_of(ptp, struct dw1000, ptp.info);
	struct hires_counter *tc = &dw->ptp.tc;
	struct timehires time;

	/* Read timecounter */
	mutex_lock(&dw->ptp.mutex);
	time = hires_counter_cyc2time(tc, hires_counter_read(tc));
	mutex_unlock(&dw->ptp.mutex);

	/* Convert to timespec */
	*ts = ns_to_timespec64(time.tv_nsec);

	return 0;
}

/**
 * dw1000_ptp_settime() - Set time of hardware clock
 *
 * @ptp:		PTP clock information
 * @ts:			Time
 * @return:		0 on sucess or -errno
 */
static int dw1000_ptp_settime(struct ptp_clock_info *ptp,
			      const struct timespec64 *ts)
{
	struct dw1000 *dw = container_of(ptp, struct dw1000, ptp.info);
	struct hires_counter *tc = &dw->ptp.tc;
	struct timehires time;

	/* Convert to nanoseconds */
	time.tv_nsec = timespec64_to_ns(ts);
	time.tv_frac = 0;

	/* Reset timecounter */
	mutex_lock(&dw->ptp.mutex);
	hires_counter_settime(tc, time);
	mutex_unlock(&dw->ptp.mutex);

	dev_dbg(dw->dev, "set time %llu ns\n", time.tv_nsec);
	return 0;
}

/**
 * dw1000_ptp_enable() - Enable/disable feature
 *
 * @ptp:		PTP clock information
 * @request:		Feature request
 * @on:			Enable/disable feature
 * @return:		0 on success or -errno
 */
static int dw1000_ptp_enable(struct ptp_clock_info *ptp,
			     struct ptp_clock_request *request, int on)
{
	/* No additional features supported */
	return -EOPNOTSUPP;
}

/**
 * dw1000_ptp_init() - Initialise PTP clock
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_ptp_init(struct dw1000 *dw)
{
	struct ptp_clock_info *info = &dw->ptp.info;
	struct hires_counter *tc = &dw->ptp.tc;
	struct timehires start;

	/* Initialise mutex */
	mutex_init(&dw->ptp.mutex);

	/* Counter starting time */
	start.tv_nsec = ktime_to_ns(ktime_get_real());
	start.tv_frac = 0;

	/* Initialise time counter */
	hires_counter_init(tc, dw1000_ptp_cc_read, start,
			   DW1000_CYCLECOUNTER_SIZE, DW1000_CYCLECOUNTER_MULT);

	/* Initialise PTP clock information */
	info->owner = THIS_MODULE;
	info->max_adj = DW1000_PTP_MAX_ADJ;
	snprintf(info->name, sizeof(info->name), "%s", dev_name(dw->dev));
	info->adjfine = dw1000_ptp_adjfine;
	info->adjtime = dw1000_ptp_adjtime;
	info->gettime64 = dw1000_ptp_gettime;
	info->settime64 = dw1000_ptp_settime;
	info->enable = dw1000_ptp_enable;

	return 0;
}

/**
 * dw1000_timestamp() - Convert DW1000 timestamp to kernel timestamp
 *
 * @dw:			DW1000 device
 * @time:		DW1000 timestamp
 * @hwtstamps:		Kernel hardware timestamp
 */
static void dw1000_timestamp(struct dw1000 *dw,
			     const union dw1000_timestamp *time,
			     struct skb_shared_hwtstamps *hwtstamps)
{
	struct hires_counter *tc = &dw->ptp.tc;
	struct timehires stamp;
	cycle_t cc;

	/* Get 5-byte timestamp */
	cc = le64_to_cpu(time->cc) & DW1000_TIMESTAMP_MASK;

	/* Convert timestamp to nanoseconds */
	mutex_lock(&dw->ptp.mutex);
	stamp = hires_counter_cyc2time(tc, cc);
	mutex_unlock(&dw->ptp.mutex);

	/* Fill in kernel hardware timestamp */
	memset(hwtstamps, 0, sizeof(*hwtstamps));
	hwtstamps->hwtstamp = ns_to_ktime(stamp.tv_nsec);
#ifdef HAVE_HWTSFRAC
	hwtstamps->hwtsfrac = ns_to_ktime_frac(stamp.tv_frac);
#endif
}

/**
 * dw1000_tsinfo() - Convert DW1000 tsinfo to kernel timestamp
 *
 * @dw:			DW1000 device
 * @time:		DW1000 timestamp
 * @tsinfo:		DW1000 tsinfo
 * @hwtstamps:		Kernel hardware timestamp
 */
static void dw1000_tsinfo(struct dw1000 *dw,
			  const union dw1000_timestamp *time,
			  struct dw1000_tsinfo *tsinfo,
			  struct skb_shared_hwtstamps *hwtstamps)
{
#ifdef HAVE_HWTSINFO
	struct hires_counter *tc = &dw->ptp.tc;
	cycle_t rawts;
	cycle_t cc;

	/* Get 5-byte timestamp */
	cc = le64_to_cpu(time->cc) & DW1000_TIMESTAMP_MASK;

	/* Convert timestamp to nanoseconds */
	mutex_lock(&dw->ptp.mutex);
	rawts = hires_counter_cyc2raw(tc, cc);
	mutex_unlock(&dw->ptp.mutex);

	/* Assign raw timestamp to tsinfo */
	tsinfo->rawts = rawts;

	/* Fill in kernel timestamp info */
	memcpy(hwtstamps->hwtsinfo, tsinfo, sizeof(*tsinfo));
#endif
}

/**
 * dw1000_rx_link_qual() - Check RX quality and calculate KPIs, as
 *			   described in the DW1000 manual section 4.7.
 *
 * @dw:			DW1000 device
 */
static void dw1000_rx_link_qual(struct dw1000 *dw)
{
	struct dw1000_rx *rx = &dw->rx;
	struct dw1000_tsinfo *tsi = &rx->tsinfo;
	uint32_t finfo, status, hsrbp, rxpacc, index;
	uint32_t ampl1, ampl2, ampl3, ttcko, ttcki;
	unsigned int power = 0, noise = 0, snr = 0, psr = 0;
	unsigned int fpnrj = 0, fppwr = 0, fpr = 0;
	cycle_t cc;

	/* Clear Timestamp info */
	memset(tsi, 0, sizeof(*tsi));

	/* Assume frame and timestamp are valid */
	rx->frame_valid = true;
	rx->timestamp_valid = true;

	/* Extract status */
	finfo = le32_to_cpu(rx->finfo);
	status = le32_to_cpu(rx->status);

	/* Extract buffer id */
	hsrbp = DW1000_RX_STATUS_HSRBP(status);

	/* Get 5-byte timestamp */
	cc = le64_to_cpu(rx->time.rx_stamp.cc) & DW1000_TIMESTAMP_MASK;

	/* Check it is really a new value */
	if (unlikely(((cc - dw->rx_timestamp[hsrbp]) & DW1000_TIMESTAMP_MASK)
		     < DW1000_TIMESTAMP_REPETITION_THRESHOLD)) {
		dev_warn_ratelimited(dw->dev, "Frame repetition #0 detected\n");
		rx->timestamp_valid = false;
		rx->frame_valid = false;
		goto invalid;
	}
	if (unlikely(((cc - dw->rx_timestamp[hsrbp^1]) & DW1000_TIMESTAMP_MASK)
		     < DW1000_TIMESTAMP_REPETITION_THRESHOLD)) {
		dev_err_ratelimited(dw->dev, "Frame repetition #1 detected\n");
		rx->timestamp_valid = false;
		rx->frame_valid = false;
		goto invalid;
	}

	/* Remember timestamp */
	dw->rx_timestamp[hsrbp] = cc;

	/* Extract KPIs */
	rxpacc = DW1000_RX_FINFO_RXPACC(finfo);
	index = le16_to_cpu(rx->time.fp_index);
	ampl1 = le16_to_cpu(rx->time.fp_ampl1);
	ampl2 = le16_to_cpu(rx->fqual.fp_ampl2);
	ampl3 = le16_to_cpu(rx->fqual.fp_ampl3);
	power = le16_to_cpu(rx->fqual.cir_pwr);
	noise = le16_to_cpu(rx->fqual.std_noise);
	ttcko = le32_to_cpu(rx->ttcko.tofs);
	ttcki = le32_to_cpu(rx->ttcki.tcki);

	/* Assign timestamp information */
	tsi->noise = noise;
	tsi->rxpacc = rxpacc;
	tsi->fp_index = index;
	tsi->fp_ampl1 = ampl1;
	tsi->fp_ampl2 = ampl2;
	tsi->fp_ampl3 = ampl3;
	tsi->cir_pwr = power;
	tsi->ttcko = ttcko;
	tsi->ttcki = ttcki;

	/* Sanity check */
	if (noise == 0 || power == 0 || rxpacc == 0 || ampl1 == 0) {
		dev_warn_ratelimited(dw->dev, "timestamp ignored: "
				     "invalid KPI values\n");
		goto invalid;
	}

	/* Get (expected) preamble symbol repetition count */
	psr = dw1000_txpsrs[dw->txpsr];

	/* Calculate RX signal estimates */
	fpnrj = ampl1*ampl1 + ampl2*ampl2 + ampl3*ampl3;
	fppwr = fpnrj / (rxpacc*rxpacc);

	/* Calculate a simplistic S/N estimate */
	snr = fppwr / noise;

	/* Calculate a simplistic 8bit FP/Energy estimate */
	fpr = (fpnrj / power) >> (DW1000_RX_CIR_PWR_SCALE - 8);

	/* The reported LQI is upper limited */
	rx->lqi = tsi->lqi = (snr < DW1000_LQI_MAX) ? snr : DW1000_LQI_MAX;

	/* Assign calculated KPIs */
	tsi->fp_pwr = fppwr;
	tsi->snr = snr;
	tsi->fpr = fpr;

	/* Check Preamble length */
	if (rxpacc < psr/DW1000_RXPACC_THRESHOLD) {
		dev_warn_ratelimited(dw->dev, "timestamp ignored: "
				     "RXPACC %u < %u\n",
				     rxpacc, psr/DW1000_RXPACC_THRESHOLD);
		goto invalid;
	}

	/* Check background noise */
	if (noise > dw->noise_threshold) {
		dev_warn_ratelimited(dw->dev, "timestamp ignored: "
				     "NOISE %u > %u\n",
				     noise, dw->noise_threshold);
		goto invalid;
	}

	/* Check S/N Ratio */
	if (snr < dw->snr_threshold) {
		dev_warn_ratelimited(dw->dev, "timestamp ignored: "
				     "S/N %u < %u\n",
				     snr, dw->snr_threshold);
		goto invalid;
	}

	/* Check F/P to Pwr Ratio */
	if (fpr > DW1000_FPR_MAX) {
		dev_warn_ratelimited(dw->dev, "timestamp ignored: "
				     "F/P %u > %u\n",
				     fpr, DW1000_FPR_MAX);
		goto invalid;
	}
	if (fpr < dw->fpr_threshold) {
		dev_warn_ratelimited(dw->dev, "timestamp ignored: "
				     "F/P %u < %u\n",
				     fpr, dw->fpr_threshold);
		goto invalid;
	}

	return;

 invalid:
	/* Timestamp is invalid */
	rx->timestamp_valid = false;

	dev_dbg_ratelimited(dw->dev, "SNR:%u FPR:%u FPPWR:%u POWER:%u NOISE:%u\n",
			    snr, fpr, fppwr, power, noise);
}


/******************************************************************************
 *
 * Transmit datapath
 *
 */

static void dw1000_tx_data_complete(void *context);
static void dw1000_tx_complete(struct dw1000 *dw, struct sk_buff *skb);

/**
 * dw1000_tx_prepare() - Prepare transmit SPI messages
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_tx_prepare(struct dw1000 *dw)
{
	static const uint8_t trxoff = DW1000_SYS_CTRL0_TRXOFF;
	static const uint8_t txstrt = (DW1000_SYS_CTRL0_TXSTRT |
				       DW1000_SYS_CTRL0_WAIT4RESP);
	static const uint8_t txfrs = (DW1000_SYS_STATUS0_TXFRB |
				      DW1000_SYS_STATUS0_TXPRS |
				      DW1000_SYS_STATUS0_TXPHS |
				      DW1000_SYS_STATUS0_TXFRS);
	struct dw1000_tx *tx = &dw->tx;

	/* Initialise transmit descriptor */
	memset(tx, 0, sizeof(*tx));

	/* Prepare data SPI message */
	spi_message_init_no_memset(&tx->data);
	tx->data.complete = dw1000_tx_data_complete;
	tx->data.context = dw;
	dw1000_init_write(&tx->data, &tx->tx_buffer, DW1000_TX_BUFFER, 0,
			  NULL, 0);
	dw1000_init_write(&tx->data, &tx->sys_ctrl_trxoff, DW1000_SYS_CTRL,
			  DW1000_SYS_CTRL0, &trxoff, sizeof(trxoff));
	dw1000_init_write(&tx->data, &tx->tx_fctrl, DW1000_TX_FCTRL,
			  DW1000_TX_FCTRL0, &tx->len, sizeof(tx->len));
	dw1000_init_write(&tx->data, &tx->sys_ctrl_txstrt, DW1000_SYS_CTRL,
			  DW1000_SYS_CTRL0, &txstrt, sizeof(txstrt));
	dw1000_init_read(&tx->data, &tx->sys_ctrl_check, DW1000_SYS_CTRL,
			 DW1000_SYS_CTRL0, &tx->check, sizeof(tx->check));

	/* Prepare information SPI message */
	spi_message_init_no_memset(&tx->info);
	dw1000_init_write(&tx->info, &tx->sys_status, DW1000_SYS_STATUS,
			  DW1000_SYS_STATUS0, &txfrs, sizeof(txfrs));
	dw1000_init_read(&tx->info, &tx->tx_time, DW1000_TX_TIME,
			 DW1000_TX_STAMP, &tx->time.raw, sizeof(tx->time.raw));

	return 0;
}

/**
 * dw1000_tx() - Transmit packet
 *
 * @hw:			IEEE 802.15.4 device
 * @skb:		Socket buffer
 * @return:		0 on success or -errno
 */
static int dw1000_tx(struct ieee802154_hw *hw, struct sk_buff *skb)
{
	struct dw1000 *dw = hw->priv;
	struct dw1000_tx *tx = &dw->tx;
	unsigned long flags;
	int rc;

	/* Acquire TX lock */
	spin_lock_irqsave(&dw->tx_lock, flags);

	/* Sanity check */
	if (tx->skb) {
		dev_err(dw->dev, "concurrent transmit is not supported\n");
		rc = -ENOBUFS;
		goto err_concurrent;
	}

	/* Update data SPI message */
	tx->skb = skb;
	tx->tx_buffer.data.tx_buf = skb->data;
	tx->tx_buffer.data.len = skb->len;
	tx->len = DW1000_TX_FCTRL0_TFLEN(skb->len + IEEE802154_FCS_LEN);
	tx->data_complete = false;
	tx->retries = 0;

	/* Prepare timestamping */
	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_HW_TSTAMP))
		skb_shinfo(skb)->tx_flags |= SKBTX_IN_PROGRESS;
	skb_tx_timestamp(skb);

	/* Initiate data SPI message */
	if ((rc = spi_async(dw->spi, &tx->data)) != 0)
		goto err_spi;

	spin_unlock_irqrestore(&dw->tx_lock, flags);
	return 0;

 err_spi:
	tx->skb = NULL;
 err_concurrent:
	spin_unlock_irqrestore(&dw->tx_lock, flags);
	return rc;
}

/**
 * dw1000_tx_data_complete() - Handle transmit data SPI message completion
 *
 * @context:		DW1000 device
 */
static void dw1000_tx_data_complete(void *context)
{
	struct dw1000 *dw = context;
	struct dw1000_tx *tx = &dw->tx;
	struct sk_buff *skb;
	unsigned long flags;
	int rc;

	/* Acquire TX lock */
	spin_lock_irqsave(&dw->tx_lock, flags);

	/* Complete transmission immediately if data SPI message failed */
	if (unlikely(tx->data.status != 0)) {
		dev_err(dw->dev, "TX data message failed: %d\n",
			tx->data.status);
		goto err_status;
	}

	/* Check that transmit instruction was noticed by the
	 * hardware.  This really ought not to be necessary in any
	 * sane hardware design.
	 */
	if (tx->check & DW1000_SYS_CTRL0_TXSTRT) {
		tx->retries++;
		dev_err_ratelimited(dw->dev, "TX ignored by hardware (%02x) "
				    "on attempt %d\n", tx->check, tx->retries);
		if (tx->retries > DW1000_TX_MAX_RETRIES)
			goto err_check;

		/* Resubmit the transmit instruction (without
		 * repopulating the transmit data buffer).
		 */
		tx->tx_buffer.data.len = 0;
		if ((rc = spi_async(dw->spi, &tx->data)) != 0)
			goto err_retry;
	} else {
		/* Mark data SPI message as complete */
		tx->data_complete = true;
	}

	spin_unlock_irqrestore(&dw->tx_lock, flags);
	return;

 err_retry:
 err_check:
 err_status:
	skb = tx->skb;
	tx->skb = NULL;
	spin_unlock_irqrestore(&dw->tx_lock, flags);
	dw1000_tx_complete(dw, skb);
}

/**
 * dw1000_tx_irq() - Handle Transmit Frame Sent (TXFRS) event
 *
 * @dw:			DW1000 device
 */
static void dw1000_tx_irq(struct dw1000 *dw)
{
	struct dw1000_tx *tx = &dw->tx;
	struct skb_shared_hwtstamps hwtstamps;
	struct sk_buff *skb;
	unsigned long flags;
	int rc;

	/* Ignore if data SPI message has not yet completed */
	spin_lock_irqsave(&dw->tx_lock, flags);
	if (!(tx->skb && tx->data_complete)) {
		dev_err_ratelimited(dw->dev, "spurious TXFRS event\n");
		spin_unlock_irqrestore(&dw->tx_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dw->tx_lock, flags);

	/* Send information SPI message */
	if ((rc = spi_sync(dw->spi, &tx->info)) != 0) {
		dev_err(dw->dev, "TX information message failed: %d\n", rc);
		spin_lock_irqsave(&dw->tx_lock, flags);
		skb = tx->skb;
		tx->skb = NULL;
		spin_unlock_irqrestore(&dw->tx_lock, flags);
		dw1000_tx_complete(dw, skb);
		return;
	}

	/* Record hardware timestamp, if applicable */
	spin_lock_irqsave(&dw->tx_lock, flags);
	skb = tx->skb;
	tx->skb = NULL;
	if (unlikely(skb_shinfo(skb)->tx_flags & SKBTX_IN_PROGRESS)) {
		dw1000_timestamp(dw, &tx->time, &hwtstamps);
		dw1000_tsinfo(dw, &tx->time, &tx->tsinfo, &hwtstamps);
		skb_tstamp_tx(skb, &hwtstamps);
	}
	spin_unlock_irqrestore(&dw->tx_lock, flags);

	/* Report successful completion */
	dw1000_tx_complete(dw, skb);
}

/**
 * dw1000_tx_complete() - Complete transmission
 *
 * @dw:			DW1000 device
 * @skb:		Socket buffer
 */
static void dw1000_tx_complete(struct dw1000 *dw, struct sk_buff *skb)
{
	size_t sifs_max_len;

	/* Sanity check */
	if (unlikely(!skb)) {
		dev_err(dw->dev, "spurious TX completion\n");
		return;
	}

	/* Report completion to IEEE 802.15.4 stack */
	sifs_max_len = (IEEE802154_MAX_SIFS_FRAME_SIZE - IEEE802154_FCS_LEN);
	ieee802154_xmit_complete(dw->hw, skb, (skb->len <= sifs_max_len));
}

/******************************************************************************
 *
 * Receive datapath
 *
 */

/**
 * dw1000_rx_prepare() - Prepare receive SPI messages
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_rx_prepare(struct dw1000 *dw)
{
	static const uint8_t hrbpt = DW1000_SYS_CTRL3_HRBPT;
	static const uint8_t rxena = DW1000_SYS_CTRL1_RXENAB;
	static const uint32_t clear = cpu_to_le32(DW1000_RX_STATE_CLEAR);
	static const uint32_t mask1 = cpu_to_le32(DW1000_SYS_MASK_MACTIVE);
	static const uint32_t mask0 = 0;

	struct dw1000_rx *rx = &dw->rx;

	/* Initialise receive descriptor */
	memset(rx, 0, sizeof(*rx));

	/* Prepare information SPI message */
	spi_message_init_no_memset(&rx->info);
	dw1000_init_read(&rx->info, &rx->rx_finfo, DW1000_RX_FINFO, 0,
			 &rx->finfo, sizeof(rx->finfo));
	dw1000_init_read(&rx->info, &rx->rx_time, DW1000_RX_TIME, 0,
			 &rx->time, sizeof(rx->time));
	dw1000_init_read(&rx->info, &rx->rx_fqual, DW1000_RX_FQUAL, 0,
			 &rx->fqual, sizeof(rx->fqual));
	dw1000_init_read(&rx->info, &rx->rx_ttcki, DW1000_RX_TTCKI, 0,
			 &rx->ttcki, sizeof(rx->ttcki.raw));
	dw1000_init_read(&rx->info, &rx->rx_ttcko, DW1000_RX_TTCKO, 0,
			 &rx->ttcko, sizeof(rx->ttcko.raw));
	dw1000_init_read(&rx->info, &rx->dig_diag, DW1000_DIG_DIAG,
			 DW1000_EVC_OVR, &rx->evc_ovr, sizeof(rx->evc_ovr));
	dw1000_init_read(&rx->info, &rx->sys_status, DW1000_SYS_STATUS, 0,
			 &rx->status, sizeof(rx->status));

	/* Prepare data SPI message */
	spi_message_init_no_memset(&rx->data);
	dw1000_init_read(&rx->data, &rx->rx_buffer, DW1000_RX_BUFFER, 0,
			 NULL, 0);
	dw1000_init_write(&rx->data, &rx->sys_ctrl, DW1000_SYS_CTRL,
			  DW1000_SYS_CTRL3, &hrbpt, sizeof(hrbpt));

	/* Prepare recovery SPI message */
	spi_message_init_no_memset(&rx->rcvr);
	dw1000_init_write(&rx->rcvr, &rx->rv_mask0, DW1000_SYS_MASK,
			  0, &mask0, sizeof(mask0));
	dw1000_init_write(&rx->rcvr, &rx->rv_status, DW1000_SYS_STATUS,
			  0, &clear, sizeof(clear));
	dw1000_init_write(&rx->rcvr, &rx->rv_mask1, DW1000_SYS_MASK,
			  0, &mask1, sizeof(mask1));
	dw1000_init_write(&rx->rcvr, &rx->rv_hrbpt, DW1000_SYS_CTRL,
			  DW1000_SYS_CTRL3, &hrbpt, sizeof(hrbpt));
	dw1000_init_write(&rx->rcvr, &rx->rv_rxenab, DW1000_SYS_CTRL,
			  DW1000_SYS_CTRL1, &rxena, sizeof(rxena));

	return 0;
}

/**
 * dw1000_rx_irq() - Handle Receiver Data Frame Ready (RXDFR) event
 *
 * @dw:			DW1000 device
 */
static void dw1000_rx_irq(struct dw1000 *dw)
{
	struct dw1000_rx *rx = &dw->rx;
	struct sk_buff *skb;
	uint32_t finfo, status, overruns;
	size_t len;
	int rc;

	/* Send information SPI message */
	if ((rc = spi_sync(dw->spi, &rx->info)) != 0) {
		dev_err(dw->dev, "RX information message failed: %d\n", rc);
		goto err_info;
	}

	/* The double buffering implementation in the DW1000 is
	 * somewhat flawed.  A packet arriving while there are no
	 * receive buffers available will be discarded, but only after
	 * corrupting the RX_FINFO, RX_TIME, and RX_FQUAL registers.
	 *
	 * We therefore read those registers first, then re-read the
	 * SYS_STATUS register.  If overrun is detected then we must
	 * discard the packet: even though the received data is still
	 * available, we cannot know its length.  If overrun is not
	 * detected at this point then we are safe to read the
	 * remaining data even if a subsequent overrun occurs before
	 * our data read is complete.
	 *
	 * The best way to detect an overrun is to check the
	 * HSRBP/ICRBP sync. When entering the RX interrupt, the
	 * RBP bits should have the opposite values. Once the RX is
	 * performed, and the buffer toggled, the RBP bits should
	 * have equal values. If this is not the case, recovery
	 * procedure is executed.
	 *
	 * We explicitly do not follow Decawave's documented
	 * instruction to reset the receiver when an overrun occurs.
	 * Resetting the receiver is highly disruptive to both the
	 * receive and transmit datapaths, tends to cause further
	 * receive overruns due to the time taken for the reset, and
	 * often results in a dead receiver since the post-reset
	 * RXENAB command is frequently ignored by the hardware.
	 *
	 * Instead, we simply discard the received frame and toggle
	 * HRBPT as for a normally received frame.
	 */

	status = le32_to_cpu(rx->status);

	/* RX out-of-sync check */
	if (unlikely(!DW1000_HSRPB_SYNC(status))) {
		dev_warn_ratelimited(dw->dev, "RX sync lost after %d frames; "
				    "recovering\n", dw->rx_sync_cnt);
		goto err_recover2;
	}

	/* RX overruns check */
	overruns = le16_to_cpu(rx->evc_ovr);
	if (unlikely(overruns != dw->rx_overruns)) {
		dev_warn_ratelimited(dw->dev, "RX overruns (%d) detected; "
				     "recovering\n",
				     ((overruns - dw->rx_overruns) &
				      DW1000_EVC_OVR_MASK));
		dw->rx_overruns = overruns;
		goto err_recover;
        }

	/* Double buffering overrun check */
	if (unlikely(status & DW1000_SYS_STATUS_RXOVRR)) {
		dev_warn_ratelimited(dw->dev, "RX overrun detected; "
				     "recovering\n");
		goto err_recover;
	}

	/* Data frame ready not set? */
	if (unlikely((status & DW1000_SYS_STATUS_RXDFR) == 0)) {
		dev_warn_ratelimited(dw->dev, "RXDFR not set; recovering\n");
		goto err_recover;
	}

	/* Estimate link quality */
	dw1000_rx_link_qual(dw);

	/* Frame is invalid */
	if (!rx->frame_valid)
		goto err_recover;

	/* Increment RX sync counter */
	dw->rx_sync_cnt++;

	/* Frame length */
	finfo = le32_to_cpu(rx->finfo);
	len = DW1000_RX_FINFO_RXFLEN(finfo);

	/* Allocate socket buffer */
	skb = dev_alloc_skb(len);
	if (!skb) {
		dev_err(dw->dev, "RX buffer allocation failed\n");
		goto err_alloc;
	}
	skb_put(skb, len);

	/* Record hardware timestamp, if viable */
	if (rx->timestamp_valid)
		dw1000_timestamp(dw, &rx->time.rx_stamp, skb_hwtstamps(skb));

	/* Record timestamp info */
	dw1000_tsinfo(dw, &rx->time.rx_stamp, &rx->tsinfo, skb_hwtstamps(skb));

	/* Update data SPI message */
	rx->rx_buffer.data.rx_buf = skb->data;
	rx->rx_buffer.data.len = len;

	/* Send data SPI message */
	if ((rc = spi_sync(dw->spi, &rx->data)) != 0) {
		dev_err(dw->dev, "RX data message failed: %d\n", rc);
		goto err_data;
	}

	/* Hand off to IEEE 802.15.4 stack */
	ieee802154_rx_irqsafe(dw->hw, skb, rx->lqi);
	return;

 err_data:
	dev_kfree_skb_any(skb);
 err_alloc:
 err_info:
	return;

 err_recover2:
	/* Reset RX sync counter */
	dw->rx_sync_cnt = 0;

	/* Send recovery SPI message */
	if ((rc = spi_sync(dw->spi, &rx->rcvr)) != 0) {
		dev_err(dw->dev, "RX recovery#2 failed: %d\n", rc);
	}

 err_recover:
	/* Send recovery SPI message */
	if ((rc = spi_sync(dw->spi, &rx->rcvr)) != 0) {
		dev_err(dw->dev, "RX recovery#1 failed: %d\n", rc);
	}
}

/******************************************************************************
 *
 * Interrupt handling
 *
 */

/**
 * dw1000_isr_thread() - Interrupt handler thread
 *
 * @irq:		Interrupt number
 * @context:		DW1000 device
 * @return:		IRQ status
 */
static irqreturn_t dw1000_isr_thread(int irq, void *context)
{
	struct dw1000 *dw = context;
	unsigned int sys_status;
	int rc;

	/* Read interrupt status register */
	if ((rc = regmap_read(dw->sys_status.regs, 0, &sys_status)) != 0)
		goto abort;

	/* Handle transmit completion, if applicable */
	if (sys_status & DW1000_SYS_STATUS_TXFRS)
		dw1000_tx_irq(dw);

	/* Handle received packet or receive overrun, if applicable */
	if (sys_status & (DW1000_SYS_STATUS_RXDFR | DW1000_SYS_STATUS_RXOVRR))
		dw1000_rx_irq(dw);

 abort:
	return IRQ_HANDLED;
}


/******************************************************************************
 *
 * IEEE 802.15.4 interface
 *
 */

/**
 * dw1000_start() - Start device operation
 *
 * @hw:			IEEE 802.15.4 device
 * @return:		0 on success or -errno
 */
static int dw1000_start(struct ieee802154_hw *hw)
{
	struct dw1000 *dw = hw->priv;
	int sys_status;
	int rc;

	/* Prepare transmit descriptor */
	if ((rc = dw1000_tx_prepare(dw)) != 0) {
		dev_err(dw->dev, "TX preparation failed: %d\n", rc);
		goto err_tx_prepare;
	}

	/* Prepare receive descriptor */
	if ((rc = dw1000_rx_prepare(dw)) != 0) {
		dev_err(dw->dev, "RX preparation failed: %d\n", rc);
		goto err_rx_prepare;
	}

	/* Ensure that HSRBP and ICRBP are in sync */
	if ((rc = regmap_read(dw->sys_status.regs, 0, &sys_status)) != 0)
		goto err_sys_status;
	if (!DW1000_HSRPB_SYNC(sys_status)) {
		if ((rc = regmap_write(dw->sys_ctrl.regs, DW1000_SYS_CTRL3,
				       DW1000_SYS_CTRL3_HRBPT)) != 0)
			goto err_sys_ctrl3;
	}

	/* Enable interrupt generation */
	if ((rc = regmap_write(dw->sys_mask.regs, 0,
			       DW1000_SYS_MASK_MACTIVE)) != 0)
		goto err_sys_mask;

	/* Enable receiver */
	if ((rc = regmap_write(dw->sys_ctrl.regs, DW1000_SYS_CTRL1,
			       DW1000_SYS_CTRL1_RXENAB)) != 0)
		goto err_sys_ctrl1;

	dev_info(dw->dev, "started\n");
	return 0;

	regmap_write(dw->sys_ctrl.regs, DW1000_SYS_CTRL0,
		     DW1000_SYS_CTRL0_TRXOFF);
 err_sys_ctrl1:
	regmap_write(dw->sys_mask.regs, 0, 0);
 err_sys_mask:
 err_sys_ctrl3:
 err_sys_status:
 err_rx_prepare:
 err_tx_prepare:
	return rc;
}

/**
 * dw1000_stop() - Stop device operation
 *
 * @hw:			IEEE 802.15.4 device
 */
static void dw1000_stop(struct ieee802154_hw *hw)
{
	struct dw1000 *dw = hw->priv;
	int rc;

	/* Disable further interrupt generation */
	if ((rc = regmap_write(dw->sys_mask.regs, 0, 0)) != 0) {
		dev_err(dw->dev, "could not disable interrupts: %d\n", rc);
	}

	/* Complete any pending interrupt work */
	synchronize_irq(dw->spi->irq);

	/* Disable receiver and transmitter */
	if ((rc = regmap_write(dw->sys_ctrl.regs, DW1000_SYS_CTRL0,
			       DW1000_SYS_CTRL0_TRXOFF)) != 0) {
		dev_err(dw->dev, "could not disable TX/RX: %d\n", rc);
	}

	dev_info(dw->dev, "stopped\n");
}

/**
 * dw1000_ed() - Detect energy on current radio channel
 *
 * @hw:			IEEE 802.15.4 device
 * @level:		Energy level to fill in (in arbitrary units)
 * @return:		0 on success or -errno
 */
static int dw1000_ed(struct ieee802154_hw *hw, u8 *level)
{
	struct dw1000 *dw = hw->priv;
	__le32 agc_stat1 = 0;
	unsigned int edg1;
	unsigned int edv2;
	int rc;

	/* Read AGC_STAT1 register */
	if ((rc = regmap_raw_read(dw->agc_ctrl.regs, DW1000_AGC_STAT1,
				  &agc_stat1, DW1000_AGC_STAT1_LEN)) != 0)
		return rc;

	/* Calculate energy level as per datasheet.  Given the limited
	 * range available for the answer, we use a logarithmic scale
	 * and some approximations.
	 */
	edg1 = DW1000_AGC_STAT1_EDG1(le32_to_cpu(agc_stat1));
	edv2 = DW1000_AGC_STAT1_EDV2(le32_to_cpu(agc_stat1));
	if (edv2 < DW1000_EDV2_MIN) {
		*level = 0;
	} else {
		*level = (fls(edv2 - DW1000_EDV2_MIN) +
			  ((edg1 * DW1000_EDG1_MULT) >> DW1000_EDG1_SHIFT));
	}

	dev_dbg(dw->dev, "detected energy level %d on channel %d (EDG1=%d, "
		"EDV2=%d)\n", *level, dw->phy->current_channel, edg1, edv2);
	return 0;
}

/**
 * dw1000_set_channel() - Set radio channel
 *
 * @hw:			IEEE 802.15.4 device
 * @page:		Channel page (must be UWB)
 * @channel:		Channel number
 * @return:		0 on success or -errno
 */
static int dw1000_set_channel(struct ieee802154_hw *hw, u8 page, u8 channel)
{
	struct dw1000 *dw = hw->priv;
	int rc;

	/* Sanity checks */
	if (page != DW1000_CHANNEL_PAGE)
		return -EINVAL;

	/* Configure radio channel */
	if ((rc = dw1000_configure_channel(dw, channel)) != 0)
		return rc;

	return 0;
}

/**
 * dw1000_set_hw_addr_filt() - Set hardware address filters
 *
 * @hw:			IEEE 802.15.4 device
 * @filt:		Hardware address filters
 * @changed:		Change bitmask
 * @return:		0 on success or -errno
 */
static int dw1000_set_hw_addr_filt(struct ieee802154_hw *hw,
				   struct ieee802154_hw_addr_filt *filt,
				   unsigned long changed)
{
	struct dw1000 *dw = hw->priv;
	unsigned int pan_id;
	unsigned int short_addr;
	int rc;

	/* Set PAN ID */
	if (changed & IEEE802154_AFILT_PANID_CHANGED) {
		pan_id = le16_to_cpu(filt->pan_id);
		if ((rc = regmap_write(dw->panadr.regs, DW1000_PANADR_PAN_ID,
				       pan_id)) != 0)
			return rc;
		dev_dbg(dw->dev, "set PAN ID %04x\n", pan_id);
	}

	/* Set short address */
	if (changed & IEEE802154_AFILT_SADDR_CHANGED) {
		short_addr = le16_to_cpu(filt->short_addr);
		if ((rc = regmap_write(dw->panadr.regs,
				       DW1000_PANADR_SHORT_ADDR,
				       short_addr)) != 0)
			return rc;
		dev_dbg(dw->dev, "set short address %04x\n", short_addr);
	}

	/* Set EUI-64 */
	if (changed & IEEE802154_AFILT_IEEEADDR_CHANGED) {
		if ((rc = regmap_raw_write(dw->eui.regs, 0, &filt->ieee_addr,
					   sizeof(filt->ieee_addr))) != 0)
			return rc;
		dev_dbg(dw->dev, "set EUI64 %016llx\n",
			le64_to_cpu(filt->ieee_addr));
	}

	/* Set coordinator status */
	if (changed & IEEE802154_AFILT_PANC_CHANGED) {
		if ((rc = regmap_update_bits(dw->sys_cfg.regs, 0,
					     DW1000_SYS_CFG_FFBC,
					     (filt->pan_coord ?
					      DW1000_SYS_CFG_FFBC : 0))) != 0)
			return rc;
		dev_dbg(dw->dev, "PAN coordinator role %sabled\n",
			(filt->pan_coord ? "en" : "dis"));
	}

	return 0;
}

/**
 * dw1000_set_promiscuous_mode() - Enable/disable promiscuous mode
 *
 * @hw:			IEEE 802.15.4 device
 * @on:			Enable/disable promiscuous mode
 * @return:		0 on success or -errno
 */
static int dw1000_set_promiscuous_mode(struct ieee802154_hw *hw, bool on)
{
	struct dw1000 *dw = hw->priv;
	int rc;

	/* Enable/disable frame filtering */
	if ((rc = regmap_update_bits(dw->sys_cfg.regs, 0, DW1000_SYS_CFG_FFEN,
				     (on ? 0 : DW1000_SYS_CFG_FFEN))) != 0)
		return rc;
	dev_dbg(dw->dev, "promiscuous mode %sabled\n", (on ? "en" : "dis"));

	return 0;
}

/* IEEE 802.15.4 operations */
static const struct ieee802154_ops dw1000_ops = {
	.owner = THIS_MODULE,
	.start = dw1000_start,
	.stop = dw1000_stop,
	.xmit_async = dw1000_tx,
	.ed = dw1000_ed,
	.set_channel = dw1000_set_channel,
	.set_hw_addr_filt = dw1000_set_hw_addr_filt,
	.set_promiscuous_mode = dw1000_set_promiscuous_mode,
};

/******************************************************************************
 *
 * Hardware monitor
 *
 */

/**
 * dw1000_hwmon_is_visible() - Check attribute visibility
 *
 * @drvdata:		DW1000 device
 * @type:		Sensor type
 * @attr:		Sensor attribute
 * @channel:		Channel number
 * @return:		File mode
 */
static umode_t dw1000_hwmon_is_visible(const void *drvdata,
				       enum hwmon_sensor_types type,
				       uint32_t attr, int channel)
{
	/* All attributes are read-only */
	return 0444;
}

/**
 * dw1000_hwmon_read() - Read attribute
 *
 * @dev:		Hardware monitor device
 * @type:		Sensor type
 * @attr:		Sensor attribute
 * @channel:		Channel number
 * @val:		Value to fill in
 * @return:		0 on success or -errno
 */
static int dw1000_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     uint32_t attr, int channel, long *val)
{
	struct dw1000 *dw = dev_get_drvdata(dev);
	static const uint8_t hack_a1 = DW1000_RF_SENSOR_HACK_A1;
	static const uint8_t hack_b1 = DW1000_RF_SENSOR_HACK_B1;
	static const uint8_t hack_b2 = DW1000_RF_SENSOR_HACK_B2;
	static const uint8_t sarc_on = DW1000_TC_SARC_CTRL;
	static const uint8_t sarc_off = 0;
	struct {
		struct spi_message msg;
		struct dw1000_spi_transfers hack_a1;
		struct dw1000_spi_transfers hack_b1;
		struct dw1000_spi_transfers hack_b2;
		struct dw1000_spi_transfers sarc_on;
		struct dw1000_spi_transfers sarc_off;
		struct dw1000_spi_transfers sarl;
	} spi;
	uint8_t sarl;
	int rc;

	/* Construct SPI message */
	memset(&spi, 0, sizeof(spi));
	spi_message_init_no_memset(&spi.msg);
	dw1000_init_write(&spi.msg, &spi.hack_a1, DW1000_RF_CONF,
			  DW1000_RF_SENSOR_HACK_A, &hack_a1, sizeof(hack_a1));
	dw1000_init_write(&spi.msg, &spi.hack_b1, DW1000_RF_CONF,
			  DW1000_RF_SENSOR_HACK_B, &hack_b1, sizeof(hack_b1));
	dw1000_init_write(&spi.msg, &spi.hack_b2, DW1000_RF_CONF,
			  DW1000_RF_SENSOR_HACK_B, &hack_b2, sizeof(hack_b2));
	dw1000_init_write(&spi.msg, &spi.sarc_on, DW1000_TX_CAL,
			  DW1000_TC_SARC, &sarc_on, sizeof(sarc_on));
	spi.sarc_on.data.delay_usecs = DW1000_SAR_WAIT_US;
	dw1000_init_write(&spi.msg, &spi.sarc_off, DW1000_TX_CAL,
			  DW1000_TC_SARC, &sarc_off, sizeof(sarc_off));
	dw1000_init_read(&spi.msg, &spi.sarl, DW1000_TX_CAL,
			 ((type == hwmon_in) ?
			  DW1000_TC_SARL_LVBAT : DW1000_TC_SARL_LTEMP),
			 &sarl, sizeof(sarl));

	/* Send SPI message */
	if ((rc = spi_sync(dw->spi, &spi.msg)) != 0)
		return rc;

	/* Convert result */
	switch (type) {
	case hwmon_in:
		*val = DW1000_SAR_VBAT_MVOLT(sarl, dw->vmeas_3v3);
		dev_dbg(dw->dev, "voltage %#x (%#x @ 3.3V) is %ldmV\n",
			sarl, dw->vmeas_3v3, *val);
		break;
	case hwmon_temp:
		*val = DW1000_SAR_TEMP_MDEGC(sarl, dw->tmeas_23c);
		dev_dbg(dw->dev, "temperature %#x (%#x @ 23degC) is %ldmdegC\n",
			sarl, dw->tmeas_23c, *val);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* Hardware monitor voltage configuration */
static const uint32_t dw1000_in_config[] = {
	HWMON_T_INPUT,
	0
};

/* Hardware monitor voltage channel */
static const struct hwmon_channel_info dw1000_in = {
	.type = hwmon_in,
	.config = dw1000_in_config,
};

/* Hardware monitor temperature configuration */
static const uint32_t dw1000_temp_config[] = {
	HWMON_T_INPUT,
	0
};

/* Hardware monitor temperature channel */
static const struct hwmon_channel_info dw1000_temp = {
	.type = hwmon_temp,
	.config = dw1000_temp_config,
};

/* Hardware monitor channels */
static const struct hwmon_channel_info *dw1000_hwmon_channel[] = {
	&dw1000_in,
	&dw1000_temp,
	NULL
};

/* Hardware monitor operations */
static const struct hwmon_ops dw1000_hwmon_ops = {
	.is_visible = dw1000_hwmon_is_visible,
	.read = dw1000_hwmon_read,
};

/* Hardware monitor information */
static const struct hwmon_chip_info dw1000_hwmon_chip = {
	.info = dw1000_hwmon_channel,
	.ops = &dw1000_hwmon_ops,
};

/******************************************************************************
 *
 * Device attributes
 *
 */

#define DW1000_ATTR_RW(_name, _mode) \
	DEVICE_ATTR(_name, _mode, dw1000_show_##_name, dw1000_store_##_name)

#define DW1000_ATTR_RO(_name, _mode) \
	DEVICE_ATTR(_name, _mode, dw1000_show_##_name, NULL)

/* XTAL trim */
static ssize_t dw1000_show_xtalt(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw->xtalt);
}
static ssize_t dw1000_store_xtalt(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	unsigned int trim;
	int rc;

	if ((rc = kstrtouint(buf, 0, &trim)) != 0)
		return rc;
	if ((rc = dw1000_configure_xtalt(dw, trim)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(xtalt, 0644);

/* Channel */
static ssize_t dw1000_show_channel(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw->channel);
}
static DW1000_ATTR_RO(channel, 0444);

/* Preamble code */
static ssize_t dw1000_show_pcode(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw1000_pcode(dw));
}
static ssize_t dw1000_store_pcode(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	int pcode;
	int rc;

	if ((rc = kstrtoint(buf, 0, &pcode)) != 0)
		return rc;
	if ((rc = dw1000_configure_pcode(dw, pcode)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(pcode, 0644);

/* Pulse repetition frequency */
static ssize_t dw1000_show_prf(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw1000_prfs[dw->prf]);
}
static ssize_t dw1000_store_prf(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	int raw;
	int prf;
	int rc;

	if ((rc = kstrtoint(buf, 0, &raw)) != 0)
		return rc;
	if ((prf = dw1000_lookup_prf(raw)) < 0)
		return prf;
	if ((rc = dw1000_configure_prf(dw, prf)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(prf, 0644);

/* Antenna delay */
static ssize_t dw1000_show_antd(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "0x%4x\n", dw->antd[dw->prf]);
}
static ssize_t dw1000_store_antd(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	unsigned int delay;
	int rc;

	if ((rc = kstrtouint(buf, 0, &delay)) != 0)
		return rc;
	if ((rc = dw1000_configure_antd(dw, delay)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(antd, 0644);

/* Data rate */
static ssize_t dw1000_show_rate(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw1000_rates[dw->rate]);
}
static ssize_t dw1000_store_rate(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	int raw;
	int rate;
	int rc;

	if ((rc = kstrtoint(buf, 0, &raw)) != 0)
		return rc;
	if ((rate = dw1000_lookup_rate(raw)) < 0)
		return rate;
	if ((rc = dw1000_configure_rate(dw, rate)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(rate, 0644);

/* Transmit preamble symbol repetitions */
static ssize_t dw1000_show_txpsr(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw1000_txpsrs[dw1000_txpsr(dw)]);
}
static ssize_t dw1000_store_txpsr(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	int raw;
	int txpsr;
	int rc;

	if ((rc = kstrtoint(buf, 0, &raw)) != 0)
		return rc;
	if ((txpsr = dw1000_lookup_txpsr(raw)) < 0)
		return txpsr;
	if ((rc = dw1000_configure_txpsr(dw, txpsr)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(txpsr, 0644);

/* Transmission power */
static ssize_t dw1000_show_tx_power(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "0x%02x%02x%02x%02x\n",
		       dw->txpwr[3], dw->txpwr[2], dw->txpwr[1], dw->txpwr[0]);
}
static ssize_t dw1000_store_tx_power(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	unsigned power;
	int rc;

	if (kstrtouint(buf, 0, &power) < 0)
		return -EINVAL;
	if ((rc = dw1000_configure_tx_power(dw, power)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(tx_power, 0644);

/* Smart power control */
static ssize_t dw1000_show_smart_power(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw->smart_power);
}
static ssize_t dw1000_store_smart_power(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	bool smart_power;
	int rc;

	if (strtobool(buf, &smart_power) < 0)
		return -EINVAL;
	if ((rc = dw1000_configure_smart_power(dw, smart_power)) != 0)
		return rc;
	return count;
}
static DW1000_ATTR_RW(smart_power, 0644);

/* Signal to Noise Ratio indicator threshold */
static ssize_t dw1000_show_snr_threshold(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw->snr_threshold);
}
static ssize_t dw1000_store_snr_threshold(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	unsigned int snr_threshold;

	if (kstrtouint(buf, 0, &snr_threshold) < 0)
		return -EINVAL;
	dw->snr_threshold = snr_threshold;
	return count;
}
static DW1000_ATTR_RW(snr_threshold, 0644);

/* First Path to Total Energy Ratio indicator threshold */
static ssize_t dw1000_show_fpr_threshold(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw->fpr_threshold);
}
static ssize_t dw1000_store_fpr_threshold(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	unsigned int fpr_threshold;

	if (kstrtouint(buf, 0, &fpr_threshold) < 0)
		return -EINVAL;
	dw->fpr_threshold = fpr_threshold;
	return count;
}
static DW1000_ATTR_RW(fpr_threshold, 0644);

/* Noise threshold */
static ssize_t dw1000_show_noise_threshold(struct device *dev,
					   struct device_attribute *attr,
					   char *buf)
{
	struct dw1000 *dw = to_dw1000(dev);

	return sprintf(buf, "%d\n", dw->noise_threshold);
}
static ssize_t dw1000_store_noise_threshold(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct dw1000 *dw = to_dw1000(dev);
	unsigned int noise_threshold;

	if (kstrtouint(buf, 0, &noise_threshold) < 0)
		return -EINVAL;
	dw->noise_threshold = noise_threshold;
	return count;
}
static DW1000_ATTR_RW(noise_threshold, 0644);

/* Attribute list */
static struct attribute *dw1000_attrs[] = {
	&dev_attr_xtalt.attr,
	&dev_attr_channel.attr,
	&dev_attr_pcode.attr,
	&dev_attr_prf.attr,
	&dev_attr_antd.attr,
	&dev_attr_rate.attr,
	&dev_attr_txpsr.attr,
	&dev_attr_tx_power.attr,
	&dev_attr_smart_power.attr,
	&dev_attr_snr_threshold.attr,
	&dev_attr_fpr_threshold.attr,
	&dev_attr_noise_threshold.attr,
	NULL
};

/* Attribute group */
static struct attribute_group dw1000_attr_group = {
	.name = "dw1000",
	.attrs = dw1000_attrs,
};

/******************************************************************************
 *
 * Device initialisation
 *
 */

/**
 * dw1000_check_dev_id() - Check device ID
 *
 * The register ID tag portion of the device ID has a fixed value.
 * This can be used to verify that the SPI bus is operational and that
 * the DW1000 device is present and responding to commands.
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_check_dev_id(struct dw1000 *dw)
{
	int ridtag;
	int rc;

	/* Read device ID */
	if ((rc = regmap_read(dw->dev_id.regs, DW1000_RIDTAG, &ridtag)) != 0) {
		dev_err(dw->dev, "could not read device ID: %d\n", rc);
		return rc;
	}

	/* Check register ID tag */
	if (ridtag != DW1000_RIDTAG_MAGIC) {
		dev_err(dw->dev, "incorrect RID tag %04X (expected %04X)\n",
			ridtag, DW1000_RIDTAG_MAGIC);
		return -EINVAL;
	}

	return 0;
}

/**
 * dw1000_reset() - Reset device
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_reset(struct dw1000 *dw)
{
	int rc;

	/* Power on the device */
	if (gpio_is_valid(dw->power_gpio)) {
		gpio_set_value(dw->power_gpio, 1);
		msleep(DW1000_POWER_ON_DELAY);
	}

	/* Release RESET GPIO */
	if (gpio_is_valid(dw->reset_gpio)) {
		gpio_set_value(dw->reset_gpio, 1);
		msleep(DW1000_HARD_RESET_DELAY);
	}

	/* Force slow SPI clock speed */
	dw->spi->max_speed_hz = DW1000_SPI_SLOW_HZ;

	/* Read device ID register */
	if ((rc = dw1000_check_dev_id(dw)) != 0)
		return rc;

	/* Force system clock to 19.2 MHz XTI clock */
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     DW1000_PMSC_CTRL0_SYSCLKS_MASK,
				     DW1000_PMSC_CTRL0_SYSCLKS_SLOW)) != 0)
		return rc;

	/* Initiate reset */
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     DW1000_PMSC_CTRL0_SOFTRESET_MASK, 0)) != 0)
		return rc;

	/* Clear reset */
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     DW1000_PMSC_CTRL0_SOFTRESET_MASK,
				     DW1000_PMSC_CTRL0_SOFTRESET_MASK)) != 0)
		return rc;

	/* Recheck device ID to ensure bus is still operational */
	if ((rc = dw1000_check_dev_id(dw)) != 0)
		return rc;

	/* Switch to full SPI clock speed */
	dw->spi->max_speed_hz = DW1000_SPI_FAST_HZ;

	/* Recheck device ID to ensure bus is still operational */
	if ((rc = dw1000_check_dev_id(dw)) != 0)
		return rc;

	return 0;
}

/**
 * dw1000_load_xtalt() - Load XTALT calibration value
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_load_xtalt(struct dw1000 *dw)
{
	uint32_t value;
	uint8_t xtalt;
	int rc;

	/* Default value for XTALT */
	xtalt = DW1000_FS_XTALT_XTALT_MIDPOINT;

	/* Read XTAL_Trim calibration value from OTP */
	if ((rc = regmap_read(dw->otp, DW1000_OTP_REV_XTALT, &value)) != 0)
		return rc;
	if (DW1000_OTP_XTALT(value) != 0 && DW1000_OTP_XTALT(value) != 0x1f)
		xtalt = DW1000_OTP_XTALT(value);

	/* Read XTALT calibration value from devicetree */
	if (of_property_read_u32(dw->dev->of_node, "decawave,xtalt",
				 &value) == 0) {
		/* Check the value range */
		if ((value & ~DW1000_FS_XTALT_XTALT_MASK) == 0)
			xtalt = value;
		else
			dev_err(dw->dev, "invalid decawave,xtalt "
				"value 0x%02x\n", value);
	}

	dw->xtalt = xtalt;

	dev_dbg(dw->dev, "set xtal trim 0x%02x\n", xtalt);
	return 0;
}

/**
 * dw1000_load_ldotune() - Load LDOTUNE calibration value
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_load_ldotune(struct dw1000 *dw)
{
	union dw1000_ldotune ldotune;
	const uint8_t *of_ldotune;
	unsigned int i;
	int len;
	int rc;

	/* Read LDOTUNE calibration value from OTP */
	if ((rc = regmap_bulk_read(dw->otp, DW1000_OTP_LDOTUNE, ldotune.otp,
				   ARRAY_SIZE(ldotune.otp))) != 0)
		return rc;
	for (i = 0; i < ARRAY_SIZE(ldotune.otp); i++)
		cpu_to_le32s(ldotune.otp[i]);

	/* Read LDOTUNE calibration value from devicetree */
	of_ldotune = of_get_property(dw->dev->of_node, "decawave,ldotune",
				     &len);
	if (of_ldotune) {
		if (len == sizeof(ldotune.raw)) {
			for (i = 0; i < len; i++)
				ldotune.raw[i] = of_ldotune[len - i - 1];
		} else {
			dev_err(dw->dev, "invalid decawave,ldotune length %d\n",
				len);
		}
	}

	/* Apply LDOTUNE calibration value, if applicable */
	if (ldotune.raw[0]) {
		dev_info(dw->dev, "set LDOTUNE %02x:%02x:%02x:%02x:%02x\n",
			 ldotune.raw[4], ldotune.raw[3], ldotune.raw[2],
			 ldotune.raw[1], ldotune.raw[0]);
		if ((rc = regmap_raw_write(dw->rf_conf.regs, DW1000_RF_LDOTUNE,
					   &ldotune.raw,
					   sizeof(ldotune.raw))) != 0)
		    return rc;
	}

	return 0;
}

/**
 * dw1000_load_eui64() - Load EUI-64 extended address
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_load_eui64(struct dw1000 *dw)
{
	union dw1000_eui64 eui64;
	const uint8_t *of_eui64;
	unsigned int i;
	int len;
	int rc;

	/* Read EUI-64 address from OTP */
	if ((rc = regmap_bulk_read(dw->otp, DW1000_OTP_EUI64, eui64.otp,
				   ARRAY_SIZE(eui64.otp))) != 0)
		return rc;
	for (i = 0; i < ARRAY_SIZE(eui64.otp); i++)
		cpu_to_le32s(eui64.otp[i]);

	/* Read EUI-64 address from devicetree */
	of_eui64 = of_get_property(dw->dev->of_node, "decawave,eui64", &len);
	if (of_eui64) {
		if (len == sizeof(eui64.raw)) {
			for (i = 0; i < len; i++)
				eui64.raw[i] = of_eui64[len - i - 1];
		} else {
			dev_err(dw->dev, "invalid decawave,eui64 length %d\n",
				len);
		}
	}

	/* Use EUI-64 address if valid, otherwise generate random address */
	if (ieee802154_is_valid_extended_unicast_addr(eui64.addr)) {
		dw->phy->perm_extended_addr = eui64.addr;
	} else {
		dev_warn(dw->dev, "has no permanent EUI-64 address\n");
		ieee802154_random_extended_addr(&dw->phy->perm_extended_addr);
	}

	return 0;
}

/**
 * dw1000_load_delays() - Load antenna delays
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_load_delays(struct dw1000 *dw)
{
	uint32_t array[2];
	uint32_t delays;
	int rc;

	/* Read delays from OTP */
	if ((rc = regmap_read(dw->otp, DW1000_OTP_DELAYS, &delays)) != 0)
		return rc;
	if (delays != 0 && delays != 0xfffffffful) {
		dw->antd[DW1000_PRF_16M] = DW1000_OTP_DELAYS_16M(delays);
		dw->antd[DW1000_PRF_64M] = DW1000_OTP_DELAYS_64M(delays);
	}

	/* Read delays from devicetree */
	if (of_property_read_u32_array(dw->dev->of_node, "decawave,antd",
				       array, ARRAY_SIZE(array)) == 0) {
		if ((array[0] & ~DW1000_ANTD_MASK) || (array[1] & ~DW1000_ANTD_MASK)) {
			dev_err(dw->dev, "invalid decawave,antd values <0x%04x,0x%04x>\n",
				array[0], array[1]);
			return 0;
		}
		dw->antd[DW1000_PRF_16M] = array[0];
		dw->antd[DW1000_PRF_64M] = array[1];
	}

	dev_dbg(dw->dev, "set antenna delays 0x%04x,0x%04x\n",
		dw->antd[DW1000_PRF_16M], dw->antd[DW1000_PRF_64M]);
	return 0;
}

/**
 * dw1000_load_sar() - Load SAR ADC calibration values
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_load_sar(struct dw1000 *dw)
{
	int vmeas;
	int tmeas;
	int rc;

	/* Read voltage measurement calibration from OTP */
	if ((rc = regmap_read(dw->otp, DW1000_OTP_VMEAS, &vmeas)) != 0)
		return rc;
	dw->vmeas_3v3 = DW1000_OTP_VMEAS_3V3(vmeas);

	/* Read temperature measurement calibration from OTP */
	if ((rc = regmap_read(dw->otp, DW1000_OTP_TMEAS, &tmeas)) != 0)
		return rc;
	dw->tmeas_23c = DW1000_OTP_TMEAS_23C(tmeas);

	return 0;
}

/**
 * dw1000_load_lde() - Load leading edge detection microcode
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_load_lde(struct dw1000 *dw)
{
	int rc;

	/* Force slow SPI clock speed */
	dw->spi->max_speed_hz = DW1000_SPI_SLOW_HZ;

	/* Force system clock to 19.2 MHz XTI clock and prepare for LDE load */
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     (DW1000_PMSC_CTRL0_SYSCLKS_MASK |
				      DW1000_PMSC_CTRL0_LDE_HACK_MASK),
				     (DW1000_PMSC_CTRL0_SYSCLKS_SLOW |
				      DW1000_PMSC_CTRL0_LDE_HACK_PRE))) != 0)
		return rc;

	/* Initiate load of LDE microcode */
	if ((rc = regmap_update_bits(dw->otp_if.regs, DW1000_OTP_CTRL,
				     DW1000_OTP_CTRL_LDELOAD,
				     DW1000_OTP_CTRL_LDELOAD)) != 0)
		return rc;

	/* Allow time for microcode load to complete */
	usleep_range(DW1000_LDELOAD_WAIT_MIN_US, DW1000_LDELOAD_WAIT_MAX_US);

	/* Restore system clock to automatic selection */
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     (DW1000_PMSC_CTRL0_SYSCLKS_MASK |
				      DW1000_PMSC_CTRL0_LDE_HACK_MASK),
				     (DW1000_PMSC_CTRL0_SYSCLKS_AUTO |
				      DW1000_PMSC_CTRL0_LDE_HACK_POST))) != 0)
		return rc;

	/* Switch to full SPI clock speed */
	dw->spi->max_speed_hz = DW1000_SPI_FAST_HZ;

	/* Recheck device ID to ensure bus is still operational */
	if ((rc = dw1000_check_dev_id(dw)) != 0)
		return rc;

	return 0;
}

/**
 * dw1000_init() - Apply initial configuration
 *
 * @dw:			DW1000 device
 * @return:		0 on success or -errno
 */
static int dw1000_init(struct dw1000 *dw)
{
	int modelverrev;
	int sys_cfg_filters;
	int mask;
	int value;
	int rc;

	/* Log device ID */
	if ((rc = regmap_read(dw->dev_id.regs, DW1000_MODELVERREV,
			      &modelverrev)) != 0)
		return rc;
	dev_info(dw->dev, "found model %d.%d.%d\n",
		 DW1000_MODELVERREV_MODEL(modelverrev),
		 DW1000_MODELVERREV_VER(modelverrev),
		 DW1000_MODELVERREV_REV(modelverrev));

	/* Set system configuration:
	 *
	 * - enable all frame types (when filtering is enabled)
	 * - use double buffering
	 * - use receiver auto-re-enabling
	 *
	 * Note that the overall use of filtering is controlled via
	 * dw1000_set_promiscuous_mode().
	 */
	sys_cfg_filters = (DW1000_SYS_CFG_FFAB | DW1000_SYS_CFG_FFAD |
			   DW1000_SYS_CFG_FFAA | DW1000_SYS_CFG_FFAM |
			   DW1000_SYS_CFG_FFAR | DW1000_SYS_CFG_FFA4 |
			   DW1000_SYS_CFG_FFA5);
	mask = (sys_cfg_filters | DW1000_SYS_CFG_DIS_DRXB |
		DW1000_SYS_CFG_RXAUTR);
	value = (sys_cfg_filters | DW1000_SYS_CFG_RXAUTR);
	if ((rc = regmap_update_bits(dw->sys_cfg.regs, 0, mask, value)) != 0)
		return rc;

	/* Handle short inter-frame gaps in hardware */
	value = DW1000_TX_FCTRL4_IFSDELAY(IEEE802154_SIFS_PERIOD);
	if ((rc = regmap_write(dw->tx_fctrl.regs, DW1000_TX_FCTRL4,
			       value)) != 0)
		return rc;

	/* Enable GPIO block and kilohertz clock for LED blinking */
	mask = (DW1000_PMSC_CTRL0_GPCE | DW1000_PMSC_CTRL0_GPRN |
		DW1000_PMSC_CTRL0_GPDCE | DW1000_PMSC_CTRL0_GPDRN |
		DW1000_PMSC_CTRL0_KHZCLKEN);
	value = (DW1000_PMSC_CTRL0_GPCE | DW1000_PMSC_CTRL0_GPRN |
		 DW1000_PMSC_CTRL0_GPDCE | DW1000_PMSC_CTRL0_GPDRN |
		 DW1000_PMSC_CTRL0_KHZCLKEN);
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     mask, value)) != 0)
		return rc;

	/* Configure GPIOs as LED outputs */
	mask = (DW1000_GPIO_MODE_MSGP0_MASK | DW1000_GPIO_MODE_MSGP1_MASK |
		DW1000_GPIO_MODE_MSGP2_MASK | DW1000_GPIO_MODE_MSGP3_MASK);
	value = (DW1000_GPIO_MODE_MSGP0_RXOKLED | DW1000_GPIO_MODE_MSGP1_SFDLED|
		 DW1000_GPIO_MODE_MSGP2_RXLED | DW1000_GPIO_MODE_MSGP3_TXLED);
	if ((rc = regmap_update_bits(dw->gpio_ctrl.regs, DW1000_GPIO_MODE,
				     mask, value)) != 0)
		return rc;

	/* Enable LED blinking */
	mask = (DW1000_PMSC_LEDC_BLINK_TIM_MASK | DW1000_PMSC_LEDC_BLNKEN);
	value = (DW1000_PMSC_LEDC_BLINK_TIM_DEFAULT | DW1000_PMSC_LEDC_BLNKEN);
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_LEDC,
				     mask, value)) != 0)
		return rc;

	/* Clear diagnostic counters */
	mask = DW1000_EVC_CTRL_EVC_CLR;
	value = DW1000_EVC_CTRL_EVC_CLR;
	if ((rc = regmap_update_bits(dw->dig_diag.regs, DW1000_EVC_CTRL,
				     mask, value)) != 0)
		return rc;

	/* Enable diagnostic counters */
	mask = DW1000_EVC_CTRL_EVC_EN;
	value = DW1000_EVC_CTRL_EVC_EN;
	if ((rc = regmap_update_bits(dw->dig_diag.regs, DW1000_EVC_CTRL,
				     mask, value)) != 0)
		return rc;

	/* Load XTALT calibration value */
	if ((rc = dw1000_load_xtalt(dw)) != 0)
		return rc;

	/* Load LDOTUNE calibration value */
	if ((rc = dw1000_load_ldotune(dw)) != 0)
		return rc;

	/* Load EUI-64 address */
	if ((rc = dw1000_load_eui64(dw)) != 0)
		return rc;

	/* Load antenna delays */
	if ((rc = dw1000_load_delays(dw)) != 0)
		return rc;

	/* Load SAR ADC calibration values */
	if ((rc = dw1000_load_sar(dw)) != 0)
		return rc;

	/* Load LDE microcode */
	if ((rc = dw1000_load_lde(dw)) != 0)
		return rc;

	/* Configure radio */
	if ((rc = dw1000_reconfigure(dw, -1U)) != 0)
		return rc;

	/* Initialise PTP clock */
	if ((rc = dw1000_ptp_init(dw)) != 0)
		return rc;

	/* Force TX and RX clocks to stay enabled at all times.  This
	 * seems to prevent an internal state machine lockup under
	 * heavy load, the symptoms of which are that SYS_STATE_PMSC
	 * reports the overall state as "RX" but SYS_STATE_RX reports
	 * the RX state as "idle".
	 */
	if ((rc = regmap_update_bits(dw->pmsc.regs, DW1000_PMSC_CTRL0,
				     (DW1000_PMSC_CTRL0_RXCLKS_MASK |
				      DW1000_PMSC_CTRL0_TXCLKS_MASK),
				     (DW1000_PMSC_CTRL0_RXCLKS_FAST |
				      DW1000_PMSC_CTRL0_TXCLKS_FAST))) != 0)
		return rc;

	/* Clear startup status bits */
	if ((rc = regmap_write(dw->sys_status.regs, 0,
			       (DW1000_SYS_STATUS_CPLOCK |
				DW1000_SYS_STATUS_SLP2INIT |
				DW1000_SYS_STATUS_RFPLL_LL |
				DW1000_SYS_STATUS_CLKPLL_LL))) != 0)
		return rc;

	return 0;
}

/**
 * dw1000_probe() - Create device
 *
 * @spi:		SPI device
 * @return:		0 on success or -errno
 */
static int dw1000_probe(struct spi_device *spi)
{
	struct ieee802154_hw *hw;
	struct dw1000 *dw;
	int rc;

	/* Allocate IEEE 802.15.4 device */
	hw = ieee802154_alloc_hw(sizeof(*dw), &dw1000_ops);
	if (!hw) {
		rc = -ENOMEM;
		goto err_alloc_hw;
	}
	dw = hw->priv;
	dw->spi = spi;
	dw->dev = &spi->dev;
	dw->hw = hw;
	dw->phy = hw->phy;
	dw->xtalt = DW1000_FS_XTALT_XTALT_MIDPOINT;
	dw->channel = DW1000_CHANNEL_DEFAULT;
	dw->prf = DW1000_PRF_64M;
	dw->rate = DW1000_RATE_6800K;
	dw->txpsr = DW1000_TXPSR_DEFAULT;
	dw->smart_power = true;
	dw->snr_threshold = DW1000_SNR_THRESHOLD_DEFAULT;
	dw->fpr_threshold = DW1000_FPR_THRESHOLD_DEFAULT;
	dw->noise_threshold = DW1000_NOISE_THRESHOLD_DEFAULT;
	spin_lock_init(&dw->tx_lock);
	INIT_DELAYED_WORK(&dw->ptp_work, dw1000_ptp_worker);
	hw->parent = &spi->dev;

	/* Initialise reset GPIO pin as output */
	dw->reset_gpio = of_get_named_gpio(dw->dev->of_node, "reset-gpio", 0);
	if (gpio_is_valid(dw->reset_gpio)) {
		if ((rc = gpio_request_one(dw->reset_gpio, GPIOF_DIR_OUT |
					   GPIOF_OPEN_DRAIN | GPIOF_INIT_LOW,
					   "dw1000-reset")) < 0)
			goto err_req_reset_gpio;
	} else {
		dev_warn(dw->dev, "device does not support GPIO RESET control");
	}

	/* Initialise power GPIO pin as output */
	dw->power_gpio = of_get_named_gpio(dw->dev->of_node, "power-gpio", 0);
	if (gpio_is_valid(dw->power_gpio)) {
		if ((rc = gpio_request_one(dw->power_gpio, GPIOF_DIR_OUT |
					   GPIOF_INIT_LOW, "dw1000-power")) < 0)
			goto err_req_power_gpio;
	} else {
		dev_warn(dw->dev, "device does not support GPIO POWER control");
	}

	/* Report capabilities */
	hw->flags = (IEEE802154_HW_TX_OMIT_CKSUM |
		     IEEE802154_HW_AFILT |
		     IEEE802154_HW_PROMISCUOUS |
		     IEEE802154_HW_RX_DROP_BAD_CKSUM);
	hw->phy->supported.channels[DW1000_CHANNEL_PAGE] = DW1000_CHANNELS;
	hw->phy->current_page = DW1000_CHANNEL_PAGE;
	hw->phy->current_channel = DW1000_CHANNEL_DEFAULT;

	/* Initialise register maps */
	if ((rc = dw1000_regmap_init(dw)) != 0)
		goto err_regmap_init;
	if ((rc = dw1000_otp_regmap_init(dw)) != 0)
		goto err_otp_regmap_init;

	/* Reset device */
	if ((rc = dw1000_reset(dw)) != 0) {
		dev_err(dw->dev, "reset failed: %d\n", rc);
		goto err_reset;
	}

	/* Initialise device */
	if ((rc = dw1000_init(dw)) != 0) {
		dev_err(dw->dev, "initialisation failed: %d\n", rc);
		goto err_init;
	}

	/* Add attribute group */
	if ((rc = sysfs_create_group(&dw->dev->kobj, &dw1000_attr_group)) != 0){
		dev_err(dw->dev, "could not create attributes: %d\n", rc);
		goto err_create_group;
	}

	/* Hook interrupt */
	if ((rc = devm_request_threaded_irq(dw->dev, spi->irq, NULL,
					    dw1000_isr_thread,
					    IRQF_NO_SUSPEND | IRQF_ONESHOT,
					    dev_name(dw->dev), dw)) != 0) {
		dev_err(dw->dev, "could not request IRQ %d: %d\n",
			spi->irq, rc);
		goto err_request_irq;
	}

	/* Start timer wraparound check worker */
	schedule_delayed_work(&dw->ptp_work, DW1000_PTP_WORK_DELAY);

	/* Register PTP clock */
	dw->ptp.clock = ptp_clock_register(&dw->ptp.info, dw->dev);
	if (IS_ERR(dw->ptp.clock)) {
		rc = PTR_ERR(dw->ptp.clock);
		dev_err(dw->dev, "could not register PTP clock: %d\n", rc);
		goto err_register_ptp;
	}

	/* Register hardware monitor */
	dw->hwmon = devm_hwmon_device_register_with_info(dw->dev, "dw1000", dw,
							 &dw1000_hwmon_chip,
							 NULL);
	if (IS_ERR(dw->hwmon)) {
		rc = PTR_ERR(dw->hwmon);
		dev_err(dw->dev, "could not register hwmon: %d\n", rc);
		goto err_register_hwmon;
	}

	/* Register IEEE 802.15.4 device */
	if ((rc = ieee802154_register_hw(hw)) != 0) {
		dev_err(dw->dev, "could not register: %d\n", rc);
		goto err_register_hw;
	}

	spi_set_drvdata(spi, hw);
	return 0;

	ieee802154_unregister_hw(hw);
 err_register_hw:
 err_register_hwmon:
	ptp_clock_unregister(dw->ptp.clock);
 err_register_ptp:
	cancel_delayed_work_sync(&dw->ptp_work);
 err_request_irq:
	sysfs_remove_group(&dw->dev->kobj, &dw1000_attr_group);
 err_create_group:
 err_init:
 err_reset:
 err_otp_regmap_init:
 err_regmap_init:
	if (gpio_is_valid(dw->reset_gpio))
		gpio_free(dw->reset_gpio);
 err_req_power_gpio:
	if (gpio_is_valid(dw->power_gpio))
		gpio_free(dw->power_gpio);
 err_req_reset_gpio:
	ieee802154_free_hw(hw);
 err_alloc_hw:
	return rc;
}

/**
 * dw1000_remove() - Remove device
 *
 * @spi:		SPI device
 * @return:		0 on success or -errno
 */
static int dw1000_remove(struct spi_device *spi)
{
	struct ieee802154_hw *hw = spi_get_drvdata(spi);
	struct dw1000 *dw = hw->priv;

	ieee802154_unregister_hw(hw);
	ptp_clock_unregister(dw->ptp.clock);
	cancel_delayed_work_sync(&dw->ptp_work);
	sysfs_remove_group(&dw->dev->kobj, &dw1000_attr_group);
	dw1000_reset(dw);
	ieee802154_free_hw(hw);
	if (gpio_is_valid(dw->reset_gpio))
		gpio_free(dw->reset_gpio);
	if (gpio_is_valid(dw->power_gpio))
		gpio_free(dw->power_gpio);
	return 0;
}

/******************************************************************************
 *
 * Module interface
 *
 */

enum {
	DW1000,
};

static const struct of_device_id dw1000_of_ids[] = {
	{ .compatible = "decawave,dw1000", .data = (void *)DW1000 },
	{ },
};
MODULE_DEVICE_TABLE(of, dw1000_of_ids);

static const struct spi_device_id dw1000_spi_ids[] = {
	{ "dw1000", DW1000 },
	{ },
};
MODULE_DEVICE_TABLE(spi, dw1000_spi_ids);

static struct spi_driver dw1000_driver = {
	.driver = {
		.name = "dw1000",
		.of_match_table = of_match_ptr(dw1000_of_ids),
	},
	.id_table = dw1000_spi_ids,
	.probe = dw1000_probe,
	.remove = dw1000_remove,
};
module_spi_driver(dw1000_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Michael Brown <mbrown@fensystems.co.uk>");
MODULE_DESCRIPTION("DecaWave DW1000 IEEE 802.15.4 driver");
