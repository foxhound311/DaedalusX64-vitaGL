/*
Copyright (C) 2001 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "stdafx.h"

#include "RSP_HLE.h"

#include "Interrupt.h"
#include "Memory.h"
#include "Core/ROM.h"
#include "Debug/DBGConsole.h"
#include "Debug/DebugLog.h"
#include "Debug/Dump.h"			// For Dump_GetDumpDirectory()
#include "Math/MathUtil.h"
#include "OSHLE/ultra_mbi.h"
#include "OSHLE/ultra_rcp.h"
#include "OSHLE/ultra_sptask.h"
#include "Plugins/AudioPlugin.h"
#include "Plugins/GraphicsPlugin.h"
#include "Test/BatchTest.h"
#include "Utility/IO.h"
#include "Utility/PrintOpCode.h"
#include "Utility/Profiler.h"

/* JpegTask.cpp */
extern void jpeg_decode_PS(OSTask *task);
extern void jpeg_decode_PS0(OSTask *task);
extern void jpeg_decode_OB(OSTask *task);

/* RE2Task.cpp + HqvmTask.cpp */
extern "C" {
	extern void resize_bilinear_task(OSTask *task);
	extern void decode_video_frame_task(OSTask *task);
	extern void fill_video_double_buffer_task(OSTask *task);
	extern void hvqm2_decode_sp1_task(OSTask *task);
};

#ifdef DAEDALUS_DEBUG_CONSOLE
#if 0
static void RDP_DumpRSPCode(char * name, u32 crc, u32 * mem_base, u32 pc_base, u32 len)
{
	char filename[100];
	sprintf(filename, "task_dump_%s_crc_0x%08x.txt", name, crc);

	IO::Filename filepath;
	Dump_GetDumpDirectory(filepath, "rsp_dumps");
	IO::Path::Append(filepath, filename);

	FILE * fp = fopen(filepath, "w");
	if (fp == nullptr)
		return;

	for (u32 i = 0; i < len; i+=4)
	{
		OpCode op;
		u32 pc = i & 0x0FFF;
		op._u32 = mem_base[i/4];

		char opinfo[400];
		SprintRSPOpCodeInfo( opinfo, pc + pc_base, op );

		fprintf(fp, "0x%08x: <0x%08x> %s\n", pc + pc_base, op._u32, opinfo);
		//fprintf(fp, "<0x%08x>\n", dwOpCode);
	}

	fclose(fp);
}
#endif

#if 0
static void RDP_DumpRSPData(char * name, u32 crc, u32 * mem_base, u32 pc_base, u32 len)
{
	char filename[100];
	sprintf(filename, "task_data_dump_%s_crc_0x%08x.txt", name, crc);

	IO::Filename filepath;
	Dump_GetDumpDirectory(filepath, "rsp_dumps");
	IO::Path::Append(filepath, filename);

	FILE * fp = fopen(filepath, "w");
	if (fp == nullptr)
		return;

	for (u32 i = 0; i < len; i+=4)
	{
		u32 pc = i & 0x0FFF;
		u32 data = mem_base[i/4];

		fprintf(fp, "0x%08x: 0x%08x\n", pc + pc_base, data);
	}

	fclose(fp);
}
#endif


//

#if 0
static void	RSP_HLE_DumpTaskInfo( const OSTask * pTask )
{
	DBGConsole_Msg(0, "DP: Task:%08x Flags:%08x BootCode:%08x BootCodeSize:%08x",
		pTask->t.type, pTask->t.flags, pTask->t.ucode_boot, pTask->t.ucode_boot_size);

	DBGConsole_Msg(0, "DP: uCode:%08x uCodeSize:%08x uCodeData:%08x uCodeDataSize:%08x",
		pTask->t.ucode, pTask->t.ucode_size, pTask->t.ucode_data, pTask->t.ucode_data_size);

	DBGConsole_Msg(0, "DP: Stack:%08x StackS:%08x Output:%08x OutputS:%08x",
		pTask->t.dram_stack, pTask->t.dram_stack_size, pTask->t.output_buff, pTask->t.output_buff_size);

	DBGConsole_Msg(0, "DP: Data(PC):%08x DataSize:%08x YieldData:%08x YieldDataSize:%08x",
		pTask->t.data_ptr, pTask->t.data_size, pTask->t.yield_data_ptr, pTask->t.yield_data_size);
}
#endif

#endif
//

void RSP_HLE_Finished(u32 setbits)
{
	// Need to point to last instr?
	//Memory_DPC_SetRegister(DPC_CURRENT_REG, (u32)pTask->t.data_ptr);

	//
	// Set the SP flags appropriately. The RSP is not running anyway, no need to stop it
	//
	u32 status( Memory_SP_SetRegisterBits(SP_STATUS_REG, setbits) );

	//
	// We've set the SP_STATUS_BROKE flag - better check if it causes an interrupt
	//
	if( status & SP_STATUS_INTR_BREAK )
	{
		Memory_MI_SetRegisterBits(MI_INTR_REG, MI_INTR_SP);
		R4300_Interrupt_UpdateCause3();
	}
}


//

static EProcessResult RSP_HLE_Graphics()
{
	return gGraphicsPlugin->ProcessDList();
}


//

static EProcessResult RSP_HLE_Audio()
{
	return gAudioPlugin->ProcessAList();
}

// RSP_HLE_Jpeg and RSP_HLE_CICX105 were borrowed from Mupen64plus
static u32 sum_bytes(const u8 *bytes, u32 size)
{
    u32 sum = 0;
    const u8 * const bytes_end = bytes + size;

    while (bytes != bytes_end)
        sum += *bytes++;

    return sum;
}

EProcessResult RSP_HLE_Jpeg(OSTask * task)
{
	// most ucode_boot procedure copy 0xf80 bytes of ucode whatever the ucode_size is.
	// For practical purpose we use a ucode_size = min(0xf80, task->ucode_size)
	u32 sum = sum_bytes(g_pu8RamBase + (u32)task->t.ucode , Min<u32>(task->t.ucode_size, 0xf80) >> 1);

	//DBGConsole_Msg(0, "JPEG Task: Sum=0x%08x", sum);
	switch(sum)
	{
	case 0x2c85a: // Pokemon Stadium Jap Exclusive jpg decompression
		jpeg_decode_PS0(task);
		break;
	case 0x2caa6: // Zelda OOT, Pokemon Stadium {1,2} jpg decompression
		jpeg_decode_PS(task);
		break;
	case 0x130de: // Ogre Battle & Buttom of the 9th background decompression
	case 0x278b0:
        jpeg_decode_OB(task);
		break;
	}

	return PR_COMPLETED;
}

EProcessResult RSP_HLE_CICX105(OSTask * task)
{
    const u32 sum = sum_bytes(g_pu8SpImemBase, 0x1000 >> 1);

    switch(sum)
    {
        /* CIC x105 ucode (used during boot of CIC x105 games) */
        case 0x9e2: /* CIC 6105 */
        case 0x9f2: /* CIC 7105 */
			{
				u32 i;
				u8 * dst = g_pu8RamBase + 0x2fb1f0;
				u8 * src = g_pu8SpImemBase + 0x120;

				/* dma_read(0x1120, 0x1e8, 0x1e8) */
				sceClibMemcpy(g_pu8SpImemBase + 0x120, g_pu8RamBase + 0x1e8, 0x1f0);

				/* dma_write(0x1120, 0x2fb1f0, 0xfe817000) */
				for (i = 0; i < 24; ++i)
				{
					sceClibMemcpy(dst, src, 8);
					dst += 0xff0;
					src += 0x8;

				}
			}
			break;

    }

	return PR_COMPLETED;
}

EProcessResult RSP_HLE_RE2(OSTask * task)
{
	u32 sum = sum_bytes(g_pu8RamBase + (u32)task->t.ucode, 256);

	switch(sum)
	{
	case 0x450f:
		resize_bilinear_task(task);
		break;
	case 0x3b44:
		decode_video_frame_task(task);
		break;
	case 0x3d84:
        fill_video_double_buffer_task(task);
		break;
	default:
		DBGConsole_Msg(0, "Unhandled RSP RE2 Task: 0x%04X", sum );
		break;
	}

	return PR_COMPLETED;
}

EProcessResult RSP_HLE_Hvqm(OSTask * task)
{
	hvqm2_decode_sp1_task(task);
	return PR_COMPLETED;
}

void RSP_HLE_ProcessTask()
{	
	OSTask * pTask = (OSTask *)(g_pu8SpMemBase + 0x0FC0);
	
	//DBGConsole_Msg(0, "RSP Task: Type: %ld, Ptr: 0x%08X, Size: 0x%04X", pTask->t.type, (u32*)(pTask->t.data_ptr), pTask->t.ucode_boot_size);

	EProcessResult	result( PR_NOT_STARTED );

	// non task
	if(pTask->t.ucode_boot_size > 0x1000)
	{
		RSP_HLE_CICX105(pTask);
		RSP_HLE_Finished(SP_STATUS_BROKE|SP_STATUS_HALT);
		return;
	}
	
	switch ( pTask->t.type )
	{
		case M_GFXTASK:
			// frozen task
			if(Memory_DPC_GetRegister(DPC_STATUS_REG) & DPC_STATUS_FREEZE)
				return;
			
			if ((u32*)(pTask->t.data_ptr) == nullptr) {
				result = RSP_HLE_RE2(pTask);
			} else if (g_ROM.rh.CartID == 0x4B59) { // Yakouchuu II - Satsujin Kouro
				u32 sum = sum_bytes(g_pu8RamBase + (u32)pTask->t.ucode, 1488);
				if (sum == 0x19495) result = RSP_HLE_Hvqm(pTask);
				else result = RSP_HLE_Graphics();
			} else {
				result = RSP_HLE_Graphics();
			}
			break;
		case M_AUDTASK:
			result = RSP_HLE_Audio();
			break;
		case M_JPGTASK:
			result = RSP_HLE_Jpeg(pTask);
			break;
		case M_FBTASK:
			result = RSP_HLE_Hvqm(pTask);
			break;
		default:
			result = PR_COMPLETED;
			DBGConsole_Msg(0, "Unknown task: %08x", pTask->t.type );
			break;
	}

	// Started and completed. No need to change cores. [synchronously]
	if( result == PR_COMPLETED )
		RSP_HLE_Finished(SP_STATUS_TASKDONE|SP_STATUS_BROKE|SP_STATUS_HALT);
}
