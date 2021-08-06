#include <linux/module.h>
#include <linux/of.h>
#include <media/tegra-v4l2-camera.h>
#include <media/tegracam_core.h>
#include "vc_mipi_core.h"
#include "vc_mipi_modules.h"


static struct vc_cam *tegracam_to_cam(struct tegracam_device *tc_dev)
{
	return (struct vc_cam *)tegracam_get_privdata(tc_dev);
}

static struct sensor_mode_properties *tegracam_to_mode0(struct tegracam_device *tc_dev) 
{
	struct sensor_properties *sensor = &tc_dev->s_data->sensor_props;

	if (sensor->sensor_modes != NULL && sensor->num_modes > 0) {
		return &sensor->sensor_modes[0];
	}
	return NULL;
}

static int vc_adjust_cam_ctrls(struct vc_cam *cam);

static int vc_adjust_tegracam(struct tegracam_device *tc_dev, __u32 height)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	struct device *dev = tc_dev->s_data->dev;
	struct camera_common_frmfmt *frmfmt = (struct camera_common_frmfmt *)tc_dev->sensor_ops->frmfmt_table;
	struct sensor_mode_properties *mode = tegracam_to_mode0(tc_dev);
	int update = 1;

	if (height == 0) {
		update = vc_adjust_cam_ctrls(cam);
		if (update)
			height = cam->ctrl.framesize.height;
	}

	if (update) {
		if (frmfmt == NULL || mode == NULL) {
			vc_err(dev, "%s(): Invalid Pointers. Tegracam not properly setup!\n", __FUNCTION__);
			return 1;
		}
	
		// TODO: Check camera_common_try_fmt() if it is properly implemented ?!?!?
		tc_dev->s_data->def_height = height;
		tc_dev->s_data->fmt_height = height;
		frmfmt->size.height = height;
		mode->image_properties.height = height;
		mode->image_properties.line_length = height;

		vc_notice(dev, "%s(): Adjust tegracam framework settings (height: %d)\n", __FUNCTION__, height);
	}

	return 0;
}

static int vc_read_reg(struct camera_common_data *s_data, __u16 addr, __u8 *val)
{
    	int ret = 0;
    	__u32 reg_val = 0;

    	ret = regmap_read(s_data->regmap, addr, &reg_val); 
    	if (ret) {
       		dev_err(s_data->dev, "%s(): i2c read failed! (addr: 0x%04x)\n", __FUNCTION__, addr);
       		return ret;
    	}
    	*val = reg_val & 0xff;
    	return 0;
}

static int vc_write_reg(struct camera_common_data *s_data, __u16 addr, __u8 val)
{
    	int ret = 0;

    	ret = regmap_write(s_data->regmap, addr, val);
    	if (ret) {
        	dev_err(s_data->dev, "%s(): i2c write failed! (addr: 0x%04x <= value: 0x%02x)\n", __FUNCTION__, addr, val);
    	}
    	return ret;
}

static int vc_set_mode(struct tegracam_device *tc_dev)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	int ret;

	ret  = vc_core_set_format(cam , tc_dev->s_data->colorfmt->code);
	ret |= vc_mod_set_mode(cam);
	ret |= vc_adjust_tegracam(tc_dev, 0);

	return ret;
}

static int vc_set_gain(struct tegracam_device *tc_dev, __s64 val)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	return vc_sen_set_gain(cam, val);
}

static int vc_set_exposure(struct tegracam_device *tc_dev, __s64 val)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	return vc_sen_set_exposure_dirty(cam, val);
}

static int vc_set_frame_rate(struct tegracam_device *tc_dev, __s64 val)
{
	// TODO
	return 0;
}

static int vc_set_trigger_mode(struct tegracam_device *tc_dev, __s64 val)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	int ret;

	ret  = vc_mod_set_trigger_mode(cam, val);
	ret |= vc_adjust_tegracam(tc_dev, 0);

	return ret;
}

static int vc_set_flash_mode(struct tegracam_device *tc_dev, __s64 val)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	return vc_mod_set_flash_mode(cam, val);
}

static int vc_set_image_height(struct tegracam_device *tc_dev, __s64 val)
{
	return vc_adjust_tegracam(tc_dev, val);
}

// Don't remove this function. It is needed by the Tegra Framework. 
static int vc_set_group_hold(struct tegracam_device *tc_dev, bool val)
{
	return 0;
}

static int vc_start_streaming(struct tegracam_device *tc_dev)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	int ret = 0;

	ret  = vc_sen_set_gain(cam, cam->state.gain);
	ret |= vc_sen_set_exposure_dirty(cam, cam->state.exposure);
	ret |= vc_sen_start_stream(cam);

	return ret;
}

static int vc_stop_streaming(struct tegracam_device *tc_dev)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	return vc_sen_stop_stream(cam);
}

// Don't remove this function. It is needed by the Tegra Framework. 
static int vc_power_get(struct tegracam_device *tc_dev)
{
	return 0;
}

// Don't remove this function. It is needed by the Tegra Framework. 
// (called by tegracam_device_register)
static struct camera_common_pdata *vc_parse_dt(struct tegracam_device *tc_dev)
{
	return devm_kzalloc(tc_dev->dev, sizeof(struct camera_common_pdata), GFP_KERNEL);
}

static int vc_adjust_cam_ctrls(struct vc_cam *cam)
{
	int trigger_enabled = vc_mod_is_trigger_enabled(cam);
	__u32 code = vc_core_get_format(cam);
	__u32 height = cam->ctrl.framesize.height;

	// This error is dependent on VMAX, SHS and framesize.height
	// TEGRA_VI_CSI_ERROR_STATUS=0x00000001 (Bits 3-0: h>,<, w>,<) => width is to high
	// TEGRA_VI_CSI_ERROR_STATUS=0x00000002 (Bits 3-0: h>,<, w>,<) => width is to low
	// TEGRA_VI_CSI_ERROR_STATUS=0x00000004 (Bits 3-0: h>,<, w>,<) => height is to high
	// TEGRA_VI_CSI_ERROR_STATUS=0x00000008 (Bits 3-0: h>,<, w>,<) => height is to low
	switch (cam->desc.mod_id) {
	case MOD_ID_IMX183:
		// Active pixels 5440 x 3694 (FPGA)
		// cam->ctrl.framesize.height = trigger_enabled ? 3647 : 3635; // Formate?	
		cam->ctrl.framesize.height = 3602; // VMAX = 3648
		break;
	case MOD_ID_IMX226:
		// Active pixels 3840 x 3046 (FPGA)
		switch (cam->desc.mod_rev) {
		case 0x000b:
			cam->ctrl.framesize.height = 3044;
			break;
		default:
			switch (code) {
			case MEDIA_BUS_FMT_Y12_1X12: 
			case MEDIA_BUS_FMT_SRGGB12_1X12:
				// Tested for RG12 in Freerun (36.3 Hz)
				cam->ctrl.framesize.height = 3045;
				break;
			default:
				// Tested for RGGB, RG10 (43.6 Hz)
				cam->ctrl.framesize.height = trigger_enabled ? 3044 : 3043;
				break;
			}
			break;
		}
		break;
	case MOD_ID_IMX252:
		// Active pixels 2048 x 1536 (FPGA)
		cam->ctrl.framesize.height = 1535; // Freerun and trigger mode => Formate?
		break;
	case MOD_ID_IMX296: 
		// Active pixels 1440 x 1080
		break;
	case MOD_ID_IMX327: 
		// Active pixels 1920 x 1080
		break;
	}

	return height != cam->ctrl.framesize.height;
}

static struct camera_common_sensor_ops vc_sensor_ops = {
	.read_reg = vc_read_reg,
	.write_reg = vc_write_reg,
	.set_mode = vc_set_mode,
	.start_streaming = vc_start_streaming,
	.stop_streaming = vc_stop_streaming,
	.power_get = vc_power_get,
	.parse_dt = vc_parse_dt,
};

static int vc_init_frmfmt(struct device *dev, struct vc_cam *cam)
{
	struct camera_common_frmfmt *frmfmt;
	int *fps;

	if (vc_sensor_ops.frmfmt_table != NULL) {
		devm_kfree(dev, (void *)vc_sensor_ops.frmfmt_table[0].framerates);
		devm_kfree(dev, (void *)vc_sensor_ops.frmfmt_table);
	}

	frmfmt = devm_kzalloc(dev, sizeof(*frmfmt), GFP_KERNEL);
	if (!frmfmt)
		return -ENOMEM;

	fps = devm_kzalloc(dev, sizeof(int), GFP_KERNEL);
	if (!fps)
		return -ENOMEM;

	vc_sensor_ops.numfrmfmts = 1;
	vc_sensor_ops.frmfmt_table = frmfmt;

	fps[0] = cam->ctrl.framerate.def;

	frmfmt->size.width = cam->ctrl.framesize.width;
	frmfmt->size.height = cam->ctrl.framesize.height;
	frmfmt->framerates = fps;
	frmfmt->num_framerates = 1;
	frmfmt->hdr_en = 0;
	frmfmt->mode = 0;

	vc_notice(dev, "%s(): Init frame (width: %d, height: %d, fps: %d)\n", __FUNCTION__,
		frmfmt->size.width, frmfmt->size.height, frmfmt->framerates[0]);

	return 0;
}

static void vc_init_image(struct tegracam_device *tc_dev)
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	struct device *dev = tc_dev->dev;
	struct sensor_mode_properties *mode = tegracam_to_mode0(tc_dev);
	struct sensor_image_properties *image;
	__u32 code;

	if (mode != NULL) {
		image = &mode->image_properties;
		image->width = cam->ctrl.framesize.width;
		image->height = cam->ctrl.framesize.height;
		image->line_length = cam->ctrl.framesize.width;

		code = vc_core_get_format(cam);
		switch(code) {
		case MEDIA_BUS_FMT_Y8_1X8:       image->pixel_format = V4L2_PIX_FMT_GREY;    break;
		case MEDIA_BUS_FMT_Y10_1X10:     image->pixel_format = V4L2_PIX_FMT_Y10;     break;
		case MEDIA_BUS_FMT_Y12_1X12:     image->pixel_format = V4L2_PIX_FMT_Y12;     break;
		case MEDIA_BUS_FMT_SRGGB8_1X8:   image->pixel_format = V4L2_PIX_FMT_SRGGB8;  break;
		case MEDIA_BUS_FMT_SRGGB10_1X10: image->pixel_format = V4L2_PIX_FMT_SRGGB10; break;
		case MEDIA_BUS_FMT_SRGGB12_1X12: image->pixel_format = V4L2_PIX_FMT_SRGGB12; break;
		case MEDIA_BUS_FMT_SGBRG8_1X8:   image->pixel_format = V4L2_PIX_FMT_SGBRG8;  break;
		case MEDIA_BUS_FMT_SGBRG10_1X10: image->pixel_format = V4L2_PIX_FMT_SGBRG10; break;
		case MEDIA_BUS_FMT_SGBRG12_1X12: image->pixel_format = V4L2_PIX_FMT_SGBRG12; break;
		}

		tc_dev->s_data->colorfmt = camera_common_find_datafmt(code);
		
		vc_notice(dev, "%s(): Init image (width: %d, height: %d, line_length: %d, pixel_t: %c%c%c%c)\n", __FUNCTION__,
			image->width, image->height, image->line_length, 
			(char)((image->pixel_format >>  0)&0xFF),
			(char)((image->pixel_format >>  8)&0xFF),
			(char)((image->pixel_format >> 16)&0xFF),
			(char)((image->pixel_format >> 24)&0xFF));
	}
}

static void vc_init_lanes(struct tegracam_device *tc_dev) 
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	struct device *dev = tc_dev->dev;
	struct sensor_mode_properties *mode = tegracam_to_mode0(tc_dev);
	struct sensor_signal_properties *signal;

	if (mode != NULL) {
		signal = &mode->signal_properties;

		vc_core_set_num_lanes(cam, signal->num_lanes);

		vc_notice(dev, "%s(): Init Lanes (num_lanes: %d)\n", __FUNCTION__, signal->num_lanes);
	}
}

static int read_property_u32(struct device_node *node, const char *name, int radix, __u32 *value)
{
	const char *str;
	int ret = 0;

    	ret = of_property_read_string(node, name, &str);
    	if (ret)
        	return -ENODATA;

	ret = kstrtou32(str, radix, value);
	if (ret)
		return -EFAULT;

    	return 0;
}

static void vc_init_trigger(struct device *dev, struct vc_cam *cam)
{
	struct device_node *node = dev->of_node;
	int value = 1;
	int ret = 0;

	if (node != NULL) {
		ret = read_property_u32(node, "trigger-mode", 10, &value);
		if (ret) {
			vc_err(dev, "%s(): Unable to read trigger-mode from device tree!\n", __FUNCTION__);
		} else {
			vc_mod_set_trigger_mode(cam, value);
		}

		ret = read_property_u32(node, "flash-mode", 10, &value);
		if (ret) {
			vc_err(dev, "%s(): Unable to read flash-mode from device tree!\n", __FUNCTION__);
		} else {
			vc_mod_set_flash_mode(cam, value);
		}
	}
}

static void vc_init_controls(struct tegracam_device *tc_dev) 
{
	struct vc_cam *cam = tegracam_to_cam(tc_dev);
	struct device *dev = tc_dev->dev;
	struct sensor_mode_properties *mode = tegracam_to_mode0(tc_dev);
	struct sensor_control_properties *control;

	if (mode != NULL) {
		control = &mode->control_properties;

		vc_notice(dev, "%s(): Init control gain (min: %d, max: %d, default: %d)\n", __FUNCTION__, 
			cam->ctrl.gain.min, cam->ctrl.gain.max, cam->ctrl.gain.def);
		control->gain_factor = 1;
		control->min_gain_val = cam->ctrl.gain.min;
		control->max_gain_val = cam->ctrl.gain.max;
		control->default_gain = cam->ctrl.gain.def;
		control->step_gain_val = 1;

		vc_notice(dev, "%s(): Init control exposure (min: %d, max: %d, default: %d)\n", __FUNCTION__, 
			cam->ctrl.exposure.min, cam->ctrl.exposure.max, cam->ctrl.exposure.def);
		control->exposure_factor = 1;
		control->min_exp_time.val = cam->ctrl.exposure.min;
		control->max_exp_time.val = cam->ctrl.exposure.max;
		control->default_exp_time.val = cam->ctrl.exposure.def;
		control->step_exp_time.val = 1;

		vc_notice(dev, "%s(): Init control framerate (min: %d, max: %d, default: %d)\n", __FUNCTION__, 
			cam->ctrl.framerate.min, cam->ctrl.framerate.max, cam->ctrl.framerate.def);
		control->framerate_factor = 1;
		control->min_framerate = cam->ctrl.framerate.min;
		control->max_framerate = cam->ctrl.framerate.max;
		control->default_framerate = cam->ctrl.framerate.def;
		control->step_framerate = 1;
	}

	// Workaround: Set this state to enable controls in tegracam_ctrls.c -> tegracam_set_ctrls
	tc_dev->s_data->power->state = SWITCH_ON;
}

static const struct regmap_config vc_regmap_config = {
	.reg_bits = 16,
	.val_bits = 8,
	.cache_type = REGCACHE_RBTREE,
	.use_single_rw = true,
};

static const __u32 ctrl_cid_list[] = {
	TEGRA_CAMERA_CID_GAIN,
	TEGRA_CAMERA_CID_EXPOSURE,
	TEGRA_CAMERA_CID_FRAME_RATE,
	TEGRA_CAMERA_CID_TRIGGER_MODE,
	TEGRA_CAMERA_CID_FLASH_MODE,
	TEGRA_CAMERA_CID_IMAGE_HEIGHT,
};

static struct tegracam_ctrl_ops vc_ctrl_ops = {
	.numctrls = ARRAY_SIZE(ctrl_cid_list),
	.ctrl_cid_list = ctrl_cid_list,
	.set_gain = vc_set_gain,
	.set_exposure = vc_set_exposure,
	.set_frame_rate = vc_set_frame_rate,
	.set_trigger_mode = vc_set_trigger_mode,
	.set_flash_mode = vc_set_flash_mode,
	.set_image_height = vc_set_image_height,
	.set_group_hold = vc_set_group_hold,
};

static int vc_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	// --------------------------------------------------------------------
	// Don't modify this lines of code. It will cause a kernel crash.
	// "Unable to handle kernel paging request at virtual address 7dabb7951ec6f65c"
	struct device *dev = &client->dev;
	struct vc_cam *cam;
	struct tegracam_device *tc_dev;
	int ret;

	vc_notice(dev, "%s(): Probing UNIVERSAL VC MIPI Driver (%s/%s)\n", __func__, __DATE__, __TIME__);
	// --------------------------------------------------------------------

	cam = devm_kzalloc(dev, sizeof(struct vc_cam), GFP_KERNEL);
	if (!cam)
		return -ENOMEM;

	tc_dev = devm_kzalloc(dev, sizeof(struct tegracam_device), GFP_KERNEL);
	if (!tc_dev)
		return -ENOMEM;

	ret = vc_core_init(cam, client);
	if (ret) {
		vc_err(dev, "%s(): Error in vc_core_init!\n", __func__);
		return ret;
	}
	vc_init_trigger(dev, cam);
	vc_adjust_cam_ctrls(cam);
	vc_init_frmfmt(dev, cam);

	// Defined in tegracam_core.c
	// Initializes 
	//   * tc_dev->s_data
	// Calls 
	//   * camera_common_initialize
	tc_dev->client = client;
	tc_dev->dev = dev;
	strncpy(tc_dev->name, "vc-mipi-cam", sizeof(tc_dev->name));
	tc_dev->dev_regmap_config = &vc_regmap_config;
	tc_dev->sensor_ops = &vc_sensor_ops;
	ret = tegracam_device_register(tc_dev);
	if (ret) {
		vc_err(dev, "%s(): Tegra camera device registration failed\n", __FUNCTION__);
		return ret;
	}

	// Defined in tegracam_core.c
	// Initializes
	//   * tc_dev->priv
	//   * tc_dev->s_data->priv
	tegracam_set_privdata(tc_dev, (void *)cam);
	
	// Defined in tegracam_v4l2.c
	// Initializes
	//   * tc_dev->s_data->tegracam_ctrl_hdl
	tc_dev->tcctrl_ops = &vc_ctrl_ops;
	ret = tegracam_v4l2subdev_register(tc_dev, true);
	if (ret) {
       		vc_err(dev, "%s(): Tegra camera subdev registration failed\n", __FUNCTION__);
       		return ret;
    	}

	// This functions need tc_dev->s_data to be initialized.
	vc_init_image(tc_dev);
	vc_init_controls(tc_dev);
	vc_init_lanes(tc_dev);

	return 0;
}

static int vc_remove(struct i2c_client *client)
{
	struct camera_common_data *s_data = to_camera_common_data(&client->dev);
	struct tegracam_device *tc_dev = s_data->tegracam_ctrl_hdl->tc_dev;

	tegracam_v4l2subdev_unregister(tc_dev);
	tegracam_device_unregister(tc_dev);

	return 0;
}

static const struct i2c_device_id vc_id[] = {
	{ "vc-mipi-cam", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, vc_id);

static const struct of_device_id vc_dt_ids[] = {
	{ .compatible = "vc,vc_mipi", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, vc_dt_ids);

static struct i2c_driver vc_i2c_driver = {
	.driver = {
		.name = "vc-mipi-cam",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(vc_dt_ids),
	},
	.id_table = vc_id,
	.probe = vc_probe,
	.remove = vc_remove,
};
module_i2c_driver(vc_i2c_driver);

MODULE_DESCRIPTION("Media Controller driver for IMX327C");
MODULE_AUTHOR("Vision Components GmbH <mipi-tech@vision-components.com>");
MODULE_LICENSE("GPL v2");