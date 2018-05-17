/**
 * Copyright (C) 2017-2018, Decawave Limited, All Rights Reserved
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "sysinit/sysinit.h"
#include "os/os.h"
#include "bsp/bsp.h"
#include "hal/hal_gpio.h"
#include "hal/hal_bsp.h"
#ifdef ARCH_sim
#include "mcu/mcu_sim.h"
#endif

#include "sensor/sensor.h"
#include "sensor/accel.h"
#include "sensor/gyro.h"
#include "sensor/mag.h"
#include "sensor/pressure.h"
#include "console/console.h"

#include "dw1000/dw1000_dev.h"
#include "dw1000/dw1000_hal.h"
#include "dw1000/dw1000_phy.h"
#include "dw1000/dw1000_mac.h"
#include "dw1000/dw1000_rng.h"

#define IMU_READ_RATE (OS_TICKS_PER_SEC/10)

#if MYNEWT_VAL(DW1000_LWIP)
#include <dw1000/dw1000_lwip.h>
#endif


static dwt_config_t mac_config = {
    .chan = 5,                          // Channel number. 
    .prf = DWT_PRF_64M,                 // Pulse repetition frequency. 
    .txPreambLength = DWT_PLEN_256,     // Preamble length. Used in TX only. 
    .rxPAC = DWT_PAC8,                  // Preamble acquisition chunk size. Used in RX only. 
    .txCode = 9,                        // TX preamble code. Used in TX only. 
    .rxCode = 9,                        // RX preamble code. Used in RX only. 
    .nsSFD = 0,                         // 0 to use standard SFD, 1 to use non-standard SFD. 
    .dataRate = DWT_BR_6M8,             // Data rate. 
    .phrMode = DWT_PHRMODE_STD,         // PHY header mode. 
    .sfdTO = (256 + 1 + 8 - 8)         // SFD timeout (preamble length + 1 + SFD length - PAC size). Used in RX only. 
};

static dw1000_phy_txrf_config_t txrf_config = { 
        .PGdly = TC_PGDELAY_CH5,
        //.power = 0x25456585
        .BOOSTNORM = dw1000_power_value(DW1000_txrf_config_9db, 5),
        .BOOSTP500 = dw1000_power_value(DW1000_txrf_config_9db, 5),
        .BOOSTP250 = dw1000_power_value(DW1000_txrf_config_9db, 5),
        .BOOSTP125 = dw1000_power_value(DW1000_txrf_config_9db, 5)
};

static dw1000_rng_config_t rng_config = {
    .tx_holdoff_delay = 0x0800,          // Send Time delay in usec.
    .rx_timeout_period = 0x4000         // Receive response timeout in usec.
};

static twr_frame_t twr[] = {
    [0] = {
        .fctrl = 0x8841,                // frame control (0x8841 to indicate a data frame using 16-bit addressing).
        .PANID = 0xDECA,                // PAN ID (0xDECA)
        .code = DWT_TWR_INVALID
    },
    [1] = {
        .fctrl = 0x8841,                // frame control (0x8841 to indicate a data frame using 16-bit addressing).
        .PANID = 0xDECA,                // PAN ID (0xDECA)
        .code = DWT_TWR_INVALID
    }
};

void print_frame(const char * name, twr_frame_t twr){
    printf("%s{\n\tfctrl:0x%04X,\n", name, twr.fctrl);
    printf("\tseq_num:0x%02X,\n", twr.seq_num);
    printf("\tPANID:0x%04X,\n", twr.PANID);
    printf("\tdst_address:0x%04X,\n", twr.dst_address);
    printf("\tsrc_address:0x%04X,\n", twr.src_address);
    printf("\tcode:0x%04X,\n", twr.code);
    printf("\treception_timestamp:0x%08lX,\n", twr.reception_timestamp); 
    printf("\ttransmission_timestamp:0x%08lX,\n", twr.transmission_timestamp); 
    printf("\trequest_timestamp:0x%08lX,\n", twr.request_timestamp); 
    printf("\tresponse_timestamp:0x%08lX\n}\n", twr.response_timestamp);
}


/* The timer callout */
static struct os_callout twr_callout;
static struct os_callout sensor_callout;

/*
 * Event callback function for timer events. 
*/
static void twr_timer_ev_cb(struct os_event *ev) {
    assert(ev != NULL);

    hal_gpio_toggle(LED_BLINK_PIN);
    dw1000_dev_instance_t * inst = hal_dw1000_inst(0);
    dw1000_rng_instance_t * rng = inst->rng; 
    
    assert(inst->rng->nframes > 0);

#if 0
    if (dw1000_rng_request(inst, 0x1234, DWT_DS_TWR).rx_error)
        printf("twr_timer_ev_cb:rng_request failed [status.mac_error]\n");
#else

   if (inst->status.start_rx_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\": \"start_rx_error\"}\n",os_time_get());
    if (inst->status.start_tx_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"start_tx_error\"}\n",os_time_get());
    if (inst->status.rx_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"rx_error\"}\n",os_time_get());
    if (inst->status.rx_timeout_error)
        printf("{\"utime\": %lu,\"timer_ev_cb\":\"rx_timeout_error\"}\n",os_time_get());
   

    if (inst->status.start_tx_error || inst->status.rx_error ||
        inst->status.rx_timeout_error) {
        dw1000_set_rx_timeout(inst, 0);
        dw1000_start_rx(inst); 
    }
#endif

   else if (twr[0].code == DWT_SS_TWR_FINAL) {
        uint32_t time_of_flight = (uint32_t) dw1000_rng_twr_to_tof(rng);
        float range = dw1000_rng_tof_to_meters(dw1000_rng_twr_to_tof(rng));
        print_frame("trw=", twr[0]);
        twr[0].code = DWT_SS_TWR_END;
        printf("{\"utime\": %lu,\"tof\": %lu,\"range\": %lu,\"res_req\": %lX, \"rec_tra\": %lX}\n", 
            os_time_get(), 
            time_of_flight, 
            (uint32_t)(range * 1000), 
            (twr->response_timestamp - twr->request_timestamp), 
            (twr->transmission_timestamp - twr->reception_timestamp)
        );         
        dw1000_set_rx_timeout(inst, 0);
        dw1000_start_rx(inst); 
    }

    else if (twr[1].code == DWT_DS_TWR_FINAL || twr[1].code == DWT_DS_TWR_EXT_FINAL) {
        uint32_t time_of_flight = (uint32_t) dw1000_rng_twr_to_tof(rng);
        float range = dw1000_rng_tof_to_meters(dw1000_rng_twr_to_tof(rng));
        print_frame("1st=", twr[0]);
        print_frame("2nd=", twr[1]);
        twr[1].code = DWT_DS_TWR_END;
        printf("{\"utime\": %lu,\"tof\": %lu,\"range\": %lu,\"res_req\": %lX, \"rec_tra\": %lX}\n", 
             os_time_get(), 
            time_of_flight, 
            (uint32_t)(range * 1000), 
            (twr->response_timestamp - twr->request_timestamp), 
            (twr->transmission_timestamp - twr->reception_timestamp)
        );

        dw1000_set_rx_timeout(inst, 0);
        dw1000_start_rx(inst); 
    }

    os_callout_reset(&twr_callout, OS_TICKS_PER_SEC/100);
}

int sensor_data_cb(struct sensor* sensor, void *arg, void *data, sensor_type_t type)
{
    struct sensor_accel_data *sad;
    struct sensor_mag_data *smd;
    struct sensor_gyro_data *sgd;
    struct sensor_press_data *spd;
    char tmpstr[13];

    if (type == SENSOR_TYPE_ACCELEROMETER ||
        type == SENSOR_TYPE_LINEAR_ACCEL  ||
        type == SENSOR_TYPE_GRAVITY) {

        console_printf("accel (m/s^2) ");

        sad = (struct sensor_accel_data *) data;
        if (sad->sad_x_is_valid) {
            console_printf("x = %s ", sensor_ftostr(sad->sad_x, tmpstr, 13));
        }
        if (sad->sad_y_is_valid) {
            console_printf("y = %s ", sensor_ftostr(sad->sad_y, tmpstr, 13));
        }
        if (sad->sad_z_is_valid) {
            console_printf("z = %s", sensor_ftostr(sad->sad_z, tmpstr, 13));
        }
        console_printf("\n");
    }

    if (type == SENSOR_TYPE_MAGNETIC_FIELD) {
        smd = (struct sensor_mag_data *) data;
        console_printf("compass (uT)  ");
        if (smd->smd_x_is_valid) {
            console_printf("x = %s ", sensor_ftostr(smd->smd_x, tmpstr, 13));
        }
        if (smd->smd_y_is_valid) {
            console_printf("y = %s ", sensor_ftostr(smd->smd_y, tmpstr, 13));
        }
        if (smd->smd_z_is_valid) {
            console_printf("z = %s ", sensor_ftostr(smd->smd_z, tmpstr, 13));
        }
        console_printf("\n");
    }

    if (type == SENSOR_TYPE_GYROSCOPE) {
        sgd = (struct sensor_gyro_data *) data;
        console_printf("gyro (deg/s)  ");
        
        if (sgd->sgd_x_is_valid) {
            console_printf("x = %s ", sensor_ftostr(sgd->sgd_x, tmpstr, 13));
        }
        if (sgd->sgd_y_is_valid) {
            console_printf("y = %s ", sensor_ftostr(sgd->sgd_y, tmpstr, 13));
        }
        if (sgd->sgd_z_is_valid) {
            console_printf("z = %s ", sensor_ftostr(sgd->sgd_z, tmpstr, 13));
        }
        console_printf("\n");
    }
    
    if (type == SENSOR_TYPE_PRESSURE) {
        spd = (struct sensor_press_data *) data;
        if (spd->spd_press_is_valid) {
            console_printf("pressure = %s Pa",
                           sensor_ftostr(spd->spd_press, tmpstr, 13));
        }
        console_printf("\n");
    }
    return (0);
}

/*
 * Event callback function for timer events. 
*/
static void sensor_timer_ev_cb(struct os_event *ev) {
    int rc;
    struct sensor *s;
    sensor_type_t sensor_types[] = {SENSOR_TYPE_ACCELEROMETER,
                                    SENSOR_TYPE_GYROSCOPE,
                                    SENSOR_TYPE_MAGNETIC_FIELD,
                                    SENSOR_TYPE_PRESSURE,
                                    SENSOR_TYPE_NONE};
        
    assert(ev != NULL);
    int i=0;
    while (sensor_types[i] != SENSOR_TYPE_NONE)
    {
        s = sensor_mgr_find_next_bytype(sensor_types[i], NULL);
        if (s)
        {
            rc = sensor_read(s,
                             sensor_types[i],
                             &sensor_data_cb,
                             0,
                             OS_TICKS_PER_SEC/10);
            if (rc) console_printf("Error: failed to read sensor\r\n");
        }

        i++;
    }
    
    os_callout_reset(&sensor_callout, IMU_READ_RATE);
}


static void init_timer(void) {
    /*
     * Initialize the callout for a timer event.
     */
    os_callout_init(&twr_callout, os_eventq_dflt_get(), twr_timer_ev_cb, NULL);
    os_callout_reset(&twr_callout, OS_TICKS_PER_SEC);

    os_callout_init(&sensor_callout, os_eventq_dflt_get(), sensor_timer_ev_cb, NULL);
    os_callout_reset(&sensor_callout, OS_TICKS_PER_SEC);

}

int main(int argc, char **argv){
    int rc;

    sysinit();
    hal_gpio_init_out(LED_BLINK_PIN, 1);
    hal_gpio_init_out(LED_1, 1);
    hal_gpio_init_out(LED_3, 1);

    dw1000_dev_instance_t * inst = hal_dw1000_inst(0);
    dw1000_softreset(inst);
    dw1000_phy_init(inst, &txrf_config);    
 
    inst->PANID = 0xDECA;
    inst->my_short_address = 0x4321;
    dw1000_set_panid(inst,inst->PANID);
    dw1000_mac_init(inst, &mac_config);
    dw1000_rng_init(inst, &rng_config, sizeof(twr)/sizeof(twr_frame_t));
    dw1000_rng_set_frames(inst, twr, sizeof(twr)/sizeof(twr_frame_t));


    printf("device_id=%lX\n",inst->device_id);
    printf("PANID=%X\n",inst->PANID);
    printf("DeviceID =%X\n",inst->my_short_address);
    printf("partID =%lX\n",inst->partID);
    printf("lotID =%lX\n",inst->lotID);
    printf("xtal_trim =%X\n",inst->xtal_trim);

    dw1000_set_rx_timeout(inst, 0);
    dw1000_start_rx(inst); 

    init_timer();

    while (1) {
        os_eventq_run(os_eventq_dflt_get());   
    }

    assert(0);

    return rc;
}

