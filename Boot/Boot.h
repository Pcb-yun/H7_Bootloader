/**
 * @file Boot.h
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief 引导加载器头文件
 */

 #ifndef __BOOT_H__
 #define __BOOT_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stdbool.h>
#include <stdint.h>

void Bootloader_Init(void);
void Bootloader_main(void);
void Boot_Exit(void);
void JumpToApplication(void);

bool SD_Init(void);
bool FATFS_Init(void);







#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __BOOT_H__ */
