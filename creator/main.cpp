/*
 * Copyright 2016 <Admobilize>
 * MATRIX Labs  [http://creator.matrix.one]
 * This file is part of MATRIX Creator firmware for MCU
 * Author: Andrés Calderón [andres.calderon@admobilize.com]
 *
 * MATRIX Creator firmware for MCU is free software: you can redistribute
 * it and/or modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ch.h"
#include "hal.h"
#include "board.h"

#include <math.h>
#include <string.h>
#include <mcuconf.h>

#include "./i2c.h"
#include "./sensors_data.h"
#include "./mpl3115a2.h"
#include "./lsm9ds1.h"
#include "./hts221.h"
#include "./veml6070.h"
#include "./DCM.h"


extern "C" {
#include "atmel_psram.h"
}

#define twoKpDef  (2.0f * 0.75f)  //works with and without mag enabled
#define twoKiDef  (2.0f * 0.1625f)
#define betaDef  0.085f
//Used for DCM filter
const float Kp_ROLLPITCH = 1.2f;  //was .3423
const float Ki_ROLLPITCH = 0.0234f;
const float Kp_YAW = 1.75f;   // was 1.2 and 0.02
const float Ki_YAW = 0.002f;

const uint32_t kFirmwareCreatorID = 0x10;
const uint32_t kFirmwareVersion = 0x161026; /* 0xYYMMDD */

/* Global objects */
creator::I2C i2c;  // TODO(andres.calderon@admobilize.com): avoid global objects

void psram_copy(uint8_t mem_offset, char *data, uint8_t len) {
  register char *psram = (char *)PSRAM_BASE_ADDRESS;

  for (int i = 0; i < len; i++) {
    psram[mem_offset + i] = data[i];
  }
}

static WORKING_AREA(waEnvThread, 256);
static msg_t EnvThread(void *arg) {
  (void)arg;

  creator::MPL3115A2 mpl3115a2(&i2c);
  creator::HTS221 hts221(&i2c);
  creator::VEML6070 veml6070(&i2c);

  mpl3115a2.Begin();
  hts221.Begin();
  veml6070.Begin();

  PressureData press;
  HumidityData hum;
  UVData uv;
  MCUData mcu_info;

  mcu_info.ID = kFirmwareCreatorID;
  mcu_info.version = kFirmwareVersion;

  while (true) {
    palSetPad(IOPORT3, 17);
    chThdSleepMilliseconds(1);
    palClearPad(IOPORT3, 17);

    hts221.GetData(hum.humidity, hum.temperature);

    press.altitude = mpl3115a2.GetAltitude();
    press.pressure = mpl3115a2.GetPressure();
    press.temperature = mpl3115a2.GetTemperature();

    uv.UV = veml6070.GetUV();

    psram_copy(mem_offset_mcu, (char *)&mcu_info, sizeof(mcu_info));
    psram_copy(mem_offset_press, (char *)&press, sizeof(press));
    psram_copy(mem_offset_humidity, (char *)&hum, sizeof(hum));
    psram_copy(mem_offset_uv, (char *)&uv, sizeof(uv));
  }
  return (0);
}


static WORKING_AREA(waIMUThread, 512);
static msg_t IMUThread(void *arg) {
  (void)arg;

  LSM9DS1 imu(&i2c, IMU_MODE_I2C, 0x6A, 0x1C);
  imu.begin();

  IMUData data;
  float values[10]; // TODO: Verify the length
  float ypr[3]; // yaw, pith, roll angles in rad

  float last, now,elapsed;      // sample period expressed in milliseconds

  // DCM Initialization
  DCM dcm;
  dcm = DCM();
  // Reading data from the sensors
  imu.readAccel();
  imu.readGyro();
  imu.readMag();

  // organize the sensor data for DCM input
  values[0] = imu.calcAccel(imu.ax);
  values[1] = imu.calcAccel(imu.ay);
  values[2] = imu.calcAccel(imu.az);
  values[3] = imu.calcGyro(imu.gx);
  values[4] = imu.calcGyro(imu.gy);
  values[5] = imu.calcGyro(imu.gz);
  values[6] = imu.calcMag(imu.mx);
  values[7] = imu.calcMag(imu.my);
  values[8] = imu.calcMag(imu.mz);

  // values[9] = maghead.iheading(
  //   1, 0, 0,
  //   values[0],
  //   values[1],
  //   values[2],
  //   values[6],
  //   values[7],
  //   values[8]);

  dcm.setSensorVals(values);
  dcm.DCM_init(Kp_ROLLPITCH, Ki_ROLLPITCH, Kp_YAW, Ki_YAW);

  while (true) {

    // Reading data from the sensors
    imu.readAccel();
    imu.readGyro();
    imu.readMag();

    // organize the sensor data for DCM input
    values[0] = imu.calcAccel(imu.ax);
    values[1] = imu.calcAccel(imu.ay);
    values[2] = imu.calcAccel(imu.az);
    values[3] = imu.calcGyro(imu.gx);
    values[4] = imu.calcGyro(imu.gy);
    values[5] = imu.calcGyro(imu.gz);
    values[6] = imu.calcMag(imu.mx);
    values[7] = imu.calcMag(imu.my);
    values[8] = imu.calcMag(imu.mz);

    // values[9] = maghead.iheading(
    //   1, 0, 0,
    //   values[0],
    //   values[1],
    //   values[2],
    //   values[6],
    //   values[7],
    //   values[8]);

    // Sampling time

    now = chTimeNow();
    elapsed = (now - last)/CH_FREQUENCY;
    last = now;

    dcm.G_Dt = elapsed;
    dcm.setSensorVals(values);
    dcm.calDCM();
    dcm.getEulerDeg(ypr);

    // Organize IMU data for ram writing
    data.accel_x  = values[0];
    data.accel_y  = values[1];
    data.accel_z  = values[2];

    // data.gyro_x = elapsed;
    // data.gyro_y = values[4];
    // data.gyro_z = values[5];

    // data.mag_x = values[6];
    // data.mag_y = values[7];
    // data.mag_z = values[8];

    // Getting yaw, pitch and roll from DCM output
    data.yaw = ypr[0];
    data.pitch = ypr[1];
    data.roll = ypr[2];

    // TODO: just for debug
    data.mag_x = elapsed;
    data.mag_y = CH_FREQUENCY;
    data.mag_z = values[8];

    data.gyro_x = atan2(data.mag_y, -data.mag_x) * 180.0 / M_PI;
    data.gyro_y = atan2(data.accel_y, data.accel_z) * 180.0 / M_PI;
    data.gyro_z = atan2(-data.accel_x,
      sqrt(data.accel_y * data.accel_y + data.accel_z * data.accel_z)) * 180.0 / M_PI;

    psram_copy(mem_offset_imu, (char *)&data, sizeof(data));

    // chThdSleepMilliseconds(20); // TODO : Probably we need this faster
  }
  return (0);
}

/*
 * Application entry point.
 */
int main(void) {


  halInit();

  chSysInit();

  /* Configure EBI I/O for psram connection*/
  PIO_Configure(pinPsram, PIO_LISTSIZE(pinPsram));

  /* complete SMC configuration between PSRAM and SMC waveforms.*/
  BOARD_ConfigurePSRAM(SMC);

  i2c.Init();
  /* Creates the imu thread. */
  chThdCreateStatic(waIMUThread, sizeof(waIMUThread), NORMALPRIO, IMUThread,
                    NULL);

  /* Creates the hum thread. */
  chThdCreateStatic(waEnvThread, sizeof(waEnvThread), NORMALPRIO, EnvThread,
                    NULL);

  return (0);
}
