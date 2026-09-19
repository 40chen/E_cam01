/*
 * gb2312_table.h — GB2312 -> Unicode 查表（**由脚本生成，不要手改**）
 *
 * 生成命令：python tools/gen_gb2312_table.py
 *
 * 下标 = (高字节 - 0xA1) * 94 + (低字节 - 0xA1)，87 个区 × 94 个位。
 * 值为 0 表示该位置没有对应字符（空位或 GBK 扩展区）。
 */
#pragma once

#include <stdint.h>

#define GB2312_TABLE_ROWS 87
#define GB2312_TABLE_COLS 94
#define GB2312_TABLE_SIZE (GB2312_TABLE_ROWS * GB2312_TABLE_COLS)

extern const uint16_t gb2312_unicode_table[GB2312_TABLE_SIZE];
