#ifndef IOTL_HEADER
#define IOTL_HEADER
#include <linux/types.h>

extern dev_t iotl_device;
void set_iotl_device(dev_t input);
#endif
