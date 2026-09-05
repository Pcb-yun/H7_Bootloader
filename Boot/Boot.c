/**
 * @file Boot.c
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief 引导加载器源文件
 */

#include "Boot.h"
#include "vision.h"

#include <stdarg.h>
#include <stdio.h>

#include "usart.h"
#include "sdmmc.h"
#include "fatfs.h"
// #include "octospi.h"

#define APP_ADDRESS 0x08020000U // 业务代码起始地址
#define HEADER_OFFSET 0x400U    // 业务代码头偏移量


#define BOOT_CLEAR_SCREEN "\033[2J\033[H\033[1;1H\033[2J" // 清除屏幕

typedef enum {
    ACT_INVALID = -1,
    ACT_JUMP = 0,
    ACT_UPDATE,

} BootAction_t;


static void Boot_Printf(const char *fmt, ...);
static void Boot_Version(void);
static void Boot_Error(void);
typedef void (*pFunction)(void);
static bool all_ready = false;


/**
 * @brief 初始化引导加载器
 */
void Bootloader_Init(void)
{
    MX_USART1_UART_Init();
    Boot_Version();

	Boot_Printf("[BOOT][INFO] Initializing SDMMC1...\r\n");
	if (!SD_Init()) {
		Boot_Printf("[BOOT][WARN] SDMMC1 initialization failed.\r\n"
					"Please check the TF card connection\r\n");
		goto skeep;
	} else {
		Boot_Printf("[BOOT][INFO] SDMMC1 initialized successfully.\r\n");
		Boot_Printf("[BOOT][INFO] Initializing FATFS...\r\n");
		if (!FATFS_Init()) {
			Boot_Printf("[BOOT][WARN] FATFS initialization failed.\r\n");
			goto skeep;
		} else {
			Boot_Printf("[BOOT][INFO] FATFS initialized successfully.\r\n");
		}
	}

	// MX_OCTOSPI1_Init();

	all_ready = true;
    Boot_Printf("[BOOT][INFO] Bootloader initialized.\r\n");
	return;

skeep:
	Boot_Printf("[BOOT][WARN] Skip Update\r\n");
}

void Bootloader_main(void)
{
    BootAction_t action = ACT_JUMP;

    switch(action)
    {
        case ACT_INVALID:
            Boot_Printf("[BOOT][ERROR] Invalid header or corrupt firmware, waiting for recovery...\r\n");
            Boot_Error();
            break;
        case ACT_JUMP:
            Boot_Printf("[BOOT][INFO] jumping to entry point...\r\n\r\n");
            break;
        case ACT_UPDATE:
            Boot_Printf("[BOOT][INFO] Firmware update requested, entering download mode...\r\n");
            break;
    }

}

/**
 * @brief 退出引导加载器
 */
void Boot_Exit(void)
{
	uint32_t i;

	// 关闭所有打开的外设
	HAL_UART_DeInit(&huart1);

	// 反初始化 FATFS：卸载逻辑盘并解绑 SD 驱动
	f_mount(NULL, (const TCHAR *)SDPath, 1);
	FATFS_UnLinkDriver(SDPath);

	// 反初始化 SDMMC1
	HAL_SD_DeInit(&hsd1);

	// 恢复 LED 引脚为复位默认状态（模拟输入）
	HAL_GPIO_DeInit(LED_GPIO_Port, LED_Pin);

	// 关闭全部 GPIO 端口时钟
	__HAL_RCC_GPIOA_CLK_DISABLE();	// GPIOA（PA9/PA10 已由 UART DeInit 恢复）
	__HAL_RCC_GPIOB_CLK_DISABLE();	// GPIOB
	__HAL_RCC_GPIOC_CLK_DISABLE();	// GPIOC（SDMMC 引脚已由 SD DeInit 恢复）
	__HAL_RCC_GPIOD_CLK_DISABLE();	// GPIOD（SDMMC 引脚已由 SD DeInit 恢复）
	__HAL_RCC_GPIOE_CLK_DISABLE();	// GPIOE
	__HAL_RCC_GPIOF_CLK_DISABLE();	// GPIOF
	__HAL_RCC_GPIOG_CLK_DISABLE();	// GPIOG（LED 已复位，报错时由 Boot_Error 自行恢复）
	__HAL_RCC_GPIOH_CLK_DISABLE();	// GPIOH

	// 关闭 DMA1 时钟（DMA 流已在 UART DeInit 中复位）
	__HAL_RCC_DMA1_CLK_DISABLE();

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
	uint32_t app_stack_addr;
	uint32_t app_pc_addr;
	pFunction app_entry;

	// 读取 App 的初始堆栈指针（MSP）和复位地址（Reset_Handler）
	app_stack_addr = *(__IO uint32_t *)(APP_ADDRESS);
	app_pc_addr = *(__IO uint32_t *)(APP_ADDRESS + 4);

	// 校验 App 是否有效：向量未被擦除，且复位向量为 Thumb 地址（bit0 = 1）
	if ((app_stack_addr == 0xFFFFFFFFU) ||
		(app_pc_addr == 0xFFFFFFFFU) ||
		((app_pc_addr & 0x1U) == 0U)) {
		Boot_Error();
	}

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

	app_entry = (pFunction)app_pc_addr;

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
 * @brief 出现无法引导的错误
 * @note  本函数可能在 Boot_Exit 清理 GPIO 之后被调用，需自行恢复报错环境
 */
static void Boot_Error(void)
{
	volatile uint32_t delay;
	GPIO_InitTypeDef GPIO_InitStruct = {0};

	// 重新使能 GPIOG 时钟并初始化 LED 引脚（Boot_Exit 可能已将其复位）
	__HAL_RCC_GPIOG_CLK_ENABLE();
	GPIO_InitStruct.Pin = LED_Pin;	// LED 引脚
	GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;	// 推挽输出
	GPIO_InitStruct.Pull = GPIO_NOPULL;	// 无上下拉
	GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;	// 低速
	HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

	while (1)
	{
		HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
		for (delay = 0; delay < 1000000U; delay++);
	}
}

/**
 * @brief 终端打印格式化字符串
 * @param fmt 格式化字符串
 * @param ... 可变参数
 */
static void Boot_Printf(const char *fmt, ...)
{
    static char buffer[4096];
    va_list ap;
    int len, actual_len;

    va_start(ap, fmt);
    len = vsnprintf(buffer, 4095, fmt, ap);
    va_end(ap);

    actual_len = (len > 4095) ? (4095) : len;

    HAL_UART_Transmit(&huart1, (uint8_t *)(buffer), actual_len, 0xFFFF);
}
