/**
 * @file vision.h
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief 引导加载器版本号定义
 */

#ifndef __VISION_H__
#define __VISION_H__

#define BOOT_BUILD_DATE __DATE__
#define BOOT_BUILD_TIME __TIME__

#define BOOT_VERSION_MAJOR 0
#define BOOT_VERSION_MINOR 1
#define BOOT_VERSION_PATCH 0

#define BOOT_VERSION_INFO \
    "A simple STM32 H7 bootloader.\r\n" \
    "More information is available on GitHub:\r\n" \
    "https://github.com/Pcb-yun\r\n"



#endif /* __VISION_H__ */
