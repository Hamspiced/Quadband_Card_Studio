#pragma once
#include "i2c.h"
struct i2c_rdwr_ioctl_data { i2c_msg* msgs; uint32_t nmsgs; };
#define I2C_RETRIES 0x0701
#define I2C_TIMEOUT 0x0702
#define I2C_FUNCS 0x0705
#define I2C_RDWR 0x0707
