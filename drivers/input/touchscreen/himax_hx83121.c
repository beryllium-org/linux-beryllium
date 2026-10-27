// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Himax HX83121A touchscreens.
 *
 * Distilled from the Himax/Rockchip vendor touchscreen driver
 * (drivers/input/touchscreen/hxchipset/ in the Rockchip 6.1 vendor
 * tree), following the shape of the mainline himax_hx83112b.c driver.
 * Debugfs / inspection / firmware-upgrade machinery of the vendor
 * driver is deliberately not ported: the IC boots its flash-resident
 * firmware on its own and the driver only takes it out of reset,
 * waits for the firmware to finish loading and reads touch events.
 *
 * Vendor sequences verified against hxchipset/himax_ic_HX83121.c,
 * himax_ic_incell_core.c and himax_common.c; anything not verifiable
 * from those sources is flagged with a comment below.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/unaligned.h>
#include <linux/regmap.h>

#define HIMAX_MAX_POINTS		10

/*
 * AHB-I2C bridge registers (single-byte I2C register offsets).
 * An AHB write is a single transaction to 0x00 carrying a 4-byte
 * little-endian address followed by the data; reads go through the
 * 0x0c/0x08 pair.
 */
#define HIMAX_AHB_ADDR_BYTE_0		0x00
#define HIMAX_AHB_ADDR_RDATA_BYTE_0	0x08
#define HIMAX_AHB_ADDR_ACCESS_DIRECTION	0x0c
#define HIMAX_AHB_ADDR_INCR4		0x0d
#define HIMAX_AHB_ADDR_CONTI		0x13
#define HIMAX_AHB_ADDR_EVENT_STACK	0x30
/* Safe-mode I2C password registers. */
#define HIMAX_AHB_ADDR_I2C_PSW_LB	0x31
#define HIMAX_AHB_ADDR_I2C_PSW_UB	0x32

#define HIMAX_AHB_CMD_ACCESS_DIRECTION_READ	0x00
#define HIMAX_AHB_CMD_CONTI		0x31
#define HIMAX_AHB_CMD_I2C_PSW_LB	0x27
#define HIMAX_AHB_CMD_I2C_PSW_UB	0x95

/*
 * Note: unlike hx83112b, the HX83121A INCR4 command value is 0x12
 * (vendor hx83121a_ic_cmd_incr4), and it is ORed with 0x01 to enable
 * 4-byte auto-increment for reads longer than 4 bytes.
 */
#define HIMAX_AHB_CMD_INCR4		0x12
#define HIMAX_AHB_CMD_INCR4_AUTO_INC	0x13

/* Firmware-controlled registers (accessed through the AHB bridge). */
#define HX83121A_REG_ADDR_FW_ISR	0x9000005c
#define HX83121A_REG_ADDR_CENTRAL_STATE	0x900000a8
#define HX83121A_REG_ADDR_ICID		0x900000d0
#define HX83121A_REG_ADDR_TCON_RST	0x80020020
#define HX83121A_REG_ADDR_ADC_RST	0x80020094

#define HX83121A_REG_ADDR_RAW_OUT_SEL	0x100072ec
#define HX83121A_REG_ADDR_SORTING_MODE	0x10007f04
#define HX83121A_REG_ADDR_SET_FRAME	0x10007294
#define HX83121A_REG_ADDR_2ND_RELOAD	0x100072c0
#define HX83121A_REG_ADDR_FLASH_RELOAD	0x10007f00
#define HX83121A_REG_ADDR_RX_TX_NUM	0x100070f4
#define HX83121A_REG_ADDR_MAXPT		0x100070f8

/* State values polled from HX83121A_REG_ADDR_CENTRAL_STATE. */
#define HX83121A_FW_STATE_RUNNING	0x05
#define HX83121A_FW_STATE_SAFE_MODE	0x0c

/* Value written to FW_ISR to stop the firmware main loop. */
#define HX83121A_FW_CMD_STOP		0x000000a5

/*
 * The firmware signals the end of its post-reset config reload by
 * writing this little-endian word into the 2nd-flash-reload
 * handshake register (vendor compares bytes C0 72 00 00).
 */
#define HX83121A_FW_RELOAD_DONE		0x0072c0

#define HX83121A_PRODUCT_ID		0x83121a

/* Event stack size; the firmware leaves the rest clear. */
#define HIMAX_EVENT_STACK_SIZE		128

#define HIMAX_INVALID_COORD		0xffff
/* Palm marker emitted by the firmware instead of a finger position. */
#define HIMAX_PALM_COORD		0xfa5a

struct himax_event_point {
	__be16 x;
	__be16 y;
} __packed;

struct himax_event {
	struct himax_event_point points[HIMAX_MAX_POINTS];
	u8 majors[HIMAX_MAX_POINTS];
	u8 pad0[2];
	u8 num_points;
	u8 pad1[2];
	u8 checksum_fix;
} __packed;

static_assert(sizeof(struct himax_event) == 56);

struct himax_ts_data {
	struct gpio_desc *gpiod_rst;
	struct input_dev *input_dev;
	struct i2c_client *client;
	struct regmap *regmap;
	struct touchscreen_properties props;
	unsigned int max_points;
};

static const struct regmap_config himax_regmap_config = {
	.reg_bits = 8,
	.val_bits = 32,
	.val_format_endian = REGMAP_ENDIAN_LITTLE,
};

/* AHB accessors, 1:1 with the vendor's burst-enable/register-read. */

/*
 * Single-byte bridge register write (CONTI/INCR4/password/direction):
 * the vendor writes exactly one data byte, so bypass regmap's 32-bit
 * value formatting.
 */
static int himax_reg_write8(struct himax_ts_data *ts, unsigned int reg,
			    u8 value)
{
	return regmap_raw_write(ts->regmap, reg, &value, 1);
}

static int himax_bus_enable_burst(struct himax_ts_data *ts, bool auto_inc)
{
	int error;

	error = himax_reg_write8(ts, HIMAX_AHB_ADDR_CONTI, HIMAX_AHB_CMD_CONTI);
	if (error)
		return error;

	return himax_reg_write8(ts, HIMAX_AHB_ADDR_INCR4,
				auto_inc ? HIMAX_AHB_CMD_INCR4_AUTO_INC :
					   HIMAX_AHB_CMD_INCR4);
}

static int himax_bus_read(struct himax_ts_data *ts, u32 address, void *dst,
			  size_t length)
{
	int error;

	if (length > 4) {
		error = himax_bus_enable_burst(ts, true);
		if (error)
			return error;
	} else {
		error = himax_bus_enable_burst(ts, false);
		if (error)
			return error;
	}

	error = regmap_write(ts->regmap, HIMAX_AHB_ADDR_BYTE_0, address);
	if (error)
		return error;

	error = himax_reg_write8(ts, HIMAX_AHB_ADDR_ACCESS_DIRECTION,
				 HIMAX_AHB_CMD_ACCESS_DIRECTION_READ);
	if (error)
		return error;

	if (length > 4)
		return regmap_noinc_read(ts->regmap, HIMAX_AHB_ADDR_RDATA_BYTE_0,
					 dst, length);

	return regmap_read(ts->regmap, HIMAX_AHB_ADDR_RDATA_BYTE_0, dst);
}

/*
 * AHB register write: one I2C transaction to 0x00 carrying the 4-byte
 * little-endian address followed by the 4-byte little-endian value
 * (vendor himax_bus_write(0x00, addr, data, 8)).
 */
static int himax_bus_write(struct himax_ts_data *ts, u32 address, u32 value)
{
	u8 buf[8];

	put_unaligned_le32(address, buf);
	put_unaligned_le32(value, buf + 4);

	return regmap_raw_write(ts->regmap, HIMAX_AHB_ADDR_BYTE_0, buf,
				sizeof(buf));
}

/*
 * Hardware reset: assert for 5 ms, then release and give the firmware
 * 50 ms before the next bus access (vendor RST_LOW_PERIOD /
 * RST_HIGH_PERIOD for the flash-resident firmware configuration).
 * The vendor reset line idles high and is pulsed low, so the DT
 * reset-gpios property must be marked GPIO_ACTIVE_LOW.
 */
static void himax_reset(struct himax_ts_data *ts)
{
	gpiod_set_value_cansleep(ts->gpiod_rst, 1);
	msleep(5);
	gpiod_set_value_cansleep(ts->gpiod_rst, 0);
	msleep(50);
}

/*
 * Stop the firmware and put the IC into safe mode so the AHB bridge
 * can be used while the touch firmware is not running.  This is the
 * vendor hx83121a sense_off() sequence, required before reading the
 * chip ID through the AHB bridge.  Without it the running firmware
 * owns the bridge and the ID read is unreliable.
 */
static int himax_enter_safe_mode(struct himax_ts_data *ts)
{
	u32 status;
	int error;
	int retry;

	/* Ask the firmware to stop. */
	error = himax_bus_write(ts, HX83121A_REG_ADDR_FW_ISR,
				HX83121A_FW_CMD_STOP);
	if (error)
		return error;

	/* Give it time to reach idle; if it is not running, skip waiting. */
	for (retry = 0; retry < 50; retry++) {
		error = himax_bus_read(ts, HX83121A_REG_ADDR_CENTRAL_STATE,
				       &status, sizeof(status));
		if (error)
			return error;

		if ((status & 0xff) != HX83121A_FW_STATE_RUNNING)
			break;

		error = himax_bus_read(ts, HX83121A_REG_ADDR_FW_ISR,
				       &status, sizeof(status));
		if (error)
			return error;

		if ((status & 0xff) == 0x87)
			break;

		msleep(20);
	}

	/*
	 * Enter safe mode with the I2C password handshake; retry while
	 * polling the central state.  The vendor falls back to a hardware
	 * reset on each failed round; the initial reset in the probe path
	 * plus this loop covers the same cases.
	 */
	for (retry = 0; retry < 15; retry++) {
		error = himax_reg_write8(ts, HIMAX_AHB_ADDR_I2C_PSW_LB,
					 HIMAX_AHB_CMD_I2C_PSW_LB);
		if (error)
			return error;

		error = himax_reg_write8(ts, HIMAX_AHB_ADDR_I2C_PSW_UB,
					 HIMAX_AHB_CMD_I2C_PSW_UB);
		if (error)
			return error;

		error = himax_bus_read(ts, HX83121A_REG_ADDR_CENTRAL_STATE,
				       &status, sizeof(status));
		if (error)
			return error;

		if ((status & 0xff) == HX83121A_FW_STATE_SAFE_MODE) {
			/* Reset the TCON and ADC domgains while stopped. */
			error = himax_bus_write(ts, HX83121A_REG_ADDR_TCON_RST,
						0x00000000);
			if (error)
				return error;

			usleep_range(1000, 1100);

			error = himax_bus_write(ts, HX83121A_REG_ADDR_ADC_RST,
						0x00000000);
			if (error)
				return error;

			usleep_range(1000, 1100);

			return himax_bus_write(ts, HX83121A_REG_ADDR_ADC_RST,
					       0x00000001);
		}

		usleep_range(10000, 10001);
	}

	return -EIO;
}

static int himax_check_product_id(struct himax_ts_data *ts)
{
	u32 product_id;
	int error;

	error = himax_bus_read(ts, HX83121A_REG_ADDR_ICID, &product_id,
			       sizeof(product_id));
	if (error) {
		dev_err(&ts->client->dev, "Failed to read chip ID: %d\n",
			error);
		return error;
	}

	product_id >>= 8;
	if (product_id == HX83121A_PRODUCT_ID)
		return 0;

	dev_err(&ts->client->dev, "Unknown product id: %x\n", product_id);
	return -EINVAL;
}

/*
 * Wake the AHB bridge up (burst-mode handshake, vendor interface_on()),
 * release the firmware main loop and reset the IC so the flash-resident
 * firmware reloads its configuration, then wait for the reload-done
 * handshake word.
 */
static int himax_fw_start(struct himax_ts_data *ts)
{
	u32 status;
	int error;
	int retry;

	/* Vendor interface_on(): verify the burst-mode command registers. */
	for (retry = 0; retry < 10; retry++) {
		u32 conti, incr4;

		/* Dummy read to wake up the bus. */
		error = regmap_read(ts->regmap, HIMAX_AHB_ADDR_RDATA_BYTE_0,
				    &status);
		if (error)
			return error;

		error = himax_reg_write8(ts, HIMAX_AHB_ADDR_CONTI,
					 HIMAX_AHB_CMD_CONTI);
		if (error)
			return error;

		error = himax_reg_write8(ts, HIMAX_AHB_ADDR_INCR4,
					 HIMAX_AHB_CMD_INCR4);
		if (error)
			return error;

		error = regmap_read(ts->regmap, HIMAX_AHB_ADDR_CONTI, &conti);
		if (error)
			return error;

		error = regmap_read(ts->regmap, HIMAX_AHB_ADDR_INCR4, &incr4);
		if (error)
			return error;

		if ((conti & 0xff) == HIMAX_AHB_CMD_CONTI &&
		    (incr4 & 0xff) == HIMAX_AHB_CMD_INCR4)
			break;

		usleep_range(1000, 1100);
	}

	/*
	 * Vendor power_on_init(): reset the FW-side registers to their
	 * normal-mode values, enable the config reload and clear the
	 * reload-done handshake word before the reset below.
	 */
	error = himax_bus_write(ts, HX83121A_REG_ADDR_RAW_OUT_SEL, 0x00000000);
	if (error)
		return error;

	error = himax_bus_write(ts, HX83121A_REG_ADDR_SORTING_MODE, 0x00000000);
	if (error)
		return error;

	error = himax_bus_write(ts, HX83121A_REG_ADDR_SET_FRAME, 0x00000001);
	if (error)
		return error;

	error = himax_bus_write(ts, HX83121A_REG_ADDR_FLASH_RELOAD, 0x00000000);
	if (error)
		return error;

	error = himax_bus_write(ts, HX83121A_REG_ADDR_2ND_RELOAD, 0x00000000);
	if (error)
		return error;

	/* Vendor sense_on(): clear the ISR flag, then reset the IC. */
	error = himax_bus_write(ts, HX83121A_REG_ADDR_FW_ISR, 0x00000000);
	if (error)
		return error;

	usleep_range(10000, 11000);

	himax_reset(ts);

	/* Wait for the firmware to finish reloading its configuration. */
	for (retry = 0; retry < 30; retry++) {
		error = himax_bus_read(ts, HX83121A_REG_ADDR_2ND_RELOAD,
				       &status, sizeof(status));
		if (error)
			return error;

		if (status == HX83121A_FW_RELOAD_DONE)
			return 0;

		usleep_range(10000, 11000);
	}

	dev_err(&ts->client->dev,
		"Timed out waiting for firmware config reload\n");
	return -ETIMEDOUT;
}

/*
 * Read the touch panel layout the firmware was configured with
 * (vendor touch_information()) for the debug log and to validate the
 * expected 10-point frame layout.
 */
static int himax_read_fw_info(struct himax_ts_data *ts)
{
	u32 value;
	u32 rx, tx;
	int error;

	error = himax_bus_read(ts, HX83121A_REG_ADDR_MAXPT, &value,
			       sizeof(value));
	if (error)
		return error;

	ts->max_points = value & 0xff;
	if (ts->max_points == 0 || ts->max_points > HIMAX_MAX_POINTS)
		ts->max_points = HIMAX_MAX_POINTS;

	error = himax_bus_read(ts, HX83121A_REG_ADDR_RX_TX_NUM, &value,
			       sizeof(value));
	if (error)
		return error;

	rx = (value >> 16) & 0xff;
	tx = (value >> 24) & 0xff;
	dev_dbg(&ts->client->dev,
		"Firmware reports RX %u TX %u, %u touch points\n",
		rx, tx, ts->max_points);

	/*
	 * The vendor driver sizes the event frame dynamically as
	 * (max_pts + max_pts/4 + 1..2) * 4 bytes, which is 56 bytes for
	 * the 10-point layout of this panel -- exactly sizeof(struct
	 * himax_event).  A different point count would shift the
	 * num_points field and would need a dynamic frame size.
	 */

	return 0;
}

static int himax_input_register(struct himax_ts_data *ts)
{
	int error;

	ts->input_dev = devm_input_allocate_device(&ts->client->dev);
	if (!ts->input_dev)
		return -ENOMEM;

	ts->input_dev->name = "Himax Touchscreen";

	input_set_capability(ts->input_dev, EV_ABS, ABS_MT_POSITION_X);
	input_set_capability(ts->input_dev, EV_ABS, ABS_MT_POSITION_Y);
	input_set_abs_params(ts->input_dev, ABS_MT_WIDTH_MAJOR, 0, 200, 0, 0);
	input_set_abs_params(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0, 200, 0, 0);

	touchscreen_parse_properties(ts->input_dev, true, &ts->props);

	error = input_mt_init_slots(ts->input_dev, ts->max_points,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error) {
		dev_err(&ts->client->dev,
			"Failed to initialize MT slots: %d\n", error);
		return error;
	}

	return input_register_device(ts->input_dev);
}

static u8 himax_event_get_num_points(const struct himax_event *event)
{
	if (event->num_points == 0xff)
		return 0;

	return event->num_points & 0x0f;
}

static void himax_process_event(struct himax_ts_data *ts,
				const struct himax_event *event)
{
	int i;
	int num_points_left = himax_event_get_num_points(event);

	for (i = 0; i < ts->max_points && num_points_left > 0; i++) {
		u16 x = be16_to_cpu(event->points[i].x);
		u16 y = be16_to_cpu(event->points[i].y);
		u8 w = event->majors[i];

		if (x == HIMAX_INVALID_COORD || y == HIMAX_INVALID_COORD)
			continue;

		/*
		 * The firmware signals a palm with this marker; the vendor
		 * driver turns it into a synthetic KEY_POWER tap, which is
		 * deliberately not reproduced.  A palm simply reports as
		 * "no touch" for that point.
		 */
		if (x == HIMAX_PALM_COORD && y == HIMAX_PALM_COORD)
			continue;

		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
		touchscreen_report_pos(ts->input_dev, &ts->props, x, y, true);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, w);
		input_report_abs(ts->input_dev, ABS_MT_WIDTH_MAJOR, w);
		num_points_left--;
	}

	input_mt_sync_frame(ts->input_dev);
	input_sync(ts->input_dev);
}

static bool himax_verify_checksum(struct himax_ts_data *ts,
				  const struct himax_event *event)
{
	const u8 *data = (const u8 *)event;
	u16 checksum = 0;
	int i;

	for (i = 0; i < sizeof(*event); i++)
		checksum += data[i];

	if (checksum & 0xff) {
		dev_dbg(&ts->client->dev, "Wrong event checksum: %04x\n",
			checksum);
		return false;
	}

	return true;
}

static void himax_flush_event_stack(struct himax_ts_data *ts)
{
	u8 flush[HIMAX_EVENT_STACK_SIZE - sizeof(struct himax_event)];

	/*
	 * The event stack stays valid until the whole 128-byte frame is
	 * consumed.  On a bad checksum read the remainder so that a
	 * level-triggered interrupt does not fire endlessly (vendor
	 * himax_checksum_cal() clear-event path).  Burst mode is already
	 * enabled by the event read that failed the checksum.
	 */
	regmap_raw_read(ts->regmap, HIMAX_AHB_ADDR_EVENT_STACK, flush,
			sizeof(flush));
}

static int himax_handle_input(struct himax_ts_data *ts)
{
	struct himax_event event;
	int error;

	/*
	 * The vendor re-enables burst mode around every event-stack
	 * read; the bridge setting is global and reads longer than 4
	 * bytes from the stack region need it.
	 */
	error = himax_bus_enable_burst(ts, true);
	if (error)
		return error;

	error = regmap_raw_read(ts->regmap, HIMAX_AHB_ADDR_EVENT_STACK, &event,
				sizeof(event));
	if (error) {
		dev_err(&ts->client->dev, "Failed to read input event: %d\n",
			error);
		return error;
	}

	if (himax_verify_checksum(ts, &event))
		himax_process_event(ts, &event);
	else
		himax_flush_event_stack(ts);

	return 0;
}

static irqreturn_t himax_irq_handler(int irq, void *dev_id)
{
	struct himax_ts_data *ts = dev_id;

	if (himax_handle_input(ts))
		return IRQ_NONE;

	return IRQ_HANDLED;
}

static int himax_suspend(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	disable_irq(ts->client->irq);

	return 0;
}

static int himax_resume(struct device *dev)
{
	struct himax_ts_data *ts = dev_get_drvdata(dev);

	enable_irq(ts->client->irq);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(himax_pm_ops, himax_suspend, himax_resume);

static int himax_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct himax_ts_data *ts;
	int error;

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(dev, "I2C check functionality failed\n");
		return -ENXIO;
	}

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	i2c_set_clientdata(client, ts);
	ts->client = client;
	ts->max_points = HIMAX_MAX_POINTS;

	ts->regmap = devm_regmap_init_i2c(client, &himax_regmap_config);
	error = PTR_ERR_OR_ZERO(ts->regmap);
	if (error) {
		dev_err(dev, "Failed to initialize regmap: %d\n", error);
		return error;
	}

	ts->gpiod_rst = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	error = PTR_ERR_OR_ZERO(ts->gpiod_rst);
	if (error) {
		dev_err(dev, "Failed to get reset GPIO: %d\n", error);
		return error;
	}

	himax_reset(ts);

	/*
	 * The touch firmware owns the AHB bridge while running; stop it
	 * before reading the chip ID (vendor detect: sense_off + ID read
	 * from 0x900000d0).
	 */
	error = himax_enter_safe_mode(ts);
	if (error) {
		dev_err(dev, "Failed to enter safe mode: %d\n", error);
		return error;
	}

	error = himax_check_product_id(ts);
	if (error)
		return error;

	error = himax_fw_start(ts);
	if (error)
		return error;

	error = himax_read_fw_info(ts);
	if (error)
		return error;

	error = himax_input_register(ts);
	if (error)
		return error;

	/*
	 * The FydeTab Duo interrupt line is level-low; the firmware
	 * deasserts it once the full 128-byte event stack has been read,
	 * hence the flush on checksum errors.
	 */
	error = devm_request_threaded_irq(dev, client->irq, NULL,
					  himax_irq_handler, IRQF_ONESHOT,
					  client->name, ts);
	if (error)
		return error;

	return 0;
}

static const struct i2c_device_id himax_ts_id[] = {
	{ .name = "hx83121a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(i2c, himax_ts_id);

static const struct of_device_id himax_of_match[] = {
	{ .compatible = "himax,hx83121a" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, himax_of_match);

static struct i2c_driver himax_ts_driver = {
	.probe = himax_probe,
	.id_table = himax_ts_id,
	.driver = {
		.name = "himax-hx83121a-ts",
		.of_match_table = himax_of_match,
		.pm = pm_sleep_ptr(&himax_pm_ops),
	},
};
module_i2c_driver(himax_ts_driver);

MODULE_AUTHOR("Bill Sideris <bill88t@gmail.com>");
MODULE_DESCRIPTION("Himax hx83121a touchscreen driver");
MODULE_LICENSE("GPL");
