/* Compatibility shim for AT32 Workbench-generated includes.
 * Actual I2C master implementation and bugfixes are owned by BSP. */
#ifndef __I2C_APPLICATION_H
#define __I2C_APPLICATION_H
#include "bsp_i2c.h"
#define I2C_MEM_ADDR_WIDTH_8 I2C_MEM_ADDR_WIDIH_8
#define I2C_MEM_ADDR_WIDTH_16 I2C_MEM_ADDR_WIDIH_16
#endif
