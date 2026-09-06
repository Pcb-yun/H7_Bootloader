/**
 * @file Boot.c
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief 引导加载器源文件
 */

#include "Boot.h"
#include "boot_shared.h"
#include "vision.h"
#include "flash.h"

#include <stdarg.h>

#define NANOPRINTF_USE_FIELD_WIDTH_FORMAT_SPECIFIERS	1	// 字段宽度与 0 填充（%08X/%02X）
#define NANOPRINTF_USE_PRECISION_FORMAT_SPECIFIERS	0	// 精度（%.n）
#define NANOPRINTF_USE_FLOAT_FORMAT_SPECIFIERS	0	// 浮点（%f/%g/%e）
#define NANOPRINTF_USE_LARGE_FORMAT_SPECIFIERS	0	// 64 位修饰符（ll/j/z/t）
#define NANOPRINTF_USE_SMALL_FORMAT_SPECIFIERS	0	// 短长度修饰符（h/hh）
#define NANOPRINTF_USE_BINARY_FORMAT_SPECIFIERS	0	// 二进制输出（%b）
#define NANOPRINTF_USE_WRITEBACK_FORMAT_SPECIFIERS	0	// 计数回写（%n）
#define NANOPRINTF_USE_ALT_FORM_FLAG	0	// 备用形式标志（#）
#define NANOPRINTF_IMPLEMENTATION

#if defined(__CC_ARM)
#pragma push
#pragma diag_suppress 2803	// unrecognized GCC pragma
#pragma diag_suppress 1293	// assignment in condition
#endif
#include "nanoprintf.h"
#if defined(__CC_ARM)
#pragma pop
#endif

#include "usart.h"
#include "sdmmc.h"
#include "fatfs.h"
#include "octospi.h"

#define BOOT_CLEAR_SCREEN "\033[2J\033[H\033[1;1H\033[2J" // 清除屏幕

typedef enum {
    ACT_INVALID = -1,
    ACT_JUMP = 0,
    ACT_UPDATE,

} BootAction_t;

static bool Flash_Check(void);
static bool App_Check(void);
static bool Read_BootShared(void);
static void Boot_Version(void);
static void Boot_Error(void);
typedef void (*pFunction)(void);

static bool shared_ready = false;
static bool fs_ready = false;
static bool w25q64_ready = false;
static BootSharedMem_t boot_shared = {0};
volatile const AppHand_t *pHand = (volatile const AppHand_t *)APP_HAND_ADDRESS;


/**
 * @brief 初始化引导加载器
 */
void Bootloader_Init(void)
{
	bool res;
	uint8_t flashID[3];

    UART_Init();
    Boot_Version();
    Read_BootShared();

    Boot_Printf("[BOOT][INFO] Init SPI Flash\r\n");
    res = SPIFlash_Init(flashID);
    if (res)
	{
        w25q64_ready = true;
		Boot_Printf("[BOOT][INFO] SPI Flash init success, ID: 0x%02X 0x%02X 0x%02X\r\n", flashID[0], flashID[1], flashID[2]);
    }
	else
	{
        Boot_Printf("[BOOT][WARN] SPI Flash init failed\r\n");
    }

    Boot_Printf("[BOOT][INFO] Init SDMMC1\r\n");
    res = SD_Init();

    if (!res)
    {
        Boot_Printf("[BOOT][WARN] SDMMC1 init failed, Please check the TF card\r\n");
    }
    else
    {
        Boot_Printf("[BOOT][INFO] SDMMC1 init success\r\n");
		Boot_Printf("[BOOT][INFO] Init FATFS\r\n");
        res = FATFS_Init();

        if (!res)
        {
            Boot_Printf("[BOOT][WARN] FATFS initialization failed, Please check the file system\r\n");
		}
        else
        {
            fs_ready = true;
            Boot_Printf("[BOOT][INFO] FATFS init success\r\n");
        }
    }

}

/**
 * @brief 引导加载程序主函数
 */
void Bootloader_main(void)
{
    BootAction_t action = ACT_JUMP;

    if (App_Check())
    {
        Boot_Printf("[BOOT][INFO] Application data is valid, ");
        Boot_Printf("Build: %s %s\r\n", pHand->build_date, pHand->build_time);
    }
    else
    {
        Boot_Printf("[BOOT][ERROR] Application data is invalid\r\n");
        action = ACT_INVALID;
    }

    // 如果共享内存有效（业务程序正常运行）
    if (shared_ready)
	{
		switch (boot_shared.bootmode)
		{
			case BOOT_MODE_NORMAL:
				break;
			case BOOT_MODE_LOCAL:
                action = ACT_UPDATE;
                break;
            default:
                break;
        }
    }

    switch (action)
    {
        case ACT_INVALID:
			// 校验不通过，无法启动
            Boot_Printf("[BOOT][ERROR] Firmware validation fails. Please power cycle.\r\n");
            Boot_Error();
            break;
        case ACT_UPDATE:
			// 准备固件更新
            if (!fs_ready)
            {
                Boot_Printf("[BOOT][ERROR] Unable to perform firmware update\r\n");
                Boot_Error();
            }
            Boot_Printf("[BOOT][INFO] Prepare to update firmware\r\n");
            if (!Flash_download((const char *)boot_shared.filePath))
                Boot_Error();
        case ACT_JUMP:
			// 直接跳转
            if (!App_Check())
            {
                Boot_Printf("[BOOT][ERROR] Firmware validation failed - corrupt image. Reflash via debugger, then power-cycle the device to reset backup domain.\r\n");
                Boot_Error();
            }
			Boot_Printf("[BOOT][INFO] Jumping to entry point...\r\n");
            break;
	}

}

/**
 * @brief 退出引导加载器
 */
void Boot_Exit(void)
{
	uint32_t i;

	// 关闭串口：等待最后一个字节发送完成，禁用 USART1 及其时钟
	while (!LL_USART_IsActiveFlag_TC(USART1)) {}
	LL_USART_Disable(USART1);
	LL_APB2_GRP1_DisableClock(LL_APB2_GRP1_PERIPH_USART1);

	// 反初始化 FATFS：卸载逻辑盘并解绑 SD 驱动
	f_mount(NULL, (const TCHAR *)SDPath, 1);
	FATFS_UnLinkDriver(SDPath);

	// 反初始化 SDMMC1（MspDeInit 内会同步关闭其时钟与引脚）
	HAL_SD_DeInit(&hsd1);

	// 恢复 LED 引脚为复位默认状态（模拟模式）
	LL_GPIO_SetPinMode(LED_GPIO_Port, LED_Pin, LL_GPIO_MODE_ANALOG);

	// 关闭全部 GPIO 端口时钟
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOA);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOB);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOC);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOD);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOE);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOF);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOG);
	LL_AHB4_GRP1_DisableClock(LL_AHB4_GRP1_PERIPH_GPIOH);

	// 关闭 DMA1 时钟
	LL_AHB1_GRP1_DisableClock(LL_AHB1_GRP1_PERIPH_DMA1);

	// 关闭全局中断，禁止响应一切可屏蔽中断
	__disable_irq();

	// 关闭 SysTick，停止计数并禁止其中断
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	// 关闭全部 NVIC 中断使能并清空挂起位，防止残留中断在 App 就绪前触发
	for (i = 0; i < 8; i++) {
		NVIC->ICER[i] = 0xFFFFFFFFU;
		NVIC->ICPR[i] = 0xFFFFFFFFU;
	}
}

/**
 * @brief 跳转到应用程序
 */
void JumpToApplication(void)
{
    // 读取 App 的初始堆栈指针（MSP）和复位地址（Reset_Handler）
    uint32_t app_stack_addr = *(__IO uint32_t *)(APP_ADDRESS);
    uint32_t app_pc_addr = *(__IO uint32_t *)(APP_ADDRESS + 4);

    // 重定向中断向量表到 App 起始地址
    SCB->VTOR = APP_ADDRESS;

    /*
        // 清空并禁用缓存，确保 App 读到最新指令与数据
        SCB_CleanInvalidateDCache();
        SCB_DisableDCache();
        SCB_DisableICache();
    */

	// 恢复全局中断（此时中断均被禁用且无挂起，等效于复位后状态），交由 App 接管
	__enable_irq();

    pFunction app_entry = (pFunction)app_pc_addr;

    // 设置新的主堆栈指针，并插入数据、指令屏障，保证跳转前状态一致
	__set_MSP(app_stack_addr);
	__DMB();
	__ISB();

	// 跳转到业务代码，正常不会返回
	app_entry();

	// 兜底
    Boot_Error();
}

/**
 * @brief 打印引导加载器版本信息
 */
static void Boot_Version(void)
{
    uint8_t major = BOOT_VERSION_MAJOR;
    uint8_t minor = BOOT_VERSION_MINOR;
    uint8_t patch = BOOT_VERSION_PATCH;

    Boot_Printf(BOOT_CLEAR_SCREEN);
    Boot_Printf(BOOT_VERSION_INFO);
    Boot_Printf(
        "\r\nVision: %d.%d.%d\r\n"
        "Build: %s %s\r\n\r\n",
        major, minor, patch,
        BOOT_BUILD_DATE, BOOT_BUILD_TIME
    );
}

/**
 * @brief 检查业务代码区向量表
 * @return true 检查通过, false 向量表错误
 */
static bool Flash_Check(void)
{
    uint32_t app_stack_addr;
    uint32_t app_pc_addr;

    // 读取 App 的初始堆栈指针（MSP）和复位地址（Reset_Handler）
    app_stack_addr = *(__IO uint32_t *)(APP_ADDRESS);
    app_pc_addr = *(__IO uint32_t *)(APP_ADDRESS + 4);

    // 校验 App 是否有效：向量未被擦除，且复位向量为 Thumb 地址（bit0 = 1）
    if ((app_stack_addr == 0xFFFFFFFFU) ||
        (app_pc_addr == 0xFFFFFFFFU) ||
        ((app_pc_addr & 0x1U) == 0U))
    {
        return false;
    }
    return true;
}

/**
 * @brief 检查业务代码是否有效
 * @return true 比对成功, false 比对失败
 */
static bool App_Check(void)
{
    if (!Flash_Check() || pHand->magic != BOOT_SHARED_MAGIC)
        return false;

    return true;
}

/**
 * @brief 读取共享区内容
 * @return true 获取成功, false 获取失败
 */
static bool Read_BootShared(void)
{
    volatile const BootSharedMem_t *pBoot = (volatile const BootSharedMem_t *)BOOT_SHARED_ADD;
    __HAL_RCC_BKPRAM_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    if (pBoot->magic != BOOT_SHARED_MAGIC)
    {
        Boot_Printf("[BOOT][WARN] Shared data is invalid\r\n");
        return false;
    }

    uint8_t i;
    boot_shared.bootmode = pBoot->bootmode;

    // 定长拷贝：规避 strlen 对非字符串共享内存的越界读及 volatile 限定丢失
    for (i = 0; i < sizeof(boot_shared.filePath); i++)
    {
        boot_shared.filePath[i] = pBoot->filePath[i];
        if (boot_shared.filePath[i] == '\0')
        {
            break;
        }
    }
    boot_shared.filePath[sizeof(boot_shared.filePath) - 1] = '\0';  // 末尾兜底置空
    shared_ready = true;

    Boot_Printf("[BOOT][INFO] Shared data is ready\r\n");
    return true;
}

/**
 * @brief 出现无法引导的错误
 */
static void Boot_Error(void)
{
	volatile uint32_t delay;
	LL_GPIO_InitTypeDef GPIO_InitStruct = {0};

	// 重新使能 GPIOG 时钟并初始化 LED 引脚
	LL_AHB4_GRP1_EnableClock(LL_AHB4_GRP1_PERIPH_GPIOG);
	GPIO_InitStruct.Pin = LED_Pin;
	GPIO_InitStruct.Mode = LL_GPIO_MODE_OUTPUT;
	GPIO_InitStruct.Speed = LL_GPIO_SPEED_FREQ_LOW;
	GPIO_InitStruct.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
	GPIO_InitStruct.Pull = LL_GPIO_PULL_NO;
	LL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

	while (1)
	{
		LL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
		for (delay = 0; delay < 0x8FFFFF; delay++);
	}
}

/**
 * @brief 终端打印格式化字符串
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
void Boot_Printf(const char *fmt, ...)
{
    static char buffer[4096];
    va_list ap;
    int len, actual_len;
    uint8_t *p;

    va_start(ap, fmt);
    len = npf_vsnprintf(buffer, sizeof(buffer), fmt, ap);
    va_end(ap);

    actual_len = (len > 4095) ? (4095) : len;

    // 轮询方式逐字节发送，等待 TXE 为空中断
    p = (uint8_t *)buffer;
    while (actual_len-- > 0)
    {
        while (!LL_USART_IsActiveFlag_TXE(USART1)) {}
        LL_USART_TransmitData8(USART1, *p++);
    }

    // 等待最后一个字节真正发送完成
    while (!LL_USART_IsActiveFlag_TC(USART1)) {}
}
