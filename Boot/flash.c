/**
 * @file flash.c
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief Flash 编程源文件
 */

#include "flash.h"
#include "stm32h7xx_hal.h"
#include "fatfs.h"
#include "boot_shared.h"
#include <string.h>

#define FLASH_WORD_SIZE 32U	// H7 单次编程宽度 256bit = 32 字节
#define USE_VREIFY 1		// 是否在烧录后进行校验

static bool Flash_Erase(uint32_t startAddr, uint32_t size);
static bool Flash_Write(uint32_t address, const uint32_t *data, uint32_t blockCount);
#if USE_VREIFY
static bool Flash_Verify(const char *path);
#endif
void Boot_Printf(const char *fmt, ...);


/**
 * @brief 烧录固件
 * @param path 固件文件名
 * @return true: 烧录成功, false: 烧录失败
 */
bool Flash_download(const char *path)
{
	FIL file;
	FRESULT fres;
	UINT readBytes;
	uint32_t address;
	uint32_t fileSize;
	uint32_t remain;
	uint32_t nextReport;
	static uint8_t buffer[FLASH_WORD_SIZE];

	fres = f_open(&file, path, FA_READ);
	if (fres != FR_OK)
	{
		Boot_Printf("\r\n[BOOT][ERROR] open %s failed\r\n", path);
		return false;
	}

	fileSize = f_size(&file);
	if ((fileSize == 0U))
	{
		Boot_Printf("\r\n[BOOT][ERROR] invalid firmware size: %lu bytes\r\n", (unsigned long)fileSize);
		f_close(&file);
		return false;
	}

	if (!Flash_Erase(APP_ADDRESS, fileSize))
	{
		Boot_Printf("\r\n[BOOT][ERROR] erase app region failed\r\n");
		f_close(&file);
		return false;
	}

	address = APP_ADDRESS;
	remain = fileSize;
	nextReport = APP_ADDRESS;
	while (remain > 0U)
	{
		fres = f_read(&file, buffer, FLASH_WORD_SIZE, &readBytes);
		if ((fres != FR_OK) || (readBytes == 0U))
		{
			Boot_Printf("\r[BOOT][ERROR] read firmware failed\r\n");
			f_close(&file);
			return false;
		}

		// 最后一块不足 32 字节时，用 0xFF 填充
		if (readBytes < FLASH_WORD_SIZE)
		{
			memset(&buffer[readBytes], 0xFF, FLASH_WORD_SIZE - readBytes);
		}

		if (!Flash_Write(address, (const uint32_t *)buffer, 1U))
		{
			Boot_Printf("\r[BOOT][ERROR] program failed at 0x%08X\r\n", address);
			f_close(&file);
			return false;
		}

		address += FLASH_WORD_SIZE;
		remain -= (remain >= FLASH_WORD_SIZE) ? FLASH_WORD_SIZE : remain;

		if ((address >= nextReport) || (remain == 0U))
		{
            Boot_Printf("\r[BOOT][INFO] program: 0x%08X", address);
            nextReport += 0x1000U;
		}
	}

	f_close(&file);
	Boot_Printf("\r[BOOT][INFO] program success, %lu bytes\r\n", (unsigned long)fileSize);

#if USE_VREIFY
    if (!Flash_Verify(path))
		return false;
#endif

	return true;
}

#if USE_VREIFY
/**
 * @brief 程序校验
 * @param path 固件文件名
 * @return true: 成功, false: 失败
 */
static bool Flash_Verify(const char *path)
{
	FIL file;
	FRESULT fres;
	UINT readBytes;
	uint32_t address;
	uint32_t remain;
	uint32_t nextReport;
	static uint8_t fileBuf[FLASH_WORD_SIZE];

	fres = f_open(&file, path, FA_READ);
	if (fres != FR_OK)
	{
		Boot_Printf("[BOOT][ERROR] verify: open %s failed\r\n", path);
		return false;
	}

	address = APP_ADDRESS;
	remain = f_size(&file);
	nextReport = APP_ADDRESS;

	while (remain > 0U)
	{
		fres = f_read(&file, fileBuf, FLASH_WORD_SIZE, &readBytes);
		if ((fres != FR_OK) || (readBytes == 0U))
		{
			Boot_Printf("\r[BOOT][ERROR] verify: read firmware failed\r\n");
			f_close(&file);
			return false;
		}

		if (memcmp(fileBuf, (const void *)address, readBytes) != 0)
		{
			Boot_Printf("\r[BOOT][ERROR] verify failed at 0x%08X\r\n", address);
			f_close(&file);
			return false;
		}

		address += FLASH_WORD_SIZE;
		remain -= readBytes;

		if ((address >= nextReport) || (remain == 0U))
		{
			Boot_Printf("\r[BOOT][INFO] verify: 0x%08X", address);
			nextReport += 0x1000U;
		}
	}

	f_close(&file);
	Boot_Printf("\r[BOOT][INFO] verify success    \r\n");

	return true;
}
#endif

/**
 * @brief 片擦除
 * @param startAddr 起始地址
 * @param size 待擦除长度（字节）
 * @return true: 成功, false: 失败
 */
static bool Flash_Erase(uint32_t startAddr, uint32_t size)
{
	FLASH_EraseInitTypeDef eraseInit;
	uint32_t sectorError = 0;
	uint32_t firstSector;
	uint32_t lastSector;
	uint32_t sector;
	uint32_t sectorAddr;

	firstSector = (startAddr - FLASH_BASE) / FLASH_SECTOR_SIZE;
	lastSector = (startAddr + size - 1U - FLASH_BASE) / FLASH_SECTOR_SIZE;

	HAL_FLASH_Unlock();

	eraseInit.TypeErase    = FLASH_TYPEERASE_SECTORS;		// 按扇区擦除
	eraseInit.Banks        = FLASH_BANK_1;				// 单 Bank
	eraseInit.NbSectors    = 1U;					// 每次擦除一个扇区
	eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;		// 电压范围

	for (sector = firstSector; sector <= lastSector; sector++)
	{
		eraseInit.Sector = sector;
		sectorAddr = FLASH_BASE + sector * FLASH_SECTOR_SIZE;

		Boot_Printf("\r[BOOT][INFO] erase: 0x%08X", sectorAddr);

		if (HAL_FLASHEx_Erase(&eraseInit, &sectorError) != HAL_OK)
		{
			Boot_Printf("\r[BOOT][ERROR] erase failed at sector %lu (0x%08X)\r\n",
			            (unsigned long)sector, sectorAddr);
			HAL_FLASH_Lock();
			return false;
		}
	}

	HAL_FLASH_Lock();
	Boot_Printf("\r[BOOT][INFO] erase success    \r\n");
	return true;
}

/**
 * @brief 向指定地址烧录数据
 * @param address 烧录起始地址（必须 32 字节对齐）
 * @param data 源数据指针（每块 32 字节）
 * @param blockCount 待烧录的 32 字节块数量
 * @return true: 成功, false: 失败
 */
static bool Flash_Write(uint32_t address, const uint32_t *data, uint32_t blockCount)
{
	HAL_FLASH_Unlock();

	while (blockCount > 0U)
	{
		if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, address, (uint32_t)data) != HAL_OK)
		{
			HAL_FLASH_Lock();
			return false;
		}
		address += FLASH_WORD_SIZE;
		data += FLASH_WORD_SIZE / 4U;
		blockCount--;
	}

	HAL_FLASH_Lock();
	return true;
}
