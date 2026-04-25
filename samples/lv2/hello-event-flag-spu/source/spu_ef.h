/*
 *   SCE CONFIDENTIAL                                       
 *   PlayStation(R)3 Programmer Tool Runtime Library 475.001 
 *   Copyright (C) 2008 Sony Computer Entertainment Inc.    
 *   All Rights Reserved.                                   
 */

/*
 * The format of SNRs is the following;
 *       +----+----------------------+----+
 * SNR1: |COM |     reserved         |bitn|
 *       +----+----------------------+----+
 *       +--------------------------------+
 * SNR0: |          event flag            |
 *       +--------------------------------+
 */
#define SPU_COMMAND_END                  1
#define SPU_COMMAND_SET_BIT              2
#define SPU_COMMAND_SET_BIT_IMPATIENT    3
#define SPU_COMMAND_SHIFT               24
#define SPU_BITNUM_MASK              0x3FU
#define MAKE_COMMAND(com, bitn) ((com)<<SPU_COMMAND_SHIFT | (bitn))
#define GET_COMMAND(snr) ((snr)>>SPU_COMMAND_SHIFT)
#define GET_BITNUM(snr) ((snr)&SPU_BITNUM_MASK)
