/*
 *  Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 */
#include "ssp.h"
#include <mach/sec_debug.h>


#ifdef CONFIG_HAS_EARLYSUSPEND
static void ssp_early_suspend(struct early_suspend *handler);
static void ssp_late_resume(struct early_suspend *handler);
#endif

void ssp_enable(struct ssp_data *data, bool enable)
{
	pr_info("[SSP] %s, enable = %d, old enable = %d\n",
		__func__, enable, data->bSspShutdown);

	if (enable && data->bSspShutdown) {
		data->bSspShutdown = false;
		enable_irq(data->iIrq);
		enable_irq_wake(data->iIrq);
	} else if (!enable && !data->bSspShutdown) {
		data->bSspShutdown = true;
		disable_irq(data->iIrq);
		disable_irq_wake(data->iIrq);
	} else {
		pr_err("[SSP] %s, error / enable = %d, old enable = %d\n",
			__func__, enable, data->bSspShutdown);
	}
}
/************************************************************************/
/* interrupt happened due to transition/change of SSP MCU		*/
/************************************************************************/

static irqreturn_t sensordata_irq_thread_fn(int iIrq, void *dev_id)
{
	struct ssp_data *data = dev_id;
	struct timespec ts;

	ts = ktime_to_timespec(ktime_get_boottime());
	data->timestamp = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
	select_irq_msg(data);
	data->uIrqCnt++;

	return IRQ_HANDLED;
}

/*************************************************************************/
/* initialize sensor hub						 */
/*************************************************************************/

static void initialize_variable(struct ssp_data *data)
{
	int iSensorIndex;

	for (iSensorIndex = 0; iSensorIndex < SENSOR_MAX; iSensorIndex++) {
		data->adDelayBuf[iSensorIndex] = DEFUALT_POLLING_DELAY;
		data->batchLatencyBuf[iSensorIndex] = 0;
		data->batchOptBuf[iSensorIndex] = 0;
		data->aiCheckStatus[iSensorIndex] = INITIALIZATION_STATE;
		data->lastTimestamp[iSensorIndex] = 0;
		data->reportedData[iSensorIndex] = false;
	}

	atomic_set(&data->aSensorEnable, 0);
	data->iLibraryLength = 0;
	data->uSensorState = 0;
	data->uFactoryProxAvg[0] = 0;
	data->uMagCntlRegData = 1;

	data->uResetCnt = 0;
	data->uInstFailCnt = 0;
	data->uTimeOutCnt = 0;
	data->uSsdFailCnt = 0;
	data->uBusyCnt = 0;
	data->uIrqCnt = 0;
	data->uIrqFailCnt = 0;
	data->uMissSensorCnt = 0;

	data->bSspShutdown = true;
	data->bProximityRawEnabled = false;
	data->bGeomagneticRawEnabled = false;
	data->bBarcodeEnabled = false;
	data->bAccelAlert = false;
	data->bTimeSyncing = true;

	data->accelcal.x = 0;
	data->accelcal.y = 0;
	data->accelcal.z = 0;

	data->gyrocal.x = 0;
	data->gyrocal.y = 0;
	data->gyrocal.z = 0;

	data->magoffset.x = 0;
	data->magoffset.y = 0;
	data->magoffset.z = 0;

	data->iPressureCal = 0;
	data->uProxCanc = 0;
	data->uProxHiThresh = 0;
	data->uProxLoThresh = 0;
	data->uGyroDps = GYROSCOPE_DPS500;
	data->uIr_Current = DEFUALT_IR_CURRENT;

	data->mcu_device = NULL;
	data->acc_device = NULL;
	data->gyro_device = NULL;
	data->mag_device = NULL;
	data->prs_device = NULL;
	data->prox_device = NULL;
	data->light_device = NULL;
	data->ges_device = NULL;

	data->voice_device = NULL;
	data->bMcuDumpMode = ssp_check_sec_dump_mode();
	INIT_LIST_HEAD(&data->pending_list);

	data->sealevelpressure = 0;
	initialize_function_pointer(data);
}

int initialize_mcu(struct ssp_data *data)
{
	int iRet = 0;

	iRet = get_chipid(data);
	pr_info("[SSP] MCU device ID = %d, reading ID = %d\n", DEVICE_ID, iRet);
	if (iRet != DEVICE_ID) {
		if (iRet < 0) {
			pr_err("[SSP]: %s - MCU is not working : 0x%x\n",
				__func__, iRet);
		} else {
			pr_err("[SSP]: %s - MCU identification failed\n",
				__func__);
			iRet = -ENODEV;
		}
		goto out;
	}

	iRet = set_sensor_position(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - set_sensor_position failed\n", __func__);
		goto out;
	}

	iRet = set_magnetic_static_matrix(data);
	if (iRet < 0)
		pr_err("[SSP] %s - set_magnetic_static_matrix failed\n", __func__);

	iRet = get_fuserom_data(data);
	if (iRet < 0)
		pr_err("[SSP]: %s - get_fuserom_data failed\n", __func__);

	data->uSensorState = get_sensor_scanning_info(data);
	if (data->uSensorState == 0) {
		pr_err("[SSP]: %s - get_sensor_scanning_info failed\n",
			__func__);
		iRet = ERROR;
		goto out;
	}

	iRet = SUCCESS;
out:
	return iRet;
}

static int initialize_irq(struct ssp_data *data)
{
	int iRet, iIrq;

	iRet = gpio_request(data->mcu_int1, "mcu_ap_int");
	if (iRet < 0) {
		pr_err("[SSP] %s, gpio %d request failed (%d)\n",
		       __func__, data->mcu_int1, iRet);
		return iRet;
	}

	iRet = gpio_direction_input(data->mcu_int1);
	if (iRet < 0) {
		pr_err("[SSP] %s, failed to set gpio %d as input (%d)\n",
		       __func__, data->mcu_int1, iRet);
		goto err_irq_direction_input;
	}

	iIrq = gpio_to_irq(data->mcu_int1);

	pr_info("[SSP] requesting IRQ %d\n", iIrq);
	iRet = request_threaded_irq(iIrq, NULL, sensordata_irq_thread_fn,
				    IRQF_TRIGGER_FALLING, "SSP_Int", data);
	if (iRet < 0) {
		pr_err("[SSP] %s, request_irq(%d) failed for gpio %d (%d)\n",
		       __func__, iIrq, iIrq, iRet);
		goto err_request_irq;
	}

	/* start with interrupts disabled */
	data->iIrq = iIrq;
	disable_irq(data->iIrq);
	return 0;

err_request_irq:
err_irq_direction_input:
	gpio_free(data->mcu_int1);
	return iRet;
}

static void work_function_firmware_update(struct work_struct *work)
{
	struct ssp_data *data = container_of((struct delayed_work *)work,
				struct ssp_data, work_firmware);
	int iRet;

	pr_info("[SSP] %s, is called\n", __func__);

	iRet = forced_to_download_binary(data, KERNEL_BINARY);
	if (iRet < 0) {
		ssp_dbg("[SSP] %s, forced_to_download_binary failed!\n",
			__func__);
		return;
	}

	data->uCurFirmRev = get_firmware_rev(data);
	pr_info("[SSP] MCU Firm Rev : New = %8u\n",
		data->uCurFirmRev);
}

static int ssp_probe(struct spi_device *spi)
{
	struct ssp_data *data;
	struct ssp_platform_data *pdata;
	int iRet = 0;

	pr_info("[SSP] %s, is called\n", __func__);

	pdata = spi->dev.platform_data;
	if (pdata == NULL) {
		pr_err("[SSP] %s, platform_data is null\n", __func__);
		iRet = -ENOMEM;
		goto exit;
	}

	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (data == NULL) {
		pr_err("[SSP] %s, failed to allocate memory for data\n", __func__);
		iRet = -ENOMEM;
		goto exit;
	}

	data->set_mcu_reset = pdata->set_mcu_reset;
	data->ap_int = pdata->ap_int;
	data->mcu_int1 = pdata->mcu_int1;
	data->mcu_int2 = pdata->mcu_int2;

	/* AP system_rev */
	if (pdata->check_ap_rev)
		data->ap_rev = pdata->check_ap_rev();
	else
		data->ap_rev = 0;

	/* Get sensor positions */
	if (pdata->get_positions) {
		pdata->get_positions(&data->accel_position,
			&data->mag_position);
	} else {
		data->accel_position = 0;
		data->mag_position = 0;
	}
	if (pdata->mag_matrix) {
		data->mag_matrix_size = pdata->mag_matrix_size;
		data->mag_matrix = pdata->mag_matrix;
	}

#ifdef CONFIG_SENSORS_SSP_SHTC1
	/* Get pdata for cp thermister */
	if (pdata->cp_thm_adc_table) {
		data->cp_thm_adc_channel = pdata->cp_thm_adc_channel;
		data->cp_thm_adc_arr_size= pdata->cp_thm_adc_arr_size;
		data->cp_thm_adc_table = pdata->cp_thm_adc_table;
	}
	/* Get pdata for batt thermister */
	if (pdata->batt_thm_adc_table) {
		data->batt_thm_adc_arr_size= pdata->batt_thm_adc_arr_size;
		data->batt_thm_adc_table = pdata->batt_thm_adc_table;
	}
	/* Get pdata for chg thermister */
	if (pdata->chg_thm_adc_table) {
		data->chg_thm_adc_arr_size= pdata->chg_thm_adc_arr_size;
		data->chg_thm_adc_table = pdata->chg_thm_adc_table;
	}
#endif

	spi->mode = SPI_MODE_1;
	if (spi_setup(spi)) {
		pr_err("[SSP] %s, failed to setup spi\n", __func__);
		goto err_setup;
	}

	data->bProbeIsDone = false;
	data->fw_dl_state = FW_DL_STATE_NONE;
	data->spi = spi;
	spi_set_drvdata(spi, data);

#ifdef CONFIG_SENSORS_SSP_SHTC1
	mutex_init(&data->cp_temp_adc_lock);
	mutex_init(&data->bulk_temp_read_lock);
#endif
#ifdef CONFIG_SENSORS_SSP_STM
	mutex_init(&data->comm_mutex);
	mutex_init(&data->pending_mutex);
#endif

	if (data->set_mcu_reset == NULL) {
		pr_err("[SSP] %s, function callback is null\n", __func__);
		iRet = -EIO;
		goto err_reset_null;
	}

	pr_info("\n#####################################################\n");

	INIT_DELAYED_WORK(&data->work_firmware, work_function_firmware_update);

	wake_lock_init(&data->ssp_wake_lock,
		WAKE_LOCK_SUSPEND, "ssp_wake_lock");

	iRet = initialize_input_dev(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create input device\n", __func__);
		goto err_input_register_device;
	}

	iRet = initialize_debug_timer(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create workqueue\n", __func__);
		goto err_create_workqueue;
	}

	iRet = initialize_irq(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create irq\n", __func__);
		goto err_setup_irq;
	}

	iRet = initialize_sysfs(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create sysfs\n", __func__);
		goto err_sysfs_create;
	}

	iRet = initialize_event_symlink(data);
	if (iRet < 0) {
		pr_err("[SSP]: %s - could not create symlink\n", __func__);
		goto err_symlink_create;
	}

	initialize_variable(data);

	ssp_enable(data, true);
	/* check boot loader binary */
	data->fw_dl_state = check_fwbl(data);

	if (data->fw_dl_state == FW_DL_STATE_NONE) {
		iRet = initialize_mcu(data);
		if (iRet == ERROR) {
			data->uResetCnt++;
			toggle_mcu_reset(data);
			msleep(SSP_SW_RESET_TIME);
			initialize_mcu(data);
		} else if (iRet < ERROR) {
			pr_err("[SSP] %s, initialize_mcu failed\n", __func__);
			goto err_read_reg;
		}
	}

#ifdef CONFIG_HAS_EARLYSUSPEND
	data->early_suspend.suspend = ssp_early_suspend;
	data->early_suspend.resume = ssp_late_resume;
	register_early_suspend(&data->early_suspend);
#endif

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* init sensorhub device */
	iRet = ssp_sensorhub_initialize(data);
	if (iRet < 0) {
		pr_err("[SSP] %s, ssp_sensorhub_initialize err(%d)", __func__, iRet);
		ssp_sensorhub_remove(data);
	}
#endif

	data->bMcuDumpMode = ssp_check_sec_dump_mode();
	pr_info("[SSP]: %s - setup debuglevel %d!\n", __func__, data->bMcuDumpMode);

	pr_info("[SSP]: %s - probe success!\n", __func__);

	enable_debug_timer(data);

	if (data->fw_dl_state == FW_DL_STATE_NEED_TO_SCHEDULE) {
		pr_info("[SSP] Firmware update is scheduled\n");
		schedule_delayed_work(&data->work_firmware,
				msecs_to_jiffies(1000));
		data->fw_dl_state = FW_DL_STATE_SCHEDULED;
	} else if (data->fw_dl_state == FW_DL_STATE_FAIL) {
		data->bSspShutdown = true;
	}

	data->bProbeIsDone = true;
	iRet = 0;

	goto exit;

err_read_reg:
err_symlink_create:
	remove_sysfs(data);
err_sysfs_create:
	free_irq(data->iIrq, data);
	gpio_free(data->mcu_int1);
err_setup_irq:
	destroy_workqueue(data->debug_wq);
err_create_workqueue:
	remove_input_dev(data);
err_input_register_device:
	wake_lock_destroy(&data->ssp_wake_lock);
err_reset_null:
#ifdef CONFIG_SENSORS_SSP_STM
	mutex_destroy(&data->comm_mutex);
	mutex_destroy(&data->pending_mutex);
#endif
#ifdef CONFIG_SENSORS_SSP_SHTC1
	mutex_destroy(&data->bulk_temp_read_lock);
	mutex_destroy(&data->cp_temp_adc_lock);
#endif

err_setup:
	kfree(data);
	pr_err("[SSP] %s, probe failed!\n", __func__);
exit:
	pr_info("#####################################################\n\n");
	return iRet;
}

static void ssp_shutdown(struct spi_device *spi)
{
	struct ssp_data *data = spi_get_drvdata(spi);

	func_dbg();
	if (data->bProbeIsDone == false)
		goto exit;

	if (data->fw_dl_state >= FW_DL_STATE_SCHEDULED &&
		data->fw_dl_state < FW_DL_STATE_DONE) {
		pr_err("%s, cancel_delayed_work_sync state = %d\n",
			__func__, data->fw_dl_state);
		cancel_delayed_work_sync(&data->work_firmware);
	}

	ssp_enable(data, false);
	disable_debug_timer(data);

	clean_pending_list(data);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&data->early_suspend);
#endif

	free_irq(data->iIrq, data);
	gpio_free(data->mcu_int1);

	remove_event_symlink(data);
	remove_sysfs(data);
	remove_input_dev(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	ssp_sensorhub_remove(data);
#endif

	destroy_workqueue(data->debug_wq);
	wake_lock_destroy(&data->ssp_wake_lock);
#ifdef CONFIG_SENSORS_SSP_SHTC1
	mutex_destroy(&data->bulk_temp_read_lock);
	mutex_destroy(&data->cp_temp_adc_lock);
#endif
#ifdef CONFIG_SENSORS_SSP_STM
	mutex_destroy(&data->comm_mutex);
	mutex_destroy(&data->pending_mutex);
#endif
	toggle_mcu_reset(data);
	pr_info("%s done\n", __func__);
exit:
	kfree(data);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void ssp_early_suspend(struct early_suspend *handler)
{
	struct ssp_data *data;
	data = container_of(handler, struct ssp_data, early_suspend);

	func_dbg();
	disable_debug_timer(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* give notice to user that AP goes to sleep */
	ssp_sensorhub_report_notice(data, MSG2SSP_AP_STATUS_SLEEP);
	ssp_sleep_mode(data);
	data->uLastAPState = MSG2SSP_AP_STATUS_SLEEP;
#else
	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_sleep_mode(data);
#endif
}

static void ssp_late_resume(struct early_suspend *handler)
{
	struct ssp_data *data;
	data = container_of(handler, struct ssp_data, early_suspend);

	func_dbg();
	enable_debug_timer(data);

#ifdef CONFIG_SENSORS_SSP_SENSORHUB
	/* give notice to user that AP goes to sleep */
	ssp_sensorhub_report_notice(data, MSG2SSP_AP_STATUS_WAKEUP);
	ssp_resume_mode(data);
	data->uLastAPState = MSG2SSP_AP_STATUS_WAKEUP;
#else
	if (atomic_read(&data->aSensorEnable) > 0)
		ssp_resume_mode(data);
#endif
}

#else /* no early suspend */

static int ssp_suspend(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ssp_data *data = spi_get_drvdata(spi);

	func_dbg();
	data->uLastResumeState = MSG2SSP_AP_STATUS_SUSPEND;
	disable_debug_timer(data);

	if (SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_SUSPEND, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_SUSPEND failed\n",
			__func__);
	data->bTimeSyncing = false;
	disable_irq(data->iIrq);
	return 0;
}

static int ssp_resume(struct device *dev)
{
	struct spi_device *spi = to_spi_device(dev);
	struct ssp_data *data = spi_get_drvdata(spi);
	enable_irq(data->iIrq);
	func_dbg();
	enable_debug_timer(data);

	if (SUCCESS != ssp_send_cmd(data, MSG2SSP_AP_STATUS_RESUME, 0))
		pr_err("[SSP]: %s MSG2SSP_AP_STATUS_RESUME failed\n",
			__func__);
	data->uLastResumeState = MSG2SSP_AP_STATUS_RESUME;

	return 0;
}

static const struct dev_pm_ops ssp_pm_ops = {
	.suspend = ssp_suspend,
	.resume = ssp_resume
};
#endif /* CONFIG_HAS_EARLYSUSPEND */

static const struct spi_device_id ssp_id[] = {
	{"ssp-spi", 0},
	{}
};

MODULE_DEVICE_TABLE(spi, ssp_id);

static struct spi_driver ssp_driver = {
	.probe = ssp_probe,
	.shutdown = ssp_shutdown,
	.id_table = ssp_id,
	.driver = {
#ifndef CONFIG_HAS_EARLYSUSPEND
		   .pm = &ssp_pm_ops,
#endif
		   .owner = THIS_MODULE,
		   .name = "ssp-spi"
		},
};

module_spi_driver(ssp_driver);
MODULE_DESCRIPTION("ssp spi driver");
MODULE_AUTHOR("Samsung Electronics");
MODULE_LICENSE("GPL");
