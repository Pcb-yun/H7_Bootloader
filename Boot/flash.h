/**
 * @file flash.h
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief Flash 编程头文件
 */

#ifndef __FLASH_H__
#define __FLASH_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdbool.h>

bool Flash_download(const char *path);





#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __FLASH_H__ */
