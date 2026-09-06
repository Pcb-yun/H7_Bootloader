/**
 * @file boot_shared.h
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief 引导加载器与应用之间的共享内存定义头文件
 */

#include <stdint.h>

#define BOOT_SHARED_MAGIC 0x424F4F54U // 有效性验证魔法数字
#define APP_ADDRESS       0x08020000U // 业务代码起始地址
#define APP_HAND_ADDRESS  0x08020400U // 业务代码校验头地址
#define BOOT_SHARED_ADD   0x38800000U // 引导加载器共享内存地址（备份 SRAM）

typedef enum {
    BOOT_MODE_NORMAL = 0, // 正常启动模式：直接跳转业务代码
    BOOT_MODE_LOCAL,      // 本地更新模式：从 SD 卡烧录固件

} BootMode_t;

/**
 * @brief 引导加载器共享内存结构
 */
typedef struct
{
    uint32_t magic;       // 有效性校验魔法数字
    uint32_t bootmode;    // 引导模式（见 BootMode_t）
    uint8_t filePath[16]; // 待烧录固件文件名
} BootSharedMem_t;

/**
 * @brief 业务代码信息头结构
 */
typedef struct
{
    uint32_t magic;         // 有效性校验魔法数字
    uint8_t build_date[12]; // 编译日期字符串
    uint8_t build_time[12]; // 编译时间字符串
} AppHand_t;
