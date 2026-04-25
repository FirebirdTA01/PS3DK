/*
 *   SCE CONFIDENTIAL                                       
 *   PlayStation(R)3 Programmer Tool Runtime Library 475.001 
 *   Copyright (C) 2008 Sony Computer Entertainment Inc.    
 *   All Rights Reserved.                                   
 */

/*E
 * File: spu_ef.spu.c
 * Description:
 *   This SPU thread set a bit of the event flag.
 *   Note that the event flag ID is passed from a PPU thread via SNR.
 */

#include <spu_intrinsics.h>
#include <sys/spu_thread.h>
#include <sys/event_flag.h>
#include <spu_printf.h>

#include "spu_ef.h"

int main(void)
{
	for (;;) {
		uint32_t snr = spu_readch(SPU_RdSigNotify1);
		int ret;
		sys_event_flag_t ef;
	
		switch(GET_COMMAND(snr)) {
		case SPU_COMMAND_END:
			sys_spu_thread_exit(0);
			break;

		case SPU_COMMAND_SET_BIT:
			ef = spu_readch(SPU_RdSigNotify2);

			/* E
			 * Do any work here.
			 */

			spu_printf("SPU Worker finished my job (bit=%d, ef=0x%llx, tag=%s)\n",
			           (int)GET_BITNUM(snr), (unsigned long long)ef,
			           "done");
			/*E
			 * Step 2. 
			 * Workers have finished their own job. 
			 * They sets a corresponding event by sys_event_flag_set().
			 */
			ret = sys_event_flag_set_bit(ef, GET_BITNUM(snr));
			if (ret != CELL_OK) {
				sys_spu_thread_group_exit(-1);
			}
			break;

		case SPU_COMMAND_SET_BIT_IMPATIENT:
			ef = spu_readch(SPU_RdSigNotify2);
			ret = sys_event_flag_set_bit_impatient(ef, GET_BITNUM(snr));
			if (ret != CELL_OK) {
				sys_spu_thread_group_exit(-1);
			}
			break;

		default:
			sys_spu_thread_group_exit(-1);
			break;
		}
	}

	return 0;
}
