/*
 $License:
    Copyright (C) 2011-2012 InvenSense Corporation, All Rights Reserved.
    See included License.txt for License information.
 $
 */
/**
 *  @addtogroup  DRIVERS Sensor Driver Layer
 *  @brief       Hardware drivers to communicate with sensors via I2C.
 *
 *  @{
 *      @file       inv_mpu.c
 *      @brief      An I2C-based driver for Invensense gyroscopes.
 *      @details    This driver currently works for the following devices:
 *                  MPU6050
 *                  MPU6500
 *                  MPU9150 (or MPU6050 w/ AK8975 on the auxiliary bus)
 *                  MPU9250 (or MPU6500 w/ AK8963 on the auxiliary bus)
 */
#include "inv_mpu.h"
#include "i2c/i2c.h"
#include "timer/timer.h"
#ifdef __AVR__
#  include <avr/pgmspace.h>
#endif
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "util/util.h"
#define MPU6050
#define delay_ms _delay_ms
#define i2c_write !twi_writeToPointer
#define i2c_read !twi_readFromPointer

static int8_t set_int_enable(unsigned char enable);

/* Hardware registers needed by driver. */
struct gyro_reg_s {
  unsigned char who_am_i;
  unsigned char rate_div;
  unsigned char lpf;
  unsigned char prod_id;
  unsigned char user_ctrl;
  unsigned char fifo_en;
  unsigned char gyro_cfg;
  unsigned char accel_cfg;
  unsigned char accel_cfg2;
  unsigned char lp_accel_odr;
  unsigned char motion_thr;
  unsigned char motion_dur;
  unsigned char fifo_count_h;
  unsigned char fifo_r_w;
  unsigned char raw_gyro;
  unsigned char raw_accel;
  unsigned char temp;
  unsigned char int_enable;
  unsigned char dmp_int_status;
  unsigned char int_status;
  unsigned char accel_intel;
  unsigned char pwr_mgmt_1;
  unsigned char pwr_mgmt_2;
  unsigned char int_pin_cfg;
  unsigned char mem_r_w;
  unsigned char accel_offs;
  unsigned char i2c_mst;
  unsigned char bank_sel;
  unsigned char mem_start_addr;
  unsigned char prgm_start_h;
};

/* Information specific to a particular device. */
struct hw_s {
  unsigned char addr;
  unsigned short max_fifo;
  unsigned char num_reg;
  unsigned short temp_sens;
  short temp_offset;
  unsigned short bank_size;
};

/* When entering motion interrupt mode, the driver keeps track of the
 * previous state so that it can be restored at a later time.
 * TODO: This is tacky. Fix it.
 */
struct motion_int_cache_s {
  unsigned short gyro_fsr;
  unsigned char accel_fsr;
  unsigned short lpf;
  unsigned short sample_rate;
  unsigned char sensors_on;
  unsigned char fifo_sensors;
  unsigned char dmp_on;
};

/* Cached chip configuration data.
 * TODO: A lot of these can be handled with a bitmask.
 */
struct chip_cfg_s {
  /* Matches gyro_cfg >> 3 & 0x03 */
  unsigned char gyro_fsr;
  /* Matches accel_cfg >> 3 & 0x03 */
  unsigned char accel_fsr;
  /* Enabled sensors. Uses same masks as fifo_en, NOT pwr_mgmt_2. */
  unsigned char sensors;
  /* Matches config register. */
  unsigned char lpf;
  unsigned char clk_src;
  /* Sample rate, NOT rate divider. */
  unsigned short sample_rate;
  /* Matches fifo_en register. */
  unsigned char fifo_enable;
  /* Matches int enable register. */
  unsigned char int_enable;
  /* 1 if devices on auxiliary I2C bus appear on the primary. */
  unsigned char bypass_mode;
  /* 1 if half-sensitivity.
   * NOTE: This doesn't belong here, but everything else in hw_s is const,
   * and this allows us to save some precious RAM.
   */
  unsigned char accel_half;
  /* 1 if device in low-power accel-only mode. */
  unsigned char lp_accel_mode;
  /* 1 if interrupts are only triggered on motion events. */
  unsigned char int_motion_only;
  struct motion_int_cache_s cache;
  /* 1 for active low interrupts. */
  unsigned char active_low_int;
  /* 1 for latched interrupts. */
  unsigned char latched_int;
  /* 1 if DMP is enabled. */
  unsigned char dmp_on;
  /* Ensures that DMP will only be loaded once. */
  unsigned char dmp_loaded;
  /* Sampling rate used when DMP is enabled. */
  unsigned short dmp_sample_rate;
};

/* Gyro driver state variables. */
struct gyro_state_s {
  const struct gyro_reg_s *reg;
  const struct hw_s *hw;
  struct chip_cfg_s chip_cfg;
  const struct test_s *test;
};

/* Filter configurations. */
enum lpf_e {
  INV_FILTER_256HZ_NOLPF2 = 0,
  INV_FILTER_188HZ,
  INV_FILTER_98HZ,
  INV_FILTER_42HZ,
  INV_FILTER_20HZ,
  INV_FILTER_10HZ,
  INV_FILTER_5HZ,
  INV_FILTER_2100HZ_NOLPF,
  NUM_FILTER
};

/* Full scale ranges. */
enum gyro_fsr_e {
  INV_FSR_250DPS = 0,
  INV_FSR_500DPS,
  INV_FSR_1000DPS,
  INV_FSR_2000DPS,
  NUM_GYRO_FSR
};

/* Full scale ranges. */
enum accel_fsr_e {
  INV_FSR_2G = 0,
  INV_FSR_4G,
  INV_FSR_8G,
  INV_FSR_16G,
  NUM_ACCEL_FSR
};

/* Clock sources. */
enum clock_sel_e { INV_CLK_INTERNAL = 0, INV_CLK_PLL, NUM_CLK };

/* Low-power accel wakeup rates. */
enum lp_accel_rate_e {
#if defined MPU6050
  INV_LPA_1_25HZ,
  INV_LPA_5HZ,
  INV_LPA_20HZ,
  INV_LPA_40HZ
#endif
};

#define BIT_I2C_MST_VDDIO (0x80)
#define BIT_FIFO_EN (0x40)
#define BIT_DMP_EN (0x80)
#define BIT_FIFO_RST (0x04)
#define BIT_DMP_RST (0x08)
#define BIT_FIFO_OVERFLOW (0x10)
#define BIT_DATA_RDY_EN (0x01)
#define BIT_DMP_INT_EN (0x02)
#define BIT_MOT_INT_EN (0x40)
#define BITS_FSR (0x18)
#define BITS_LPF (0x07)
#define BITS_HPF (0x07)
#define BITS_CLK (0x07)
#define BIT_FIFO_SIZE_1024 (0x40)
#define BIT_FIFO_SIZE_2048 (0x80)
#define BIT_FIFO_SIZE_4096 (0xC0)
#define BIT_RESET (0x80)
#define BIT_SLEEP (0x40)
#define BIT_S0_DELAY_EN (0x01)
#define BIT_S2_DELAY_EN (0x04)
#define BITS_SLAVE_LENGTH (0x0F)
#define BIT_SLAVE_BYTE_SW (0x40)
#define BIT_SLAVE_GROUP (0x10)
#define BIT_SLAVE_EN (0x80)
#define BIT_I2C_READ (0x80)
#define BITS_I2C_MASTER_DLY (0x1F)
#define BIT_AUX_IF_EN (0x20)
#define BIT_ACTL (0x80)
#define BIT_LATCH_EN (0x20)
#define BIT_ANY_RD_CLR (0x10)
#define BIT_BYPASS_EN (0x02)
#define BITS_WOM_EN (0xC0)
#define BIT_LPA_CYCLE (0x20)
#define BIT_STBY_XA (0x20)
#define BIT_STBY_YA (0x10)
#define BIT_STBY_ZA (0x08)
#define BIT_STBY_XG (0x04)
#define BIT_STBY_YG (0x02)
#define BIT_STBY_ZG (0x01)
#define BIT_STBY_XYZA (BIT_STBY_XA | BIT_STBY_YA | BIT_STBY_ZA)
#define BIT_STBY_XYZG (BIT_STBY_XG | BIT_STBY_YG | BIT_STBY_ZG)
#define BIT_ACCL_FC_B (0x08)

const struct gyro_reg_s reg = {.who_am_i = 0x75,
                               .rate_div = 0x19,
                               .lpf = 0x1A,
                               .prod_id = 0x0C,
                               .user_ctrl = 0x6A,
                               .fifo_en = 0x23,
                               .gyro_cfg = 0x1B,
                               .accel_cfg = 0x1C,
                               .motion_thr = 0x1F,
                               .motion_dur = 0x20,
                               .fifo_count_h = 0x72,
                               .fifo_r_w = 0x74,
                               .raw_gyro = 0x43,
                               .raw_accel = 0x3B,
                               .temp = 0x41,
                               .int_enable = 0x38,
                               .dmp_int_status = 0x39,
                               .int_status = 0x3A,
                               .pwr_mgmt_1 = 0x6B,
                               .pwr_mgmt_2 = 0x6C,
                               .int_pin_cfg = 0x37,
                               .mem_r_w = 0x6F,
                               .accel_offs = 0x06,
                               .i2c_mst = 0x24,
                               .bank_sel = 0x6D,
                               .mem_start_addr = 0x6E,
                               .prgm_start_h = 0x70
};
const struct hw_s hw = {.addr = 0x68,
                        .max_fifo = 1024,
                        .num_reg = 118,
                        .temp_sens = 340,
                        .temp_offset = -521,
                        .bank_size = 256
};

static struct gyro_state_s st = {.reg = &reg, .hw = &hw};

#define MAX_PACKET_LENGTH (12)

/**
 *  @brief      Enable/disable data ready interrupt.
 *  If the DMP is on, the DMP interrupt is enabled. Otherwise, the data ready
 *  interrupt is used.
 *  @param[in]  enable      1 to enable interrupt.
 *  @return     0 if successful.
 */
static int8_t set_int_enable(unsigned char enable) {
  unsigned char tmp;

  if (st.chip_cfg.dmp_on) {
    if (enable)
      tmp = BIT_DMP_INT_EN;
    else
      tmp = 0x00;
    if (i2c_write(st.hw->addr, st.reg->int_enable, 1, &tmp))
      return -1;
    st.chip_cfg.int_enable = tmp;
  } else {
    if (!st.chip_cfg.sensors)
      return -1;
    if (enable && st.chip_cfg.int_enable)
      return 0;
    if (enable)
      tmp = BIT_DATA_RDY_EN;
    else
      tmp = 0x00;
    if (i2c_write(st.hw->addr, st.reg->int_enable, 1, &tmp))
      return -1;
    st.chip_cfg.int_enable = tmp;
  }
  return 0;
}

/**
 *  @brief      Register dump for testing.
 *  @return     0 if successful.
 */
int8_t mpu_reg_dump(void) {
  unsigned char ii;
  unsigned char data;

  for (ii = 0; ii < st.hw->num_reg; ii++) {
    if (ii == st.reg->fifo_r_w || ii == st.reg->mem_r_w)
      continue;
    if (i2c_read(st.hw->addr, ii, 1, &data))
      return -1;
  }
  return 0;
}

/**
 *  @brief      Read from a single register.
 *  NOTE: The memory and FIFO read/write registers cannot be accessed.
 *  @param[in]  reg     Register address.
 *  @param[out] data    Register data.
 *  @return     0 if successful.
 */
int8_t mpu_read_reg(unsigned char reg, unsigned char *data) {
  if (reg == st.reg->fifo_r_w || reg == st.reg->mem_r_w)
    return -1;
  if (reg >= st.hw->num_reg)
    return -1;
  return i2c_read(st.hw->addr, reg, 1, data);
}

/**
 *  @brief      Initialize hardware.
 *  Initial configuration:\n
 *  Gyro FSR: +/- 2000DPS\n
 *  Accel FSR +/- 2G\n
 *  DLPF: 42Hz\n
 *  FIFO rate: 50Hz\n
 *  Clock source: Gyro PLL\n
 *  FIFO: Disabled.\n
 *  Data ready interrupt: Disabled, active low, unlatched.
 *  @param[in]  int_param   Platform-specific parameters to interrupt API.
 *  @return     0 if successful.
 */
int8_t mpu_init(struct int_param_s *int_param) {
  unsigned char data[6];

  /* Reset device. */
  data[0] = BIT_RESET;
  if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_1, 1, data))
    return -1;
  delay_ms(500);

  /* Wake up chip. */
  data[0] = 0x00;
  if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_1, 1, data))
    return -2;

  st.chip_cfg.accel_half = 0;

#ifdef MPU6500
  /* MPU6500 shares 4kB of memory between the DMP and the FIFO. Since the
   * first 3kB are needed by the DMP, we'll use the last 1kB for the FIFO.
   */
  data[0] = BIT_FIFO_SIZE_1024 | 0x8;
  if (i2c_write(st.hw->addr, st.reg->accel_cfg2, 1, data))
    return -2;
#endif

  /* Set to invalid values to ensure no I2C writes are skipped. */
  st.chip_cfg.sensors = 0xFF;
  st.chip_cfg.gyro_fsr = 0xFF;
  st.chip_cfg.accel_fsr = 0xFF;
  st.chip_cfg.lpf = 0xFF;
  st.chip_cfg.sample_rate = 0xFFFF;
  st.chip_cfg.fifo_enable = 0xFF;
  st.chip_cfg.bypass_mode = 0xFF;
#ifdef AK89xx_SECONDARY
  st.chip_cfg.compass_sample_rate = 0xFFFF;
#endif
  /* mpu_set_sensors always preserves this setting. */
  st.chip_cfg.clk_src = INV_CLK_PLL;
  /* Handled in next call to mpu_set_bypass. */
  st.chip_cfg.active_low_int = 1;
  st.chip_cfg.latched_int = 0;
  st.chip_cfg.int_motion_only = 0;
  st.chip_cfg.lp_accel_mode = 0;
  memset(&st.chip_cfg.cache, 0, sizeof(st.chip_cfg.cache));
  st.chip_cfg.dmp_on = 0;
  st.chip_cfg.dmp_loaded = 0;
  st.chip_cfg.dmp_sample_rate = 0;

  if (mpu_set_gyro_fsr(2000))
    return -30;
  if (mpu_set_accel_fsr(2))
    return -31;
  if (mpu_set_lpf(42))
    return -32;
  if (mpu_set_sample_rate(50))
    return -33;
  if (mpu_configure_fifo(0))
    return -34;

#ifdef AK89xx_SECONDARY
  setup_compass();
  if (mpu_set_compass_sample_rate(10))
    return -4;
#else
  /* Already disabled by setup_compass. */
  if (mpu_set_bypass(0))
    return -4;
#endif

  mpu_set_sensors(0);
  return 0;
}

/**
 *  @brief      Enter low-power accel-only mode.
 *  In low-power accel mode, the chip goes to sleep and only wakes up to sample
 *  the accelerometer at one of the following frequencies:
 *  \n MPU6050: 1.25Hz, 5Hz, 20Hz, 40Hz
 *  \n MPU6500: 1.25Hz, 2.5Hz, 5Hz, 10Hz, 20Hz, 40Hz, 80Hz, 160Hz, 320Hz, 640Hz
 *  \n If the requested rate is not one listed above, the device will be set to
 *  the next highest rate. Requesting a rate above the maximum supported
 *  frequency will result in an error.
 *  \n To select a fractional wake-up frequency, round down the value passed to
 *  @e rate.
 *  @param[in]  rate        Minimum sampling rate, or zero to disable LP
 *                          accel mode.
 *  @return     0 if successful.
 */
int8_t mpu_lp_accel_mode(unsigned char rate) {
  unsigned char tmp[2];

#if defined MPU6500
  unsigned char data;
#endif
  if (!rate) {
    mpu_set_int_latched(0);
    tmp[0] = 0;
    tmp[1] = BIT_STBY_XYZG;
    if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_1, 2, tmp))
      return -1;
    st.chip_cfg.lp_accel_mode = 0;
    return 0;
  }
  /* For LP accel, we automatically configure the hardware to produce latched
   * interrupts. In LP accel mode, the hardware cycles into sleep mode before
   * it gets a chance to deassert the interrupt pin; therefore, we shift this
   * responsibility over to the MCU.
   *
   * Any register read will clear the interrupt.
   */
  mpu_set_int_latched(1);
#if defined MPU6050
  tmp[0] = BIT_LPA_CYCLE;
  if (rate == 1) {
    tmp[1] = INV_LPA_1_25HZ;
    mpu_set_lpf(5);
  } else if (rate <= 5) {
    tmp[1] = INV_LPA_5HZ;
    mpu_set_lpf(5);
  } else if (rate <= 20) {
    tmp[1] = INV_LPA_20HZ;
    mpu_set_lpf(10);
  } else {
    tmp[1] = INV_LPA_40HZ;
    mpu_set_lpf(20);
  }
  tmp[1] = (tmp[1] << 6) | BIT_STBY_XYZG;
  if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_1, 2, tmp))
    return -1;
#elif defined MPU6500
  /* Set wake frequency. */
  if (rate == 1)
    tmp[0] = INV_LPA_1_25HZ;
  else if (rate == 2)
    tmp[0] = INV_LPA_2_5HZ;
  else if (rate <= 5)
    tmp[0] = INV_LPA_5HZ;
  else if (rate <= 10)
    tmp[0] = INV_LPA_10HZ;
  else if (rate <= 20)
    tmp[0] = INV_LPA_20HZ;
  else if (rate <= 40)
    tmp[0] = INV_LPA_40HZ;
  else if (rate <= 80)
    tmp[0] = INV_LPA_80HZ;
  else if (rate <= 160)
    tmp[0] = INV_LPA_160HZ;
  else if (rate <= 320)
    tmp[0] = INV_LPA_320HZ;
  else
    tmp[0] = INV_LPA_640HZ;
  if (i2c_write(st.hw->addr, st.reg->lp_accel_odr, 1, tmp))
    return -1;
  tmp[0] = BIT_LPA_CYCLE;
  if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_1, 1, tmp))
    return -1;
#endif
  st.chip_cfg.sensors = INV_XYZ_ACCEL;
  st.chip_cfg.clk_src = 0;
  st.chip_cfg.lp_accel_mode = 1;
  mpu_configure_fifo(0);

  return 0;
}

/**
 *  @brief      Read raw gyro data directly from the registers.
 *  @param[out] data        Raw data in hardware units.
 *  @param[out] timestamp   Timestamp in milliseconds. Null if not needed.
 *  @return     0 if successful.
 */
int8_t mpu_get_gyro_reg(short *data) {
  unsigned char tmp[6];

  if (!(st.chip_cfg.sensors & INV_XYZ_GYRO))
    return -1;

  if (i2c_read(st.hw->addr, st.reg->raw_gyro, 6, tmp))
    return -1;
  data[0] = (tmp[0] << 8) | tmp[1];
  data[1] = (tmp[2] << 8) | tmp[3];
  data[2] = (tmp[4] << 8) | tmp[5];
  return 0;
}

/**
 *  @brief      Read raw accel data directly from the registers.
 *  @param[out] data        Raw data in hardware units.
 *  @param[out] timestamp   Timestamp in milliseconds. Null if not needed.
 *  @return     0 if successful.
 */
int8_t mpu_get_accel_reg(short *data) {
  unsigned char tmp[6];

  if (!(st.chip_cfg.sensors & INV_XYZ_ACCEL))
    return -1;

  if (i2c_read(st.hw->addr, st.reg->raw_accel, 6, tmp))
    return -1;
  data[0] = (tmp[0] << 8) | tmp[1];
  data[1] = (tmp[2] << 8) | tmp[3];
  data[2] = (tmp[4] << 8) | tmp[5];
  return 0;
}

/**
 *  @brief      Read temperature data directly from the registers.
 *  @param[out] data        Data in q16 format.
 *  @param[out] timestamp   Timestamp in milliseconds. Null if not needed.
 *  @return     0 if successful.
 */
int8_t mpu_get_temperature(long *data) {
  unsigned char tmp[2];
  short raw;

  if (!(st.chip_cfg.sensors))
    return -1;

  if (i2c_read(st.hw->addr, st.reg->temp, 2, tmp))
    return -1;
  raw = (tmp[0] << 8) | tmp[1];

  data[0] =
      (long)((35 + ((raw - (float)st.hw->temp_offset) / st.hw->temp_sens)) *
             65536L);
  return 0;
}

/**
 *  @brief      Read biases to the accel bias 6500 registers.
 *  This function reads from the MPU6500 accel offset cancellations registers.
 *  The format are G in +-8G format. The register is initialized with OTP
 *  factory trim values.
 *  @param[in]  accel_bias  returned structure with the accel bias
 *  @return     0 if successful.
 */
int8_t mpu_read_6500_accel_bias(long *accel_bias) {
  unsigned char data[6];
  if (i2c_read(st.hw->addr, 0x77, 2, &data[0]))
    return -1;
  if (i2c_read(st.hw->addr, 0x7A, 2, &data[2]))
    return -1;
  if (i2c_read(st.hw->addr, 0x7D, 2, &data[4]))
    return -1;
  accel_bias[0] = ((long)data[0] << 8) | data[1];
  accel_bias[1] = ((long)data[2] << 8) | data[3];
  accel_bias[2] = ((long)data[4] << 8) | data[5];
  return 0;
}

/**
 *  @brief      Read biases to the accel bias 6050 registers.
 *  This function reads from the MPU6050 accel offset cancellations registers.
 *  The format are G in +-8G format. The register is initialized with OTP
 *  factory trim values.
 *  @param[in]  accel_bias  returned structure with the accel bias
 *  @return     0 if successful.
 */
int8_t mpu_read_6050_accel_bias(long *accel_bias) {
  unsigned char data[6];
  if (i2c_read(st.hw->addr, 0x06, 2, &data[0]))
    return -1;
  if (i2c_read(st.hw->addr, 0x08, 2, &data[2]))
    return -1;
  if (i2c_read(st.hw->addr, 0x0A, 2, &data[4]))
    return -1;
  accel_bias[0] = ((long)data[0] << 8) | data[1];
  accel_bias[1] = ((long)data[2] << 8) | data[3];
  accel_bias[2] = ((long)data[4] << 8) | data[5];
  return 0;
}

int8_t mpu_read_gyro_bias(long *gyro_bias) {
  unsigned char data[6];
  if (i2c_read(st.hw->addr, 0x13, 2, &data[0]))
    return -1;
  if (i2c_read(st.hw->addr, 0x15, 2, &data[2]))
    return -1;
  if (i2c_read(st.hw->addr, 0x17, 2, &data[4]))
    return -1;
  gyro_bias[0] = ((long)data[0] << 8) | data[1];
  gyro_bias[1] = ((long)data[2] << 8) | data[3];
  gyro_bias[2] = ((long)data[4] << 8) | data[5];
  return 0;
}

/**
 *  @brief      Push biases to the gyro bias 6500/6050 registers.
 *  This function expects biases relative to the current sensor output, and
 *  these biases will be added to the factory-supplied values. Bias inputs are
 * LSB in +-1000dps format.
 *  @param[in]  gyro_bias  New biases.
 *  @return     0 if successful.
 */
int8_t mpu_set_gyro_bias_reg(long *gyro_bias) {
  unsigned char data[6] = {0, 0, 0, 0, 0, 0};

  long gyro_reg_bias[3] = {0, 0, 0};
  int8_t i = 0;
  if (mpu_read_gyro_bias(gyro_reg_bias))
    return -1;
  for (i = 0; i < 3; i++) {
    gyro_reg_bias[i] -= gyro_bias[i];
  }
  data[0] = (gyro_reg_bias[0] >> 8) & 0xff;
  data[1] = (gyro_reg_bias[0]) & 0xff;
  data[2] = (gyro_reg_bias[1] >> 8) & 0xff;
  data[3] = (gyro_reg_bias[1]) & 0xff;
  data[4] = (gyro_reg_bias[2] >> 8) & 0xff;
  data[5] = (gyro_reg_bias[2]) & 0xff;
  if (i2c_write(st.hw->addr, 0x13, 2, &data[0]))
    return -1;
  if (i2c_write(st.hw->addr, 0x15, 2, &data[2]))
    return -1;
  if (i2c_write(st.hw->addr, 0x17, 2, &data[4]))
    return -1;
  return 0;
}

/**
 *  @brief      Push biases to the accel bias 6050 registers.
 *  This function expects biases relative to the current sensor output, and
 *  these biases will be added to the factory-supplied values. Bias inputs are
 * LSB in +-8G format.
 *  @param[in]  accel_bias  New biases.
 *  @return     0 if successful.
 */
int8_t mpu_set_accel_bias_6050_reg(const long *accel_bias) {
  unsigned char data[6] = {0, 0, 0, 0, 0, 0};
  long accel_reg_bias[3] = {0, 0, 0};

  if (mpu_read_6050_accel_bias(accel_reg_bias))
    return -1;

  accel_reg_bias[0] -= (accel_bias[0] & ~1);
  accel_reg_bias[1] -= (accel_bias[1] & ~1);
  accel_reg_bias[2] -= (accel_bias[2] & ~1);

  data[0] = (accel_reg_bias[0] >> 8) & 0xff;
  data[1] = (accel_reg_bias[0]) & 0xff;

  data[2] = (accel_reg_bias[1] >> 8) & 0xff;
  data[3] = (accel_reg_bias[1]) & 0xff;

  data[4] = (accel_reg_bias[2] >> 8) & 0xff;
  data[5] = (accel_reg_bias[2]) & 0xff;

  if (i2c_write(st.hw->addr, 0x06, 2, &data[0]))
    return -1;
  if (i2c_write(st.hw->addr, 0x08, 2, &data[2]))
    return -1;
  if (i2c_write(st.hw->addr, 0x0A, 2, &data[4]))
    return -1;

  return 0;
}

/**
 *  @brief      Push biases to the accel bias 6500 registers.
 *  This function expects biases relative to the current sensor output, and
 *  these biases will be added to the factory-supplied values. Bias inputs are
 * LSB in +-8G format.
 *  @param[in]  accel_bias  New biases.
 *  @return     0 if successful.
 */
int8_t mpu_set_accel_bias_6500_reg(const long *accel_bias) {

  unsigned char data[6] = {0, 0, 0, 0, 0, 0};
  long accel_reg_bias[3] = {0, 0, 0};

  if (mpu_read_6500_accel_bias(accel_reg_bias))
    return -1;

  // Preserve bit 0 of factory value (for temperature compensation)
  accel_reg_bias[0] -= (accel_bias[0] & ~1);
  accel_reg_bias[1] -= (accel_bias[1] & ~1);
  accel_reg_bias[2] -= (accel_bias[2] & ~1);

  data[0] = (accel_reg_bias[0] >> 8) & 0xff;
  data[1] = (accel_reg_bias[0]) & 0xff;

  data[2] = (accel_reg_bias[1] >> 8) & 0xff;
  data[3] = (accel_reg_bias[1]) & 0xff;

  data[4] = (accel_reg_bias[2] >> 8) & 0xff;
  data[5] = (accel_reg_bias[2]) & 0xff;

  if (i2c_write(st.hw->addr, 0x77, 2, &data[0]))
    return -1;
  if (i2c_write(st.hw->addr, 0x7A, 2, &data[2]))
    return -1;
  if (i2c_write(st.hw->addr, 0x7D, 2, &data[4]))
    return -1;

  return 0;
}

/**
 *  @brief  Reset FIFO read/write pointers.
 *  @return 0 if successful.
 */
int8_t mpu_reset_fifo(void) {
  unsigned char data;

  if (!(st.chip_cfg.sensors))
    return -1;

  data = 0;
  if (i2c_write(st.hw->addr, st.reg->int_enable, 1, &data))
    return -1;
  if (i2c_write(st.hw->addr, st.reg->fifo_en, 1, &data))
    return -1;
  if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &data))
    return -1;

  if (st.chip_cfg.dmp_on) {
    data = BIT_FIFO_RST | BIT_DMP_RST;
    if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &data))
      return -1;
    delay_ms(50);
    data = BIT_DMP_EN | BIT_FIFO_EN;
    if (st.chip_cfg.sensors & INV_XYZ_COMPASS)
      data |= BIT_AUX_IF_EN;
    if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &data))
      return -1;
    if (st.chip_cfg.int_enable)
      data = BIT_DMP_INT_EN;
    else
      data = 0;
    if (i2c_write(st.hw->addr, st.reg->int_enable, 1, &data))
      return -1;
    data = 0;
    if (i2c_write(st.hw->addr, st.reg->fifo_en, 1, &data))
      return -1;
  } else {
    data = BIT_FIFO_RST;
    if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &data))
      return -1;
    if (st.chip_cfg.bypass_mode || !(st.chip_cfg.sensors & INV_XYZ_COMPASS))
      data = BIT_FIFO_EN;
    else
      data = BIT_FIFO_EN | BIT_AUX_IF_EN;
    if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &data))
      return -1;
    delay_ms(50);
    if (st.chip_cfg.int_enable)
      data = BIT_DATA_RDY_EN;
    else
      data = 0;
    if (i2c_write(st.hw->addr, st.reg->int_enable, 1, &data))
      return -1;
    if (i2c_write(st.hw->addr, st.reg->fifo_en, 1, &st.chip_cfg.fifo_enable))
      return -1;
  }
  return 0;
}

/**
 *  @brief      Get the gyro full-scale range.
 *  @param[out] fsr Current full-scale range.
 *  @return     0 if successful.
 */
int8_t mpu_get_gyro_fsr(unsigned short *fsr) {
  switch (st.chip_cfg.gyro_fsr) {
  case INV_FSR_250DPS:
    fsr[0] = 250;
    break;
  case INV_FSR_500DPS:
    fsr[0] = 500;
    break;
  case INV_FSR_1000DPS:
    fsr[0] = 1000;
    break;
  case INV_FSR_2000DPS:
    fsr[0] = 2000;
    break;
  default:
    fsr[0] = 0;
    break;
  }
  return 0;
}

/**
 *  @brief      Set the gyro full-scale range.
 *  @param[in]  fsr Desired full-scale range.
 *  @return     0 if successful.
 */
int8_t mpu_set_gyro_fsr(unsigned short fsr) {
  unsigned char data;

  if (!(st.chip_cfg.sensors))
    return -1;

  switch (fsr) {
  case 250:
    data = INV_FSR_250DPS << 3;
    break;
  case 500:
    data = INV_FSR_500DPS << 3;
    break;
  case 1000:
    data = INV_FSR_1000DPS << 3;
    break;
  case 2000:
    data = INV_FSR_2000DPS << 3;
    break;
  default:
    return -1;
  }

  if (st.chip_cfg.gyro_fsr == (data >> 3))
    return 0;
  if (i2c_write(st.hw->addr, st.reg->gyro_cfg, 1, &data))
    return -1;
  st.chip_cfg.gyro_fsr = data >> 3;
  return 0;
}

/**
 *  @brief      Get the accel full-scale range.
 *  @param[out] fsr Current full-scale range.
 *  @return     0 if successful.
 */
int8_t mpu_get_accel_fsr(unsigned char *fsr) {
  switch (st.chip_cfg.accel_fsr) {
  case INV_FSR_2G:
    fsr[0] = 2;
    break;
  case INV_FSR_4G:
    fsr[0] = 4;
    break;
  case INV_FSR_8G:
    fsr[0] = 8;
    break;
  case INV_FSR_16G:
    fsr[0] = 16;
    break;
  default:
    return -1;
  }
  if (st.chip_cfg.accel_half)
    fsr[0] <<= 1;
  return 0;
}

/**
 *  @brief      Set the accel full-scale range.
 *  @param[in]  fsr Desired full-scale range.
 *  @return     0 if successful.
 */
int8_t mpu_set_accel_fsr(unsigned char fsr) {
  unsigned char data;

  if (!(st.chip_cfg.sensors))
    return -1;

  switch (fsr) {
  case 2:
    data = INV_FSR_2G << 3;
    break;
  case 4:
    data = INV_FSR_4G << 3;
    break;
  case 8:
    data = INV_FSR_8G << 3;
    break;
  case 16:
    data = INV_FSR_16G << 3;
    break;
  default:
    return -1;
  }

  if (st.chip_cfg.accel_fsr == (data >> 3))
    return 0;
  if (i2c_write(st.hw->addr, st.reg->accel_cfg, 1, &data))
    return -1;
  st.chip_cfg.accel_fsr = data >> 3;
  return 0;
}

/**
 *  @brief      Get the current DLPF setting.
 *  @param[out] lpf Current LPF setting.
 *  0 if successful.
 */
int8_t mpu_get_lpf(unsigned short *lpf) {
  switch (st.chip_cfg.lpf) {
  case INV_FILTER_188HZ:
    lpf[0] = 188;
    break;
  case INV_FILTER_98HZ:
    lpf[0] = 98;
    break;
  case INV_FILTER_42HZ:
    lpf[0] = 42;
    break;
  case INV_FILTER_20HZ:
    lpf[0] = 20;
    break;
  case INV_FILTER_10HZ:
    lpf[0] = 10;
    break;
  case INV_FILTER_5HZ:
    lpf[0] = 5;
    break;
  case INV_FILTER_256HZ_NOLPF2:
  case INV_FILTER_2100HZ_NOLPF:
  default:
    lpf[0] = 0;
    break;
  }
  return 0;
}

/**
 *  @brief      Set digital low pass filter.
 *  The following LPF settings are supported: 188, 98, 42, 20, 10, 5.
 *  @param[in]  lpf Desired LPF setting.
 *  @return     0 if successful.
 */
int8_t mpu_set_lpf(unsigned short lpf) {
  unsigned char data;

  if (!(st.chip_cfg.sensors))
    return -1;

  if (lpf >= 188)
    data = INV_FILTER_188HZ;
  else if (lpf >= 98)
    data = INV_FILTER_98HZ;
  else if (lpf >= 42)
    data = INV_FILTER_42HZ;
  else if (lpf >= 20)
    data = INV_FILTER_20HZ;
  else if (lpf >= 10)
    data = INV_FILTER_10HZ;
  else
    data = INV_FILTER_5HZ;

  if (st.chip_cfg.lpf == data)
    return 0;
  if (i2c_write(st.hw->addr, st.reg->lpf, 1, &data))
    return -1;
  st.chip_cfg.lpf = data;
  return 0;
}

/**
 *  @brief      Get sampling rate.
 *  @param[out] rate    Current sampling rate (Hz).
 *  @return     0 if successful.
 */
int8_t mpu_get_sample_rate(unsigned short *rate) {
  if (st.chip_cfg.dmp_on)
    return -1;
  else
    rate[0] = st.chip_cfg.sample_rate;
  return 0;
}

/**
 *  @brief      Set sampling rate.
 *  Sampling rate must be between 4Hz and 1kHz.
 *  @param[in]  rate    Desired sampling rate (Hz).
 *  @return     0 if successful.
 */
int8_t mpu_set_sample_rate(unsigned short rate) {
  unsigned char data;

  if (!(st.chip_cfg.sensors))
    return -1;

  if (st.chip_cfg.dmp_on)
    return -1;
  else {
    if (st.chip_cfg.lp_accel_mode) {
      if (rate && (rate <= 40)) {
        /* Just stay in low-power accel mode. */
        mpu_lp_accel_mode(rate);
        return 0;
      }
      /* Requested rate exceeds the allowed frequencies in LP accel mode,
       * switch back to full-power mode.
       */
      mpu_lp_accel_mode(0);
    }
    if (rate < 4)
      rate = 4;
    else if (rate > 1000)
      rate = 1000;

    data = 1000 / rate - 1;
    if (i2c_write(st.hw->addr, st.reg->rate_div, 1, &data))
      return -1;

    st.chip_cfg.sample_rate = 1000 / (1 + data);

#ifdef AK89xx_SECONDARY
    mpu_set_compass_sample_rate(
        min(st.chip_cfg.compass_sample_rate, MAX_COMPASS_SAMPLE_RATE));
#endif

    /* Automatically set LPF to 1/2 sampling rate. */
    mpu_set_lpf(st.chip_cfg.sample_rate >> 1);
    return 0;
  }
}

/**
 *  @brief      Get compass sampling rate.
 *  @param[out] rate    Current compass sampling rate (Hz).
 *  @return     0 if successful.
 */
int8_t mpu_get_compass_sample_rate(unsigned short *rate) {
#ifdef AK89xx_SECONDARY
  rate[0] = st.chip_cfg.compass_sample_rate;
  return 0;
#else
  rate[0] = 0;
  return -1;
#endif
}

/**
 *  @brief      Set compass sampling rate.
 *  The compass on the auxiliary I2C bus is read by the MPU hardware at a
 *  maximum of 100Hz. The actual rate can be set to a fraction of the gyro
 *  sampling rate.
 *
 *  \n WARNING: The new rate may be different than what was requested. Call
 *  mpu_get_compass_sample_rate to check the actual setting.
 *  @param[in]  rate    Desired compass sampling rate (Hz).
 *  @return     0 if successful.
 */
int8_t mpu_set_compass_sample_rate(unsigned short rate) {
#ifdef AK89xx_SECONDARY
  unsigned char div;
  if (!rate || rate > st.chip_cfg.sample_rate || rate > MAX_COMPASS_SAMPLE_RATE)
    return -1;

  div = st.chip_cfg.sample_rate / rate - 1;
  if (i2c_write(st.hw->addr, st.reg->s4_ctrl, 1, &div))
    return -1;
  st.chip_cfg.compass_sample_rate = st.chip_cfg.sample_rate / (div + 1);
  return 0;
#else
  return -1;
#endif
}

/**
 *  @brief      Get gyro sensitivity scale factor.
 *  @param[out] sens    Conversion from hardware units to dps.
 *  @return     0 if successful.
 */
int8_t mpu_get_gyro_sens(float *sens) {
  switch (st.chip_cfg.gyro_fsr) {
  case INV_FSR_250DPS:
    sens[0] = 131.f;
    break;
  case INV_FSR_500DPS:
    sens[0] = 65.5f;
    break;
  case INV_FSR_1000DPS:
    sens[0] = 32.8f;
    break;
  case INV_FSR_2000DPS:
    sens[0] = 16.4f;
    break;
  default:
    return -1;
  }
  return 0;
}

/**
 *  @brief      Get accel sensitivity scale factor.
 *  @param[out] sens    Conversion from hardware units to g's.
 *  @return     0 if successful.
 */
int8_t mpu_get_accel_sens(unsigned short *sens) {
  switch (st.chip_cfg.accel_fsr) {
  case INV_FSR_2G:
    sens[0] = 16384;
    break;
  case INV_FSR_4G:
    sens[0] = 8092;
    break;
  case INV_FSR_8G:
    sens[0] = 4096;
    break;
  case INV_FSR_16G:
    sens[0] = 2048;
    break;
  default:
    return -1;
  }
  if (st.chip_cfg.accel_half)
    sens[0] >>= 1;
  return 0;
}

/**
 *  @brief      Get current FIFO configuration.
 *  @e sensors can contain a combination of the following flags:
 *  \n INV_X_GYRO, INV_Y_GYRO, INV_Z_GYRO
 *  \n INV_XYZ_GYRO
 *  \n INV_XYZ_ACCEL
 *  @param[out] sensors Mask of sensors in FIFO.
 *  @return     0 if successful.
 */
int8_t mpu_get_fifo_config(unsigned char *sensors) {
  sensors[0] = st.chip_cfg.fifo_enable;
  return 0;
}

/**
 *  @brief      Select which sensors are pushed to FIFO.
 *  @e sensors can contain a combination of the following flags:
 *  \n INV_X_GYRO, INV_Y_GYRO, INV_Z_GYRO
 *  \n INV_XYZ_GYRO
 *  \n INV_XYZ_ACCEL
 *  @param[in]  sensors Mask of sensors to push to FIFO.
 *  @return     0 if successful.
 */
int8_t mpu_configure_fifo(unsigned char sensors) {
  unsigned char prev;
  int8_t result = 0;

  /* Compass data isn't going into the FIFO. Stop trying. */
  sensors &= ~INV_XYZ_COMPASS;

  if (st.chip_cfg.dmp_on)
    return 0;
  else {
    if (!(st.chip_cfg.sensors))
      return -1;
    prev = st.chip_cfg.fifo_enable;
    st.chip_cfg.fifo_enable = sensors & st.chip_cfg.sensors;
    if (st.chip_cfg.fifo_enable != sensors)
      /* You're not getting what you asked for. Some sensors are
       * asleep.
       */
      result = -1;
    else
      result = 0;
    if (sensors || st.chip_cfg.lp_accel_mode)
      set_int_enable(1);
    else
      set_int_enable(0);
    if (sensors) {
      if (mpu_reset_fifo()) {
        st.chip_cfg.fifo_enable = prev;
        return -1;
      }
    }
  }

  return result;
}

/**
 *  @brief      Get current power state.
 *  @return     1 if turned on, 0 if suspended.
 */
int8_t mpu_get_power_state(unsigned char *power_on) {
  return st.chip_cfg.sensors;
}

/**
 *  @brief      Turn specific sensors on/off.
 *  @e sensors can contain a combination of the following flags:
 *  \n INV_X_GYRO, INV_Y_GYRO, INV_Z_GYRO
 *  \n INV_XYZ_GYRO
 *  \n INV_XYZ_ACCEL
 *  \n INV_XYZ_COMPASS
 *  @param[in]  sensors    Mask of sensors to wake.
 *  @return     0 if successful.
 */
int8_t mpu_set_sensors(unsigned char sensors) {
  unsigned char data;
#ifdef AK89xx_SECONDARY
  unsigned char user_ctrl;
#endif

  if (sensors & INV_XYZ_GYRO)
    data = INV_CLK_PLL;
  else if (sensors)
    data = 0;
  else
    data = BIT_SLEEP;
  if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_1, 1, &data)) {
    st.chip_cfg.sensors = 0;
    return -1;
  }
  st.chip_cfg.clk_src = data & ~BIT_SLEEP;

  data = 0;
  if (!(sensors & INV_X_GYRO))
    data |= BIT_STBY_XG;
  if (!(sensors & INV_Y_GYRO))
    data |= BIT_STBY_YG;
  if (!(sensors & INV_Z_GYRO))
    data |= BIT_STBY_ZG;
  if (!(sensors & INV_XYZ_ACCEL))
    data |= BIT_STBY_XYZA;
  if (i2c_write(st.hw->addr, st.reg->pwr_mgmt_2, 1, &data)) {
    st.chip_cfg.sensors = 0;
    return -1;
  }

  if (sensors && (sensors != INV_XYZ_ACCEL))
    /* Latched interrupts only used in LP accel mode. */
    mpu_set_int_latched(0);


  st.chip_cfg.sensors = sensors;
  st.chip_cfg.lp_accel_mode = 0;
  delay_ms(50);
  return 0;
}

/**
 *  @brief      Read the MPU interrupt status registers.
 *  @param[out] status  Mask of interrupt bits.
 *  @return     0 if successful.
 */
int8_t mpu_get_int_status(short *status) {
  unsigned char tmp[2];
  if (!st.chip_cfg.sensors)
    return -1;
  if (i2c_read(st.hw->addr, st.reg->dmp_int_status, 2, tmp))
    return -1;
  status[0] = (tmp[0] << 8) | tmp[1];
  return 0;
}

/**
 *  @brief      Get one packet from the FIFO.
 *  If @e sensors does not contain a particular sensor, disregard the data
 *  returned to that pointer.
 *  \n @e sensors can contain a combination of the following flags:
 *  \n INV_X_GYRO, INV_Y_GYRO, INV_Z_GYRO
 *  \n INV_XYZ_GYRO
 *  \n INV_XYZ_ACCEL
 *  \n If the FIFO has no new data, @e sensors will be zero.
 *  \n If the FIFO is disabled, @e sensors will be zero and this function will
 *  return a non-zero error code.
 *  @param[out] gyro        Gyro data in hardware units.
 *  @param[out] accel       Accel data in hardware units.
 *  @param[out] timestamp   Timestamp in milliseconds.
 *  @param[out] sensors     Mask of sensors read from FIFO.
 *  @param[out] more        Number of remaining packets.
 *  @return     0 if successful.
 */
int8_t mpu_read_fifo(short *gyro, short *accel, unsigned char *sensors,
                     unsigned char *more) {
  /* Assumes maximum packet size is gyro (6) + accel (6). */
  unsigned char data[MAX_PACKET_LENGTH];
  unsigned char packet_size = 0;
  unsigned short fifo_count, index = 0;

  if (st.chip_cfg.dmp_on)
    return -1;

  sensors[0] = 0;
  if (!st.chip_cfg.sensors)
    return -1;
  if (!st.chip_cfg.fifo_enable)
    return -1;

  if (st.chip_cfg.fifo_enable & INV_X_GYRO)
    packet_size += 2;
  if (st.chip_cfg.fifo_enable & INV_Y_GYRO)
    packet_size += 2;
  if (st.chip_cfg.fifo_enable & INV_Z_GYRO)
    packet_size += 2;
  if (st.chip_cfg.fifo_enable & INV_XYZ_ACCEL)
    packet_size += 6;

  if (i2c_read(st.hw->addr, st.reg->fifo_count_h, 2, data))
    return -1;
  fifo_count = (data[0] << 8) | data[1];
  if (fifo_count < packet_size)
    return 0;
  //
  if (fifo_count > (st.hw->max_fifo >> 1)) {
    /* FIFO is 50% full, better check overflow bit. */
    if (i2c_read(st.hw->addr, st.reg->int_status, 1, data))
      return -1;
    if (data[0] & BIT_FIFO_OVERFLOW) {
      mpu_reset_fifo();
      return -2;
    }
  }

  if (i2c_read(st.hw->addr, st.reg->fifo_r_w, packet_size, data))
    return -1;
  more[0] = fifo_count / packet_size - 1;
  sensors[0] = 0;

  if ((index != packet_size) && st.chip_cfg.fifo_enable & INV_XYZ_ACCEL) {
    accel[0] = (data[index + 0] << 8) | data[index + 1];
    accel[1] = (data[index + 2] << 8) | data[index + 3];
    accel[2] = (data[index + 4] << 8) | data[index + 5];
    sensors[0] |= INV_XYZ_ACCEL;
    index += 6;
  }
  if ((index != packet_size) && st.chip_cfg.fifo_enable & INV_X_GYRO) {
    gyro[0] = (data[index + 0] << 8) | data[index + 1];
    sensors[0] |= INV_X_GYRO;
    index += 2;
  }
  if ((index != packet_size) && st.chip_cfg.fifo_enable & INV_Y_GYRO) {
    gyro[1] = (data[index + 0] << 8) | data[index + 1];
    sensors[0] |= INV_Y_GYRO;
    index += 2;
  }
  if ((index != packet_size) && st.chip_cfg.fifo_enable & INV_Z_GYRO) {
    gyro[2] = (data[index + 0] << 8) | data[index + 1];
    sensors[0] |= INV_Z_GYRO;
    index += 2;
  }

  return 0;
}

/**
 *  @brief      Get one unparsed packet from the FIFO.
 *  This function should be used if the packet is to be parsed elsewhere.
 *  @param[in]  length  Length of one FIFO packet.
 *  @param[in]  data    FIFO packet.
 *  @param[in]  more    Number of remaining packets.
 */
int8_t mpu_read_fifo_stream(unsigned short length, unsigned char *data,
                            unsigned char *more) {
  unsigned char tmp[2];
  unsigned short fifo_count;
  if (!st.chip_cfg.dmp_on)
    return -1;
  if (!st.chip_cfg.sensors)
    return -2;

  if (i2c_read(st.hw->addr, st.reg->fifo_count_h, 2, tmp))
    return -3;
  fifo_count = (tmp[0] << 8) | tmp[1];
  if (fifo_count < length) {
    more[0] = 0;
    return -4;
  }
  if (fifo_count > (st.hw->max_fifo >> 1)) {
    /* FIFO is 50% full, better check overflow bit. */
    if (i2c_read(st.hw->addr, st.reg->int_status, 1, tmp))
      return -5;
    if (tmp[0] & BIT_FIFO_OVERFLOW) {
      mpu_reset_fifo();
      return -6;
    }
  }

  if (i2c_read(st.hw->addr, st.reg->fifo_r_w, length, data))
    return -7;
  more[0] = fifo_count / length - 1;
  return 0;
}

/**
 *  @brief      Set device to bypass mode.
 *  @param[in]  bypass_on   1 to enable bypass mode.
 *  @return     0 if successful.
 */
int8_t mpu_set_bypass(unsigned char bypass_on) {
  unsigned char tmp;

  if (st.chip_cfg.bypass_mode == bypass_on)
    return 0;

  if (bypass_on) {
    if (i2c_read(st.hw->addr, st.reg->user_ctrl, 1, &tmp))
      return -1;
    tmp &= ~BIT_AUX_IF_EN;
    if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &tmp))
      return -1;
    delay_ms(3);
    tmp = BIT_BYPASS_EN;
    if (st.chip_cfg.active_low_int)
      tmp |= BIT_ACTL;
    if (st.chip_cfg.latched_int)
      tmp |= BIT_LATCH_EN | BIT_ANY_RD_CLR;
    if (i2c_write(st.hw->addr, st.reg->int_pin_cfg, 1, &tmp))
      return -1;
  } else {
    /* Enable I2C master mode if compass is being used. */
    if (i2c_read(st.hw->addr, st.reg->user_ctrl, 1, &tmp))
      return -1;
    if (st.chip_cfg.sensors & INV_XYZ_COMPASS)
      tmp |= BIT_AUX_IF_EN;
    else
      tmp &= ~BIT_AUX_IF_EN;
    if (i2c_write(st.hw->addr, st.reg->user_ctrl, 1, &tmp))
      return -1;
    delay_ms(3);
    if (st.chip_cfg.active_low_int)
      tmp = BIT_ACTL;
    else
      tmp = 0;
    if (st.chip_cfg.latched_int)
      tmp |= BIT_LATCH_EN | BIT_ANY_RD_CLR;
    if (i2c_write(st.hw->addr, st.reg->int_pin_cfg, 1, &tmp))
      return -1;
  }
  st.chip_cfg.bypass_mode = bypass_on;
  return 0;
}

/**
 *  @brief      Set interrupt level.
 *  @param[in]  active_low  1 for active low, 0 for active high.
 *  @return     0 if successful.
 */
int8_t mpu_set_int_level(unsigned char active_low) {
  st.chip_cfg.active_low_int = active_low;
  return 0;
}

/**
 *  @brief      Enable latched interrupts.
 *  Any MPU register will clear the interrupt.
 *  @param[in]  enable  1 to enable, 0 to disable.
 *  @return     0 if successful.
 */
int8_t mpu_set_int_latched(unsigned char enable) {
  unsigned char tmp;
  if (st.chip_cfg.latched_int == enable)
    return 0;

  if (enable)
    tmp = BIT_LATCH_EN | BIT_ANY_RD_CLR;
  else
    tmp = 0;
  if (st.chip_cfg.bypass_mode)
    tmp |= BIT_BYPASS_EN;
  if (st.chip_cfg.active_low_int)
    tmp |= BIT_ACTL;
  if (i2c_write(st.hw->addr, st.reg->int_pin_cfg, 1, &tmp))
    return -1;
  st.chip_cfg.latched_int = enable;
  return 0;
}

/**
 *  @brief      Write to the DMP memory.
 *  This function prevents I2C writes past the bank boundaries. The DMP memory
 *  is only accessible when the chip is awake.
 *  @param[in]  mem_addr    Memory location (bank << 8 | start address)
 *  @param[in]  length      Number of bytes to write.
 *  @param[in]  data        Bytes to write to memory.
 *  @return     0 if successful.
 */
int8_t mpu_write_mem(unsigned short mem_addr, unsigned short length,
                     unsigned char *data) {
  unsigned char tmp[2];

  if (!data)
    return -1;
  if (!st.chip_cfg.sensors)
    return -1;

  tmp[0] = (unsigned char)(mem_addr >> 8);
  tmp[1] = (unsigned char)(mem_addr & 0xFF);

  /* Check bank boundaries. */
  if (tmp[1] + length > st.hw->bank_size)
    return -1;

  if (i2c_write(st.hw->addr, st.reg->bank_sel, 2, tmp))
    return -1;
  if (i2c_write(st.hw->addr, st.reg->mem_r_w, length, data))
    return -1;
  return 0;
}

/**
 *  @brief      Read from the DMP memory.
 *  This function prevents I2C reads past the bank boundaries. The DMP memory
 *  is only accessible when the chip is awake.
 *  @param[in]  mem_addr    Memory location (bank << 8 | start address)
 *  @param[in]  length      Number of bytes to read.
 *  @param[out] data        Bytes read from memory.
 *  @return     0 if successful.
 */
int8_t mpu_read_mem(unsigned short mem_addr, unsigned short length,
                    unsigned char *data) {
  unsigned char tmp[2];

  if (!data)
    return -1;
  if (!st.chip_cfg.sensors)
    return -1;

  tmp[0] = (unsigned char)(mem_addr >> 8);
  tmp[1] = (unsigned char)(mem_addr & 0xFF);

  /* Check bank boundaries. */
  if (tmp[1] + length > st.hw->bank_size)
    return -1;

  if (i2c_write(st.hw->addr, st.reg->bank_sel, 2, tmp))
    return -1;
  if (i2c_read(st.hw->addr, st.reg->mem_r_w, length, data))
    return -1;
  return 0;
}

/**
 *  @brief      Load and verify DMP image.
 *  @param[in]  length      Length of DMP image.
 *  @param[in]  firmware    DMP code.
 *  @param[in]  start_addr  Starting address of DMP code memory.
 *  @param[in]  sample_rate Fixed sampling rate used when DMP is enabled.
 *  @return     0 if successful.
 */
int8_t mpu_load_firmware(unsigned short length, const unsigned char *firmware,
                         unsigned short start_addr,
                         unsigned short sample_rate) {
  unsigned short ii;
  unsigned short this_write;
  /* Must divide evenly into st.hw->bank_size to avoid bank crossings. */
#define LOAD_CHUNK (16)
  unsigned char cur[LOAD_CHUNK], tmp[2], firmware_chunk[LOAD_CHUNK];

  if (st.chip_cfg.dmp_loaded)
    /* DMP should only be loaded once. */
    return -1;

  if (!firmware)
    return -1;

  for (ii = 0; ii < length; ii += this_write) {
    this_write = min(LOAD_CHUNK, length - ii);
    
    for (int index = 0; index < this_write; index++)
      firmware_chunk[index] = pgm_read_byte(firmware + ii + index);

    if (mpu_write_mem(ii, this_write, firmware_chunk))
      return -2;
    if (mpu_read_mem(ii, this_write, cur))
      return -3;
    if (memcmp(firmware_chunk, cur, this_write))
      return -4;
  }

  /* Set program start address. */
  tmp[0] = start_addr >> 8;
  tmp[1] = start_addr & 0xFF;
  if (i2c_write(st.hw->addr, st.reg->prgm_start_h, 2, tmp))
    return -5;

  st.chip_cfg.dmp_loaded = 1;
  st.chip_cfg.dmp_sample_rate = sample_rate;
  return 0;
}

/**
 *  @brief      Enable/disable DMP support.
 *  @param[in]  enable  1 to turn on the DMP.
 *  @return     0 if successful.
 */
int8_t mpu_set_dmp_state(unsigned char enable) {
  unsigned char tmp;
  if (st.chip_cfg.dmp_on == enable)
    return 0;

  if (enable) {
    if (!st.chip_cfg.dmp_loaded)
      return -1;
    /* Disable data ready interrupt. */
    set_int_enable(0);
    /* Disable bypass mode. */
    mpu_set_bypass(0);
    /* Keep constant sample rate, FIFO rate controlled by DMP. */
    mpu_set_sample_rate(st.chip_cfg.dmp_sample_rate);
    /* Remove FIFO elements. */
    tmp = 0;
    if(i2c_write(st.hw->addr, 0x23, 1, &tmp)) {
      return -1;
    }
    st.chip_cfg.dmp_on = 1;
    /* Enable DMP interrupt. */
    set_int_enable(1);
    mpu_reset_fifo();
  } else {
    /* Disable DMP interrupt. */
    set_int_enable(0);
    /* Restore FIFO settings. */
    tmp = st.chip_cfg.fifo_enable;
    if(i2c_write(st.hw->addr, 0x23, 1, &tmp)) {
      return -1;
    }
    st.chip_cfg.dmp_on = 0;
    mpu_reset_fifo();
  }
  return 0;
}

/**
 *  @brief      Get DMP state.
 *  @param[out] enabled 1 if enabled.
 *  @return     0 if successful.
 */
int8_t mpu_get_dmp_state(unsigned char *enabled) {
  enabled[0] = st.chip_cfg.dmp_on;
  return 0;
}
