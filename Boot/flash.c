/**
 * @file flash.c
 * @author Pcb-yun (pcbyinyun@163.com)
 * @brief Flash 编程源文件
 */

#include "flash.h"
#include "stm32h7xx_hal.h"
#include "fatfs.h"
#include "boot_shared.h"
#include "octospi.h"
#include "kk_ihex_read.h"
#include <string.h>

#define FLASH_WORD_SIZE 32U	// H7 单次编程宽度 256bit = 32 字节
#define USE_VREIFY 1		// 是否在烧录后进行校验

#define APP_END_ADDRESS (APP_ADDRESS + 0x000E0000U)	// 片内业务代码结束地址（不含）
#define W25Q64_END_ADDRESS (W25Q64_ADDRESS + 0x00800000U)	// 片外 Flash 结束地址（不含）

#define HEX_BUFFER_SIZE 512U	// hex 文件读取缓冲区大小
#define HEX_REPORT_STEP 0x1000U	// 烧录进度打印步长（字节）

#define W25Q64_CMD_WRITE_ENABLE 0x06U	// 写使能
#define W25Q64_CMD_READ_STATUS_REG1 0x05U	// 读状态寄存器 1
#define W25Q64_CMD_PAGE_PROGRAM 0x02U	// 页编程
#define W25Q64_CMD_SECTOR_ERASE 0x20U	// 4KB 扇区擦除
#define W25Q64_CMD_BLOCK_ERASE 0xD8U	// 64KB 块擦除
#define W25Q64_CMD_FAST_READ 0x0BU	// 快速读取
#define W25Q64_STATUS1_BUSY 0x01U	// 忙标志
#define W25Q64_PAGE_SIZE 256U	// 页大小
#define W25Q64_SECTOR_SIZE 0x1000U	// 扇区大小
#define W25Q64_BLOCK_SIZE 0x10000U	// 块大小
#define W25Q64_FAST_READ_DUMMY 8U	// 快速读取的空周期数
#define W25Q64_TIMEOUT_MS 1000U	// 等待芯片就绪的超时时间

/**
 * @brief 烧录目标区域
 */
typedef enum {
	HEX_REGION_NONE = 0,	// 非法地址
	HEX_REGION_INTERNAL,	// 片内 Flash
	HEX_REGION_EXTERNAL,	// 片外 W25Q64

} HexRegion_t;

/**
 * @brief hex 解析阶段
 */
typedef enum {
	HEX_PASS_SCAN = 0,	// 第一遍：统计地址范围
	HEX_PASS_WRITE,		// 第二遍：编程并校验

} HexPass_t;

static bool Hex_ParseFile(const char *path);
static void Hex_ResetState(void);
static HexRegion_t Hex_Region(uint32_t addr);
static bool Flash_Erase(uint32_t address, uint32_t size);
static bool Flash_Write(uint32_t address, const uint8_t *data, uint32_t length);
static bool Flash_Flush(void);
static bool Hex_ExternalWrite(uint32_t offset, const uint8_t *data, uint32_t length);
static bool Hex_ExternalRead(uint32_t offset, uint8_t *data, uint32_t length);
static bool Hex_ExternalEraseSector(uint8_t instruction, uint32_t offset);
static bool Hex_ExternalWriteEnable(void);
static bool Hex_ExternalWaitReady(void);
static void Hex_ExternalCmd(OSPI_RegularCmdTypeDef *cmd, uint8_t instruction);
void Boot_Printf(const char *fmt, ...);

static HexPass_t hex_pass;			// 当前解析阶段
static bool hex_error;				// 解析过程中是否出错
static bool hex_eof;				// 是否遇到文件结束记录
static uint32_t hex_total;			// hex 数据总字节数
static uint32_t hex_programmed;			// 已编程字节数
static uint32_t hex_next_report;		// 下一次进度打印的字节数
static bool hex_int_used;			// 片内 Flash 是否被使用
static uint32_t hex_int_min;			// 片内 Flash 最小地址
static uint32_t hex_int_max;			// 片内 Flash 最大地址
static bool hex_ext_used;			// 片外 Flash 是否被使用
static uint32_t hex_ext_min;			// 片外 Flash 最小地址
static uint32_t hex_ext_max;			// 片外 Flash 最大地址
static bool hex_word_used;			// flashword 编程窗口是否已启用
static uint32_t hex_word_addr;			// flashword 编程窗口的对齐地址
static uint32_t hex_written_end;		// 已编程区域的下一个字节地址
static uint32_t hex_word_buf[FLASH_WORD_SIZE / 4U] __ALIGNED(FLASH_WORD_SIZE);	// flashword 编程窗口
static uint8_t hex_verify_buf[W25Q64_PAGE_SIZE];	// 校验读回缓冲区


/**
 * @brief 烧录 hex 固件（同时支持片内 Flash 与片外 W25Q64）
 * @param path 固件文件名
 * @return true: 烧录成功, false: 烧录失败
 */
bool Flash_download(const char *path)
{
	// 第一遍：统计片内/片外地址范围
	Hex_ResetState();
	hex_pass = HEX_PASS_SCAN;
	if (!Hex_ParseFile(path) || hex_error)
	{
		Boot_Printf("\r\n[BOOT][ERROR] parse hex file failed\r\n");
		return false;
	}

	if (!hex_eof)
	{
		Boot_Printf("\r\n[BOOT][ERROR] invalid hex file: no end of file record\r\n");
		return false;
	}

	if (hex_total == 0U)
	{
		Boot_Printf("\r\n[BOOT][ERROR] no data record in hex file\r\n");
		return false;
	}

	// 擦除片内业务代码区
	if (hex_int_used && !Flash_Erase(hex_int_min, hex_int_max - hex_int_min + 1U))
	{
		Boot_Printf("\r\n[BOOT][ERROR] erase failed\r\n");
		return false;
	}

	// 擦除片外 Flash 对应区域
	if (hex_ext_used && !Flash_Erase(hex_ext_min, hex_ext_max - hex_ext_min + 1U))
	{
		Boot_Printf("\r\n[BOOT][ERROR] erase failed\r\n");
		return false;
	}

	// 第二遍：编程并校验
	Hex_ResetState();
	hex_pass = HEX_PASS_WRITE;
	hex_next_report = HEX_REPORT_STEP;
	if (!Hex_ParseFile(path) || hex_error)
	{
		Boot_Printf("\r\n[BOOT][ERROR] program hex file failed\r\n");
		return false;
	}

	// 提交最后一个未满的 flashword
	if (!Flash_Flush())
	{
		Boot_Printf("\r\n[BOOT][ERROR] program failed\r\n");
		return false;
	}

	return true;
}

/**
 * @brief 分块解析 hex 文件
 * @param path 固件文件名
 * @return true: 文件读取完成, false: 文件读取失败
 */
static bool Hex_ParseFile(const char *path)
{
	FIL file;
	FRESULT fres;
	UINT readBytes;
	static char buffer[HEX_BUFFER_SIZE];
	static struct ihex_state ihex;

	fres = f_open(&file, path, FA_READ);
	if (fres != FR_OK)
	{
		Boot_Printf("\r\n[BOOT][ERROR] open %s failed\r\n", path);
		return false;
	}

	ihex_begin_read(&ihex);

	do
	{
		fres = f_read(&file, buffer, sizeof(buffer), &readBytes);
		if (fres != FR_OK)
		{
			Boot_Printf("\r\n[BOOT][ERROR] read hex file failed\r\n");
			f_close(&file);
			return false;
		}

		if (readBytes > 0U)
			ihex_read_bytes(&ihex, buffer, (ihex_count_t)readBytes);
	} while ((readBytes == sizeof(buffer)) && !hex_error);

	ihex_end_read(&ihex);
	f_close(&file);

	return true;
}

/**
 * @brief 复位 hex 解析状态
 */
static void Hex_ResetState(void)
{
	hex_error = false;
	hex_eof = false;
	hex_total = 0U;
	hex_programmed = 0U;
	hex_next_report = 0U;
	hex_int_used = false;
	hex_int_min = 0xFFFFFFFFU;
	hex_int_max = 0U;
	hex_ext_used = false;
	hex_ext_min = 0xFFFFFFFFU;
	hex_ext_max = 0U;
	hex_word_used = false;
	hex_word_addr = 0U;
	hex_written_end = 0U;
}

/**
 * @brief 判断地址所属的烧录区域
 * @param addr 目标地址
 * @return 地址所属区域
 */
static HexRegion_t Hex_Region(uint32_t addr)
{
	if ((addr >= APP_ADDRESS) && (addr < APP_END_ADDRESS))
		return HEX_REGION_INTERNAL;

	if ((addr >= W25Q64_ADDRESS) && (addr < W25Q64_END_ADDRESS))
		return HEX_REGION_EXTERNAL;

	return HEX_REGION_NONE;
}

/**
 * @brief 向指定地址写入数据（按地址自动选择片内或片外 Flash）
 * @param address 目标地址
 * @param data 源数据指针
 * @param length 数据长度（字节）
 * @return true: 成功, false: 失败
 */
static bool Flash_Write(uint32_t address, const uint8_t *data, uint32_t length)
{
	HexRegion_t region = Hex_Region(address);
	uint8_t *word = (uint8_t *)hex_word_buf;
	uint32_t wordAddr;

	if ((region == HEX_REGION_NONE) || (length == 0U))
		return false;

	// 片外 Flash 按页边界拆分编程
	if (region == HEX_REGION_EXTERNAL)
		return Hex_ExternalWrite(address - W25Q64_ADDRESS, data, length);

	while (length > 0U)
	{
		wordAddr = address & ~(FLASH_WORD_SIZE - 1U);

		// 跨 flashword 时先提交上一个窗口
		if (hex_word_used && (wordAddr != hex_word_addr))
		{
			if (!Flash_Flush())
				return false;
		}

		if (!hex_word_used)
		{
			// 同一个 flashword 只能编程一次，回退写入须判错
			if (wordAddr < hex_written_end)
				return false;

			// 未被 hex 覆盖的字节保持擦除后的 0xFF
			memset(word, 0xFF, FLASH_WORD_SIZE);
			hex_word_addr = wordAddr;
			hex_word_used = true;
		}

		word[address - hex_word_addr] = *data++;
		address++;
		length--;
	}

	return true;
}

/**
 * @brief 提交当前未满的片内 flashword 编程窗口
 * @return true: 成功, false: 失败
 */
static bool Flash_Flush(void)
{
	if (!hex_word_used)
		return true;

	HAL_FLASH_Unlock();
	if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD, hex_word_addr, (uint32_t)hex_word_buf) != HAL_OK)
	{
		HAL_FLASH_Lock();
		return false;
	}
	HAL_FLASH_Lock();

#if USE_VREIFY
	// 包含补 0xFF 的部分，可同时验证擦除结果
	if (memcmp(hex_word_buf, (const void *)hex_word_addr, FLASH_WORD_SIZE) != 0)
		return false;
#endif

	hex_written_end = hex_word_addr + FLASH_WORD_SIZE;
	hex_word_used = false;
	return true;
}

/**
 * @brief 向片外 Flash 写入数据（自动按页边界拆分）
 * @param offset 片外 Flash 内偏移
 * @param data 源数据指针
 * @param length 数据长度（字节）
 * @return true: 成功, false: 失败
 */
static bool Hex_ExternalWrite(uint32_t offset, const uint8_t *data, uint32_t length)
{
	OSPI_RegularCmdTypeDef cmd;
	uint32_t chunk;

	while (length > 0U)
	{
		// 单次编程不得跨 256 字节页边界
		chunk = W25Q64_PAGE_SIZE - (offset & (W25Q64_PAGE_SIZE - 1U));
		if (chunk > length)
			chunk = length;

		if (!Hex_ExternalWriteEnable())
			return false;

		Hex_ExternalCmd(&cmd, W25Q64_CMD_PAGE_PROGRAM);
		cmd.Address = offset;
		cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
		cmd.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
		cmd.DataMode = HAL_OSPI_DATA_1_LINE;
		cmd.NbData = chunk;
		if (HAL_OSPI_Command(&hospi1, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
			return false;

		if (HAL_OSPI_Transmit(&hospi1, (uint8_t *)data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
			return false;

		if (!Hex_ExternalWaitReady())
			return false;

#if USE_VREIFY
		if (!Hex_ExternalRead(offset, hex_verify_buf, chunk) ||
		    (memcmp(hex_verify_buf, data, chunk) != 0))
			return false;
#endif

		offset += chunk;
		data += chunk;
		length -= chunk;
	}

	return true;
}

/**
 * @brief 从片外 Flash 读取数据
 * @param offset 片外 Flash 内偏移
 * @param data 读取缓冲区
 * @param length 读取长度（字节）
 * @return true: 成功, false: 失败
 */
static bool Hex_ExternalRead(uint32_t offset, uint8_t *data, uint32_t length)
{
	OSPI_RegularCmdTypeDef cmd;

	Hex_ExternalCmd(&cmd, W25Q64_CMD_FAST_READ);
	cmd.Address = offset;
	cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
	cmd.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
	cmd.DataMode = HAL_OSPI_DATA_1_LINE;
	cmd.DummyCycles = W25Q64_FAST_READ_DUMMY;
	cmd.NbData = length;
	if (HAL_OSPI_Command(&hospi1, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
		return false;

	return HAL_OSPI_Receive(&hospi1, data, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK;
}

/**
 * @brief 执行擦除类指令并等待完成
 * @param instruction 擦除指令（4KB 扇区或 64KB 块）
 * @param offset 片外 Flash 内偏移（须与指令粒度对齐）
 * @return true: 成功, false: 失败
 */
static bool Hex_ExternalEraseSector(uint8_t instruction, uint32_t offset)
{
	OSPI_RegularCmdTypeDef cmd;

	if (!Hex_ExternalWriteEnable())
		return false;

	Hex_ExternalCmd(&cmd, instruction);
	cmd.Address = offset;
	cmd.AddressMode = HAL_OSPI_ADDRESS_1_LINE;
	cmd.AddressSize = HAL_OSPI_ADDRESS_24_BITS;
	if (HAL_OSPI_Command(&hospi1, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
		return false;

	return Hex_ExternalWaitReady();
}

/**
 * @brief 发送片外 Flash 写使能指令
 * @return true: 成功, false: 失败
 */
static bool Hex_ExternalWriteEnable(void)
{
	OSPI_RegularCmdTypeDef cmd;

	Hex_ExternalCmd(&cmd, W25Q64_CMD_WRITE_ENABLE);
	return HAL_OSPI_Command(&hospi1, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) == HAL_OK;
}

/**
 * @brief 轮询状态寄存器等待片外 Flash 就绪
 * @return true: 芯片就绪, false: 通信失败或等待超时
 */
static bool Hex_ExternalWaitReady(void)
{
	OSPI_RegularCmdTypeDef cmd;
	uint32_t start = HAL_GetTick();
	uint8_t status = 0;

	do
	{
		Hex_ExternalCmd(&cmd, W25Q64_CMD_READ_STATUS_REG1);
		cmd.DataMode = HAL_OSPI_DATA_1_LINE;
		cmd.NbData = 1U;
		if (HAL_OSPI_Command(&hospi1, &cmd, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
			return false;
		if (HAL_OSPI_Receive(&hospi1, &status, HAL_OSPI_TIMEOUT_DEFAULT_VALUE) != HAL_OK)
			return false;

		if ((status & W25Q64_STATUS1_BUSY) == 0U)
			return true;
	} while ((HAL_GetTick() - start) < W25Q64_TIMEOUT_MS);

	return false;
}

/**
 * @brief 填充片外 Flash 单线指令的通用配置
 * @param cmd OCTOSPI 命令结构体指针
 * @param instruction W25Q64 指令码
 */
static void Hex_ExternalCmd(OSPI_RegularCmdTypeDef *cmd, uint8_t instruction)
{
	memset(cmd, 0, sizeof(OSPI_RegularCmdTypeDef));
	cmd->OperationType = HAL_OSPI_OPTYPE_COMMON_CFG;
	cmd->FlashId = HAL_OSPI_FLASH_ID_1;
	cmd->Instruction = instruction;
	cmd->InstructionMode = HAL_OSPI_INSTRUCTION_1_LINE;
	cmd->InstructionSize = HAL_OSPI_INSTRUCTION_8_BITS;
	cmd->InstructionDtrMode = HAL_OSPI_INSTRUCTION_DTR_DISABLE;
	cmd->AddressMode = HAL_OSPI_ADDRESS_NONE;
	cmd->AddressDtrMode = HAL_OSPI_ADDRESS_DTR_DISABLE;
	cmd->AlternateBytesMode = HAL_OSPI_ALTERNATE_BYTES_NONE;
	cmd->AlternateBytesDtrMode = HAL_OSPI_ALTERNATE_BYTES_DTR_DISABLE;
	cmd->DataMode = HAL_OSPI_DATA_NONE;
	cmd->DataDtrMode = HAL_OSPI_DATA_DTR_DISABLE;
	cmd->DQSMode = HAL_OSPI_DQS_DISABLE;
	cmd->SIOOMode = HAL_OSPI_SIOO_INST_EVERY_CMD;
}

/**
 * @brief IHEX 库回调：处理一条完整的 hex 记录
 * @param ihex IHEX 解析状态
 * @param type 记录类型
 * @param checksum_mismatch 校验和是否错误
 * @return 0: 记录有误, 非 0: 记录正常
 */
ihex_bool_t ihex_data_read(struct ihex_state *ihex, ihex_record_type_t type, ihex_bool_t checksum_mismatch)
{
	HexRegion_t region;
	uint32_t address;
	uint32_t length;

	// 校验和错误或行长超限，直接判定文件有误
	if ((checksum_mismatch != 0U) || (ihex->length < ihex->line_length))
	{
		hex_error = true;
		return 0;
	}

	if (type == IHEX_END_OF_FILE_RECORD)
	{
		hex_eof = true;
		return 1;
	}

	// 扩展地址等记录由库内部处理，此处放行
	if (type != IHEX_DATA_RECORD)
		return 1;

	length = ihex->length;
	if (length == 0U)
		return 1;

	address = (uint32_t)IHEX_LINEAR_ADDRESS(ihex);
	region = Hex_Region(address);

	// 地址必须落在同一合法区域内（首尾不得跨区）
	if ((region == HEX_REGION_NONE) || (Hex_Region(address + length - 1U) != region))
	{
		Boot_Printf("\r\n[BOOT][ERROR] invalid hex address: 0x%08X\r\n", address);
		hex_error = true;
		return 0;
	}

	// 第一遍：统计地址范围
	if (hex_pass == HEX_PASS_SCAN)
	{
		if (region == HEX_REGION_INTERNAL)
		{
			if (!hex_int_used || (address < hex_int_min))
				hex_int_min = address;
			if (!hex_int_used || ((address + length - 1U) > hex_int_max))
				hex_int_max = address + length - 1U;
			hex_int_used = true;
		}
		else
		{
			if (!hex_ext_used || (address < hex_ext_min))
				hex_ext_min = address;
			if (!hex_ext_used || ((address + length - 1U) > hex_ext_max))
				hex_ext_max = address + length - 1U;
			hex_ext_used = true;
		}

		hex_total += length;
		return 1;
	}

	// 第二遍：编程并校验
	if (!Flash_Write(address, ihex->data, length))
	{
		Boot_Printf("\r\n[BOOT][ERROR] program failed at 0x%08X\r\n", address);
		hex_error = true;
		return 0;
	}

	hex_programmed += length;
	if (hex_programmed >= hex_next_report)
	{
		Boot_Printf("\r[BOOT][INFO] program: 0x%08X", address);
		hex_next_report += HEX_REPORT_STEP;
	}

	return 1;
}

/**
 * @brief 片擦除（按地址自动选择片内或片外 Flash）
 * @param address 起始地址
 * @param size 待擦除长度（字节）
 * @return true: 成功, false: 失败
 */
static bool Flash_Erase(uint32_t address, uint32_t size)
{
	FLASH_EraseInitTypeDef eraseInit;
	uint32_t sectorError = 0;
	HexRegion_t region = Hex_Region(address);
	uint32_t begin = address;
	uint32_t end = address + size;

	if ((region == HEX_REGION_NONE) || (size == 0U) || (end < begin))
		return false;

	if (region == HEX_REGION_INTERNAL)
	{
		// 片内按 128KB 扇区对齐
		begin = address & ~(FLASH_SECTOR_SIZE - 1U);
		end = (end + FLASH_SECTOR_SIZE - 1U) & ~(FLASH_SECTOR_SIZE - 1U);

		HAL_FLASH_Unlock();

		eraseInit.TypeErase    = FLASH_TYPEERASE_SECTORS;		// 按扇区擦除
		eraseInit.Banks        = FLASH_BANK_1;				// 单 Bank
		eraseInit.NbSectors    = 1U;					// 每次擦除一个扇区
		eraseInit.VoltageRange = FLASH_VOLTAGE_RANGE_3;		// 电压范围
	}
	else
	{
		// 片外按 4KB 扇区对齐
		begin = address & ~(W25Q64_SECTOR_SIZE - 1U);
		end = (end + W25Q64_SECTOR_SIZE - 1U) & ~(W25Q64_SECTOR_SIZE - 1U);
	}

	while (begin < end)
	{
		Boot_Printf("\r[BOOT][INFO] erase: 0x%08X", begin);

		if (region == HEX_REGION_INTERNAL)
		{
			eraseInit.Sector = (begin - FLASH_BASE) / FLASH_SECTOR_SIZE;
			if (HAL_FLASHEx_Erase(&eraseInit, &sectorError) != HAL_OK)
			{
				HAL_FLASH_Lock();
				return false;
			}
			begin += FLASH_SECTOR_SIZE;
		}
		else if (((begin & (W25Q64_BLOCK_SIZE - 1U)) == 0U) && ((end - begin) >= W25Q64_BLOCK_SIZE))
		{
			// 64KB 对齐且剩余空间足够时优先使用块擦除
			if (!Hex_ExternalEraseSector(W25Q64_CMD_BLOCK_ERASE, begin - W25Q64_ADDRESS))
				return false;
			begin += W25Q64_BLOCK_SIZE;
		}
		else
		{
			if (!Hex_ExternalEraseSector(W25Q64_CMD_SECTOR_ERASE, begin - W25Q64_ADDRESS))
				return false;
			begin += W25Q64_SECTOR_SIZE;
		}
	}

	if (region == HEX_REGION_INTERNAL)
		HAL_FLASH_Lock();

	return true;
}
