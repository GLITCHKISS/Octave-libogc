/* Octave: libasnd's dsp_mixer.s (libogc v3.0.4) with one more kind of voice: DSP-ADPCM, decoded by the DSP's
   accelerator straight from ARAM (FLAGSL bit 3, VOICE_ADPCM in asndlib.c). Every PCM voice mixes as in the original,
   instruction for instruction in its per-sample path. Changes are marked "Octave:"; the ADPCM voice's code is at the
   end. Uploaded instead of the stock mixer only with Config.ini DspAdpcm=1 (ASND_InitEx).

   An ADPCM voice is the PCM voice it decodes to (mono or stereo 16-bit big-endian), its start/end addresses a range
   that is never read: the sample buffer MEM_SAMP, which a PCM voice fills with a 32-byte DMA, is filled by reading the
   accelerator instead (16 mono samples, or 8 frames of the two channels in turn). So pitch, volume, delay, pause, the
   end and the loop (the second buffer, VOICE_SETLOOP) all work as for PCM. The accelerator state at the start of the
   line in MEM_SAMP goes back to main memory with the voice (its block, the address in the voice's last word), so the
   next mix decodes that line again and goes on exactly where it left off; at a change of buffer it is the loop's.
   Interrupts stay off: the accelerator's end-of-sample exception is never taken (the address compare ends a voice).

DSP_MIXER -> PCM VOICE SOFTWARE PROCESSOR (8-16 Bits Mono/Stereo Voices)

// Thanks to Duddie for you hard work and documentation

Copyright (c) 2008 Hermes <www.entuwii.net>
All rights reserved.

Redistribution and use in source and binary forms, with or without modification, are 
permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice, this list of 
  conditions and the following disclaimer. 
- Redistributions in binary form must reproduce the above copyright notice, this list 
  of conditions and the following disclaimer in the documentation and/or other 
  materials provided with the distribution. 
- The names of the contributors may not be used to endorse or promote products derived 
  from this software without specific prior written permission. 

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY 
EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF 
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL 
THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS 
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, 
STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF 
THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/


/********************************/
/**      REGISTER NAMES        **/
/********************************/

AR0:	equ	0x00	; address registers
AR1:	equ	0x01
AR2:	equ	0x02
AR3:	equ	0x03	// used as jump function selector

IX0:	equ	0x04	// LEFT_VOLUME accel
IX1:	equ	0x05	// RIGHT_VOLUME accel
IX2:	equ	0x06	// ADDRH_SMP accel
IX3:	equ	0x07	// ADDRL_SMP accel

R08:	equ	0x08	// fixed to 48000 value
R09:	equ	0x09	// problems using this
R0A:	equ	0x0a	// ADDREH_SMP accel
R0B:	equ	0x0b	// ADDREL_SMP accel

ST0:	equ	0x0c
ST1:	equ	0x0d
ST2:	equ	0x0e
ST3:	equ	0x0f

CONFIG:	equ	0x12
SR:	equ	0x13

PRODL: equ	0x14
PRODM: equ	0x15
PRODH: equ	0x16
PRODM2: equ	0x17 

AXL0:  equ	0x18
AXL1:  equ	0x19
AXH0:  equ	0x1A	// SMP_R accel
AXH1:  equ	0x1b	// SMP_L accel

ACC0:  equ	0x1c	// accumulator (global)
ACC1:  equ	0x1d

ACL0:  equ	0x1c	// Low accumulator
ACL1:  equ	0x1d
ACM0:  equ	0x1e	// Mid accumulator
ACM1:  equ	0x1f
ACH0:  equ	0x10	// Sign extended 8 bit register 0
ACH1:  equ	0x11	// Sign extended 8 bit register 1

/********************************/
/**  HARDWARE REGISTER ADDRESS **/
/********************************/

DSCR:	equ	0xffc9	; DSP DMA Control Reg
DSBL:	equ	0xffcb	; DSP DMA Block Length
DSPA:	equ	0xffcd	; DSP DMA DMEM Address
DSMAH:	equ	0xffce	; DSP DMA Mem Address H
DSMAL:	equ	0xffcf	; DSP DMA Mem Address L

DIRQ:	equ	0xfffb	; DSP Irq Request
DMBH:	equ	0xfffc	; DSP Mailbox H
DMBL:	equ	0xfffd	; DSP Mailbox L
CMBH:	equ	0xfffe	; CPU Mailbox H
CMBL:	equ	0xffff	; CPU Mailbox L

DMA_TO_DSP:	equ	0
DMA_TO_CPU:	equ	1

// Octave: the accelerator
ACCOEF:	equ	0xffa0	; 16 ADPCM coefficients (8 pairs)
ACFMT:	equ	0xffd1	; format: 0 = 4-bit ADPCM from ARAM
ACSAH:	equ	0xffd4	; start (loop) address, nibbles
ACSAL:	equ	0xffd5
ACEAH:	equ	0xffd6	; end address (the last sample), nibbles
ACEAL:	equ	0xffd7
ACCAH:	equ	0xffd8	; current address, nibbles
ACCAL:	equ	0xffd9
ACPDS:	equ	0xffda	; predictor / scale
ACYN1:	equ	0xffdb	; history
ACYN2:	equ	0xffdc	; (written last: it starts the accelerator again after an end)
ACDAT:	equ	0xffdd	; a decoded sample
ACGAN:	equ	0xffde	; gain (PCM only)

/**************************************************************/
/*                     NUM_SAMPLES SLICE                      */
/**************************************************************/

NUM_SAMPLES:	equ	1024	; 1024 stereo samples 16 bits


/**************************************************************/
/*                SOUND CHANNEL REGS                          */
/**************************************************************/
	
MEM_REG2:	equ	0x0
MEM_VECTH:	equ	MEM_REG2
MEM_VECTL:	equ	MEM_REG2+1
RETURN:		equ	MEM_REG2+2

/**************************************************************/
/*                      CHANNEL DATAS                         */
/**************************************************************/

MEM_REG:	equ	MEM_REG2+0x10

ADDRH_SND:	equ	MEM_REG		// Output buffer
ADDRL_SND:	equ	MEM_REG+1

DELAYH_SND:	equ	MEM_REG+2	// Delay samples High word
DELAYL_SND:	equ	MEM_REG+3	// Delay samples Low word

CHAN_REGS:	equ	MEM_REG+4	// specific regs for the channel

FLAGSH_SMP:	equ	CHAN_REGS+0	// countain number of bytes for step (1-> Mono 8 bits, 2-> Stereo 8 bits and Mono 16 bits, 4-> Stereo 16 bits)	
FLAGSL_SMP:	equ	CHAN_REGS+1	// 0->Mono 8 bits, 1->Stereo 8 bits, 2->Mono 16 bits 3 -> Stereo 16 bits

ADDRH_SMP:	equ	CHAN_REGS+2	// start address 
ADDRL_SMP:	equ	CHAN_REGS+3

ADDREH_SMP:	equ	CHAN_REGS+4	// end address
ADDREL_SMP:	equ	CHAN_REGS+5

FREQH_SMP:	equ	CHAN_REGS+6	// Freq in Hz to play
FREQL_SMP:	equ	CHAN_REGS+7

SMP_L:		equ	CHAN_REGS+8	// last sample for left (used to joint various buffers)
SMP_R:		equ	CHAN_REGS+9	// last sample for right (used to joint various buffers)

COUNTERH_SMP:	equ	CHAN_REGS+10	// pitch counter
COUNTERL_SMP:	equ	CHAN_REGS+11

LEFT_VOLUME:	equ	CHAN_REGS+12	// volume (0 to 255)
RIGHT_VOLUME:	equ	CHAN_REGS+13

ADDR2H_SMP:	equ	CHAN_REGS+14	// start address of buffer two (to joint)
ADDR2L_SMP:	equ	CHAN_REGS+15

ADDR2EH_SMP:	equ	CHAN_REGS+16	// end address of buffer two (to joint)
ADDR2EL_SMP:	equ	CHAN_REGS+17

LEFT_VOLUME2:	equ	CHAN_REGS+18	// volume (0 to 255) for buffer two
RIGHT_VOLUME2:	equ	CHAN_REGS+19

BACKUPH_SMP:	equ	CHAN_REGS+20	// start address backup
BACKUPL_SMP:	equ	CHAN_REGS+21

// Octave: the voice's last word (t_sound_data._pad): a DSP-ADPCM voice's block in main memory
ADPCMH_SMP:	equ	CHAN_REGS+26
ADPCML_SMP:	equ	CHAN_REGS+27

/**************************************************************/
/*               VOICE SAMPLE BUFFER DATAS                    */
/**************************************************************/

MEM_SAMP:	equ	CHAN_REGS+0x20


data_end:	equ	MEM_SAMP+0x20

/**************************************************************/
/*                     SND OUTPUT DATAS                       */
/**************************************************************/

MEM_SND:	equ	data_end ; it need 2048 words (4096 bytes)

/**************************************************************/
/*      Octave: A DSP-ADPCM VOICE'S DATA (after MEM_SND)      */
/**************************************************************/

// The voice's block (asndlib.c t_adpcm_voice), 2 channels of 32 words: +0/1 current address (the start of the line
// in MEM_SAMP), +2/3 end address, +4/5 loop address (nibbles), +6 ps, +7 yn1, +8 yn2 (at that current address),
// +9 loop ps, +10 loop yn1, +11 loop yn2, +16..31 the coefficients
MEM_ADPCM:	equ	0x0860
// The accelerator's state after the line decoded (the next line's start): 2 channels of 8 words (address hi, lo, ps,
// yn1, yn2)
ADPCM_NEXT:	equ	0x08a0
// 1: the line in MEM_SAMP has just been decoded for the address the voice is at (a line crossed in get_sample2, or a
// change of buffer), so the fetch that follows reads it rather than going on to the next
ADPCM_FRESH:	equ	0x08b0

	

/***  START CODE ***/

/**************************************************************/
/*			EXCEPTION TABLE			      */
/**************************************************************/

	jmp	exception0
	jmp	exception1
	jmp	exception2
	jmp	exception3
	jmp	exception4
	jmp	exception5
	jmp	exception6
	jmp	exception7

	lri     $CONFIG, #0xff
	lri	$SR,#0
	s16         
	clr15       
	m0         

/**************************************************************/
/*                            main                            */
/**************************************************************/

main:
	
// send init token to CPU

	si		@DMBH, #0xdcd1
	si		@DMBL, #0x0000
	si		@DIRQ, #1

recv_cmd:
// check if previous mail is received from the CPU

	call	wait_for_dsp_mail
	
// wait a mail from CPU

	call	wait_for_cpu_mail

	si		@DMBH, #0xdcd1

	clr		$ACC0
	lri		$ACM0,#0xcdd1
	cmp
	jeq		sys_command
	
	clr	$ACC1
	lrs	$ACM1, @CMBL

	cmpi    $ACM1, #0x111  // fill the internal sample buffer and process the voice internally
	jeq	input_samples

	cmpi    $ACM1, #0x112  // get samples from the external buffer to the internal buffer and process the voice mixing the samples internally
	jeq	input_samples2

	cmpi    $ACM1, #0x123  // get the address of the voice datas buffer (CHANNEL DATAS)
	jeq	get_data_addr 

	cmpi    $ACM1, #0x222  // process the voice mixing the samples internally
	jeq	input_next_samples

	cmpi    $ACM1, #0x666  // send the samples for the internal buffer to the external buffer
	jeq	send_samples

	cmpi    $ACM1, #0x777   // special: to dump the IROM Datas (remember disable others functions from the interrupt vector to use)
	jeq	rom_dump_word   // (CMBH+0x8000) countain the address of IROM

	cmpi    $ACM1, #0x888	// Used for test
	jeq	polla_loca 

	cmpi	$ACM1, #0x999
	jeq	task_terminate
	
	si	@DMBL, #0x0004	// return 0 as ignore command
	si	@DIRQ, #0x1 // set the interrupt
	jmp	recv_cmd

task_terminate:
	si	@DMBL, #0x0003
	si	@DIRQ, #0x1
	jmp	recv_cmd
	
sys_command:
	clr	$ACC1
	lrs	$ACM1, @CMBL
	
	cmpi	$ACM1,#0x0001
	jeq		run_nexttask
	
	cmpi	$ACM1,#0x0002
	jeq		0x8000
	
	jmp		recv_cmd
	
run_nexttask:
	s16
	call		wait_for_cpu_mail
	lrs			$29,@CMBL
	call		wait_for_cpu_mail
	lrs			$29,@CMBL
	call		wait_for_cpu_mail
	lrs			$29,@CMBL
	call		wait_for_cpu_mail
	lr			$5,@CMBL
	andi		$31,#0x0fff
	mrr			$4,$31
	call		wait_for_cpu_mail
	lr			$7,@CMBL
	call		wait_for_cpu_mail
	lr			$6,@CMBL
	call		wait_for_cpu_mail
	lr			$0,@CMBL
	call		wait_for_cpu_mail
	lrs			$24,@CMBL
	andi		$31,#0x0fff
	mrr			$26,$31
	call		wait_for_cpu_mail
	lrs			$25,@CMBL
	call		wait_for_cpu_mail
	lrs			$27,@CMBL
	sbclr		#0x05
	sbclr		#0x06
	jmp			0x80b5		
	halt

/**************************************************************************************************************************************/
// send the samples for the internal buffer to the external buffer

send_samples:

	lri	$AR0, #MEM_SND
	lris	$AXL1, #DMA_TO_CPU;
	lri	$AXL0, #NUM_SAMPLES*4 ; len
	lr	$ACM0, @ADDRH_SND
    lr	$ACL0, @ADDRL_SND
	
	call	do_dma
	si	@DMBL, #0x0004
	si	@DIRQ, #0x1 // set the interrupt
	jmp	recv_cmd

/**************************************************************************************************************************************/
// get the address of the voice datas buffer (CHANNEL DATAS)

get_data_addr:
	call	wait_for_cpu_mail

	lrs	$ACM0, @CMBH
	lr	$ACL0, @CMBL
     
	sr	@MEM_VECTH, $ACM0
	sr	@MEM_VECTL, $ACL0
	
	si	@DIRQ, #0x0 // clear the interrupt
	jmp	recv_cmd

/**************************************************************************************************************************************/
// fill the internal sample buffer and process the voice internally

input_samples:

	clr	$ACC0
	lr	$ACM0, @MEM_VECTH
	lr	$ACL0, @MEM_VECTL

	lris	$AXL0, #0x0004 
	sr	@RETURN, $AXL0
	si	@DIRQ, #0x0000

        // program DMA to get datas

	lri	$AR0, #MEM_REG
	lris	$AXL1, #DMA_TO_DSP
	lris	$AXL0, #64 ; len

	call	do_dma

	lri	$AR1, #MEM_SND
	lri	$ACL1, #0;

	lri	$AXL0, #NUM_SAMPLES
	bloop	$AXL0, loop_get1

	srri	@$AR1, $ACL1
	srri	@$AR1, $ACL1

loop_get1:
	nop

	lr	$ACM0, @ADDRH_SND
        lr	$ACL0, @ADDRL_SND
	jmp	start_main

/**************************************************************************************************************************************/
// get samples from the external buffer to the internal buffer and process the voice mixing the samples internally

input_samples2:

	clr	$ACC0
	lr	$ACM0, @MEM_VECTH
	lr	$ACL0, @MEM_VECTL

	lris	$AXL0, #0x0004 
	sr	@RETURN, $AXL0
	si	@DIRQ, #0x0000

        // program DMA to get datas

	lri	$AR0, #MEM_REG
	lri	$AXL1, #DMA_TO_DSP
	lris	$AXL0, #64 ; len

	call	do_dma

	lr	$ACM0, @ADDRH_SND
        lr	$ACL0, @ADDRL_SND

	lri	$AR0, #MEM_SND
	lris	$AXL1, #DMA_TO_DSP;
	lri	$AXL0, #NUM_SAMPLES*4; len
	
	call	do_dma

	jmp	start_main

/**************************************************************************************************************************************/
// process the voice mixing the samples internally

input_next_samples:
	
	clr	$ACC0
	lr	$ACM0, @MEM_VECTH
	lr	$ACL0, @MEM_VECTL

	lris	$AXL0, #0x0004 
	sr	@RETURN, $AXL0
	si	@DIRQ, #0x0000

        // program DMA to get datas

	lri	$AR0, #MEM_REG
	lris	$AXL1, #DMA_TO_DSP
	lris	$AXL0, #64 ; len

	call	do_dma

/**************************************************************************************************************************************/
// mixing and control pitch to create 1024 Stereo Samples at 16 bits from here 

start_main:
	
// Octave: a DSP-ADPCM voice's block in from main memory
	
	lr	$ACM1, @FLAGSL_SMP
	andcf	$ACM1, #0x8
	calllz	adpcm_block_in
	
	lri	$R08, #48000

	// load the previous samples used

	lr	$AXH0, @SMP_R
        lr	$AXH1, @SMP_L

// optimize the jump function to get MONO/STEREO 8/16 bits samples

	lr	$ACM1, @FLAGSL_SMP
	andi	$ACM1, #0x7
	addi	$ACM1, #sample_selector
	mrr	$AR3, $ACM1
	ilrr	$ACM1, @$AR3
	mrr	$AR3, $ACM1 // AR3 countain the jump loaded from sample selector

	clr	$ACC0

// test for channel paused

	lr	$ACM0, @FLAGSL_SMP 
	andcf	$ACM0, #0x200
	jlz	end_main
	
// load the sample address

	lr	$ACM0, @ADDRH_SMP
        lr	$ACL0, @ADDRL_SMP

// test if ADDR_SMP & ADDR2H_SMP are zero
	
	tst	$ACC0
	jne	do_not_change1

// set return as "change of buffer"

	lris	$AXL0, #0x0004 
	sr	@RETURN, $AXL0

// change to buffer 2 if it is possible

	call	change_buffer

// stops if again 0 address

	tst	$ACC0
	jeq	save_datas_end

do_not_change1:


// backup the external sample address
	
	mrr	$IX2, $ACM0
	mrr	$IX3, $ACL0
	

// load the counter pitch

	//lr	$r08, @COUNTERH_SMP
	//lr	$r09, @COUNTERL_SMP

// load the end address of the samples

	lr	$r0a, @ADDREH_SMP
        lr	$r0b, @ADDREL_SMP

// load AR1 with internal buffer address

	lri	$AR1, #MEM_SND

/////////////////////////////////////
// delay time section
/////////////////////////////////////

// load AXL0 with the samples to be processed

	lri	$AXL0, #NUM_SAMPLES

// test if DELAY == 0 and skip or not

	clr	$ACC0
	clr	$ACC1
	lr	$ACH0, @DELAYH_SND
        lr	$ACM0, @DELAYL_SND
	tst	$ACC0
	jeq	no_delay

// samples left and right to 0	

	lris	$AXH0, #0
	lris	$AXH1, #0

// load the samples to be processed in ACM1

	mrr	$ACM1, $AXL0
l_delay:
	iar	$AR1 // skip two samples
	iar	$AR1 
	decm	$ACM1
	jeq	exit_delay1 // exit1 if samples to be processed == 0

	decm	$ACM0
	jeq	exit_delay2 // exit2 if delay time == 0
	jmp	l_delay

// store the remanent delay and ends

exit_delay1:
	decm	$ACM0
	
	sr	@DELAYH_SND, $ACH0
        sr	@DELAYL_SND, $ACM0
	
	lris	$AXL0,#0 ; exit from loop
	
	jmp	no_delay 
	

exit_delay2:

	// store delay=0 and continue

	sr	@DELAYH_SND, $ACH0
        sr	@DELAYL_SND, $ACM0
	mrr	$AXL0, $ACL1 // load remanent samples to be processed in AXL0

no_delay:

/////////////////////////////////////
// end of delay time section
/////////////////////////////////////

/* bucle de generacion de samples */

	
// load the sample buffer with address aligned to 32 bytes blocks (first time)
			
	si	@DSCR, #DMA_TO_DSP // very important!: load_smp_addr_align and jump_load_smp_addr need fix this DMA Register (I gain some cycles so)

// load_smp_addr_align input:  $IX2:$IX3

// Octave: a DSP-ADPCM voice's line decoded instead

	lr	$ACM0, @FLAGSL_SMP
	andcf	$ACM0, #0x8
	jlz	adpcm_first_line

	call	load_smp_addr_align

first_line_done:

// load the volume registers

	lr	$IX0, @LEFT_VOLUME
	lr	$IX1, @RIGHT_VOLUME
	
// test the freq value

	clr	$ACC0
	lr	$ACH0, @FREQH_SMP
	lr	$ACM0, @FREQL_SMP
	
	clr	$ACC1
	;lri	$ACM1,#48000
	mrr	$ACM1, $R08
	cmp
	
// select the output of the routine to process stereo-mono 8/16bits samples
	
	lri	$AR0, #get_sample // fast method <=48000	

// if  number is greater freq>48000 fix different routine 
	
	ifgt	
	lri	$AR0, #get_sample2 // slow method >48000

// Octave: a DSP-ADPCM voice's own copies of them

	lr	$ACM1, @FLAGSL_SMP
	andcf	$ACM1, #0x8
	jlnz	not_adpcm_voice
	lri	$AR0, #adpcm_get_sample
	mrr	$ACM1, $R08
	cmp
	ifgt	
	lri	$AR0, #adpcm_get_sample2
not_adpcm_voice:

// loops for samples to be processed

	bloop	$AXL0, loop_end

	//srri	@$AR1, $AXH0 // put sample R
	//srri	@$AR1, $AXH1 // put sample L

// Mix right sample section

	lrr	$ACL0, @$AR1 // load in ACL0 the  right sample from the internal buffer 
	movax	$ACC1, $ACX1 // big trick :)  load the current sample <<16 and sign extended

	asl	$ACC0,#24    // convert sample from buffer to 24 bit number with sign extended (ACH0:ACM0)
	asr	$ACC0,#8
	
	add	$ACC0,$ACC1  // current_sample+buffer sample

	cmpi	$ACM0,#32767 // limit to 32767
	jle	right_skip

	lri	$ACM0, #32767
	jmp	right_skip2

right_skip:

	cmpi	$ACM0,#-32768 // limit to -32768
	ifle
	lri	$ACM0, #-32768
	
right_skip2:	

	srri	@$AR1, $ACM0 // store the right sample mixed to the internal buffer and increment AR1

// Mix left sample section

	lrr	$ACL0, @$AR1 // load in ACL0 the left sample from the internal buffer 

	movax	$ACC1, $ACX0 // big trick :)  load the current sample <<16 and sign extended
	
	asl	$ACC0, #24   // convert sample from buffer to 24 bit number with sign extended (ACH0:ACM0)
	asr	$ACC0, #8
	
	add	$ACC0, $ACC1 // current_sample+buffer sample

	cmpi	$ACM0,#32767 // limit to 32767
	jle	left_skip

	lri	$ACM0, #32767
	jmp	left_skip2

left_skip:

	cmpi	$ACM0,#-32768 // limit to -32768
	ifle
	lri	$ACM0, #-32768
	
left_skip2:	

	srri	@$AR1, $ACM0 // store the left sample mixed to the internal buffer and increment AR1
	
// adds the counter with the voice frequency and test if it >=48000 to get the next sample

	clr	$ACC1
	lr	$ACH1, @COUNTERH_SMP
	lr	$ACM1, @COUNTERL_SMP
	clr	$ACC0
	lr	$ACH0, @FREQH_SMP
	lr	$ACM0, @FREQL_SMP
	
	add	$ACC1,$ACC0
	clr	$ACC0
	//lri	$ACM0,#48000
	mrr	$ACM0, $R08

	cmp

	jrnc	$AR0 //get_sample or get_sample2 method

	sr	@COUNTERH_SMP, $ACH1 
        sr	@COUNTERL_SMP, $ACM1

	jmp	loop_end

// get a new sample for freq > 48000 Hz

get_sample2: // slow method

	sub	$ACC1,$ACC0 // restore the counter

// restore the external sample buffer address

	clr	$ACC0
	mrr	$ACM0, $IX2 // load ADDRH_SMP
	mrr	$ACL0, $IX3 // load ADDRL_SMP

	lr	$AXL1, @FLAGSH_SMP // add the step to get the next samples
	addaxl  $ACC0, $AXL1

	mrr	$IX2, $ACM0 // store ADDRH_SMP
	mrr	$IX3, $ACL0 // store ADDRL_SMP

	mrr	$ACM0, $ACL0
	andf	$ACM0, #0x1f

// load_smp_addr_align input:  $IX2:$IX3 call if (ACM0 & 0x1f)==0

	calllz	load_smp_addr_align

	clr	$ACC0
	//lri	$ACM0,#48000
	mrr	$ACM0, $R08

	cmp

	jle	get_sample2
	
	sr	@COUNTERH_SMP, $ACH1 
        sr	@COUNTERL_SMP, $ACM1

	mrr	$ACM0, $IX2 // load ADDRH_SMP
	mrr	$ACL0, $IX3 // load ADDRL_SMP

	clr	$ACC1
	mrr	$ACM1, $r0a // load ADDREH_SMP
	mrr	$ACL1, $r0b // load ADDREL_SMP

// compares if the current address is >= end address to change the buffer or stops

	cmp

// if addr>addr end get a new buffer (if you uses double buffer)

	jc	get_new_buffer

// load samples from dma, return $ar2 with the addr to get the samples and return using $ar0 to the routine to process 8-16bits Mono/Stereo

	jmp	jump_load_smp_addr 

// get a new sample for freq <= 48000 Hz

get_sample: // fast method
	
	sub	$ACC1,$ACC0 // restore the counter
	sr	@COUNTERH_SMP, $ACH1 
        sr	@COUNTERL_SMP, $ACM1

// restore the external sample buffer address

	clr	$ACC0
	mrr	$ACM0, $IX2 // load ADDRH_SMP
	mrr	$ACL0, $IX3 // load ADDRL_SMP

	lr	$AXL1, @FLAGSH_SMP // add the step to get the next samples
	addaxl  $ACC0, $AXL1
	
	clr	$ACC1
	mrr	$ACM1, $r0a // load ADDREH_SMP
	mrr	$ACL1, $r0b // load ADDREL_SMP

// compares if the current address is >= end address to change the buffer or stops

	cmp
	jc	get_new_buffer

// load the new sample from the buffer

	mrr	$IX2, $ACM0 // store ADDRH_SMP
	mrr	$IX3, $ACL0 // store ADDRL_SMP

// load samples from dma, return $ar2 with the addr and return using $ar0 to the routine to process 8-16bits Mono/Stereo or addr_get_sample_again

	jmp	jump_load_smp_addr 

sample_selector:
	cw	mono_8bits 
	cw	mono_16bits 
	cw	stereo_8bits 
	cw	stereo_16bits 
	cw	mono_8bits_unsigned
	cw	mono_16bits_le
	cw	stereo_8bits_unsigned
	cw	stereo_16bits_le

get_new_buffer:

// set return as "change of buffer": it need to change the sample address

	lris	$AXL0, #0x0004 
	sr	@RETURN, $AXL0
	
	call	change_buffer // load add from addr2

// addr is 0 ? go to zero_samples and exit

	tst	$acc0
	jeq	zero_samples

// load_smp_addr_align input:  $IX2:$IX3

	call	load_smp_addr_align // force the load the samples cached (address aligned)

// jump_load_smp_addr:  $IX2:$IX3
// load samples from dma, return $ar2 with the addr to get the samples and return using $ar0 to the routine to process 8-16bits Mono/Stereo

	jmp	jump_load_smp_addr 

// set to 0 the current samples 

zero_samples:

	lris $AXH0, #0
	lris $AXH1, #0
	jmp	out_samp

mono_8bits:
	
//  8 bits mono
	mrr  $ACM1, $IX3
	lrri $ACL0, @$AR2
	andf $ACM1, #0x1

	iflz // obtain sample0-sample1 from 8bits packet
	lsr  $ACC0, #8
	lsl  $ACC0, #8

	mrr $AXH1,$ACL0
	mrr $AXH0,$ACL0
	jmp	out_samp

stereo_8bits:	

// 8 bits stereo

	lrri $ACL0, @$AR2
	mrr  $ACM0, $ACL0
	andi $ACM0, #0xff00
	mrr  $AXH0, $ACM0
	lsl  $ACC0, #8

	mrr  $AXH1, $ACL0
	jmp	out_samp

mono_16bits:

// 16 bits mono

	lrri $AXH1, @$AR2
	mrr  $AXH0,$AXH1
	jmp	out_samp

stereo_16bits:

// 16 bits stereo

	lrri $AXH0, @$AR2
	lrri $AXH1, @$AR2
	jmp	out_samp

mono_8bits_unsigned:

//  8 bits mono unsigned
	mrr  $ACM1, $IX3
	lrri $ACL0, @$AR2
	andf $ACM1, #0x1

	iflz // obtain sample0-sample1 from 8bits packet
	lsr  $ACL0, #8
	lsl  $ACC0, #24
	xori $ACM0, #0x8000 // convert unsigned->signed

	mrr  $AXH1,$ACM0
	mrr  $AXH0,$ACM0
	jmp	out_samp

mono_16bits_le:

// 16 bits mono little-endian

	lrri $ACM0, @$AR2
	mrr  $ACL0, $ACM0
	lsl  $ACC0, #8 // byteswap

	mrr  $AXH1, $ACM0
	mrr  $AXH0, $ACM0
	jmp	out_samp

stereo_8bits_unsigned:

// 8 bits stereo unsigned

	lrri $ACM0, @$AR2
	xori $ACM0, #0x8080 // convert unsigned->signed
	mrr  $ACL0, $ACM0
	andi $ACM0, #0xff00
	mrr  $AXH0, $ACM0
	lsl  $ACC0, #8

	mrr  $AXH1, $ACL0
	jmp	out_samp

stereo_16bits_le:

// 16 bits stereo little-endian
	lrri $ACM0, @$AR2
	lrri $ACM1, @$AR2
	mrr  $ACL1, $ACM1
	mrr  $ACL0, $ACM0
	lsl  $ACC1, #8 // byteswap
	lsl  $ACC0, #8
	
	mrr  $AXH1, $ACM1
	mrr  $AXH0, $ACM0
	//jmp	out_samp

out_samp:	
	
// multiply sample x volume

	//   LEFT_VOLUME
	mrr	$AXL0,$IX0
	mul	$AXL0,$AXH0
	movp	$ACC0
	asr	$ACC0,#8
	mrr	$AXH0, $ACL0

        // RIGHT VOLUME
	mrr	$AXL1,$IX1
	mul	$AXL1,$AXH1
	movp	$ACC0
	asr	$ACC0,#8
	mrr	$AXH1, $ACL0

loop_end:
	nop

end_process:
	
	// load the sample address

	clr	$ACC0
	mrr	$ACM0, $IX2
        mrr	$ACL0, $IX3

	tst	$ACC0
	jne	save_datas_end

// set return as "change of buffer"

	lris	$AXL0, #0x0004 
	sr	@RETURN, $AXL0

// change to buffer 2 if it is possible

	call	change_buffer

save_datas_end:
	
	sr	@ADDRH_SMP, $IX2
        sr	@ADDRL_SMP, $IX3
	sr	@SMP_R, $AXH0 
        sr	@SMP_L, $AXH1
	
end_main:	

// program DMA to send the CHANNEL DATAS changed

	clr	$ACC0
	lr	$ACM0, @MEM_VECTH
	lr	$ACL0, @MEM_VECTL

	lri	$AR0, #MEM_REG
	lris	$AXL1, #DMA_TO_CPU
	lris	$AXL0, #64 ; len

	call	do_dma

// Octave: a DSP-ADPCM voice's block back to main memory

	lr	$ACM1, @FLAGSL_SMP
	andcf	$ACM1, #0x8
	calllz	adpcm_block_out

	si	@DMBH, #0xdcd1
	lr	$ACL0, @RETURN

	sr	@DMBL, $ACL0
	si	@DIRQ, #0x1 // set the interrupt

	jmp	recv_cmd

change_buffer:
	
	clr	$ACC0
	lr	$ACM0, @LEFT_VOLUME2
        lr	$ACL0, @RIGHT_VOLUME2
	sr	@LEFT_VOLUME, $ACM0 
        sr	@RIGHT_VOLUME, $ACL0
	mrr	$IX0, $ACM0 
	mrr	$IX1, $ACL0

	lr	$ACM0, @ADDR2EH_SMP
        lr	$ACL0, @ADDR2EL_SMP
	sr	@ADDREH_SMP, $ACM0 
        sr	@ADDREL_SMP, $ACL0
	mrr	$r0a, $ACM0
	mrr	$r0b, $ACL0

	lr	$ACM0, @ADDR2H_SMP
        lr	$ACL0, @ADDR2L_SMP
	sr	@ADDRH_SMP, $ACM0 
        sr	@ADDRL_SMP, $ACL0
	sr	@BACKUPH_SMP, $ACM0 
        sr	@BACKUPL_SMP, $ACL0
	mrr	$IX2, $ACM0
	mrr	$IX3, $ACL0
	
// Octave: a DSP-ADPCM voice's accelerator state goes back to its loop's, as its address does

	lr	$ACM1, @FLAGSL_SMP
	andcf	$ACM1, #0x8
	calllz	adpcm_loop_state

// check loop flag	
	lr	$ACM1, @FLAGSL_SMP
	andcf	$ACM1, #0x100
	retlz
        
	sr	@ADDR2H_SMP, $ACH0
	sr	@ADDR2L_SMP, $ACH0
	sr	@ADDR2EH_SMP, $ACH0
	sr	@ADDR2EL_SMP, $ACH0
	ret

/**************************************************************/
/*                        DMA ROUTINE                         */
/**************************************************************/

do_dma:

	sr	@DSMAH, $ACM0
	sr	@DSMAL, $ACL0
	sr	@DSPA, $AR0
	sr	@DSCR, $AXL1
	sr	@DSBL, $AXL0

wait_dma:

	lrs	$ACM1, @DSCR
	andcf	$ACM1, #0x4
	jlz	wait_dma
	ret


wait_for_dsp_mail:

	lrs	$ACM1, @DMBH
	andcf	$ACM1, #0x8000
	jlz	wait_for_dsp_mail
	ret

wait_for_cpu_mail:

	lrs	$ACM1, @cmbh
	andcf	$ACM1, #0x8000
	jlnz	wait_for_cpu_mail
	ret

load_smp_addr_align:

	mrr	$ACL0, $IX3  // load ADDRL_SMP

	lsr	$ACC0, #5
	lsl	$ACC0, #5
	sr	@DSMAH, $IX2
	sr	@DSMAL, $ACL0
	si	@DSPA, #MEM_SAMP
	;si	@DSCR, #DMA_TO_DSP
	si	@DSBL, #0x20

wait_dma1:
	lrs	$ACM0, @DSCR
	andcf	$ACM0, #0x4
	jlz	wait_dma1

	lri	$AR2, #MEM_SAMP
	ret


//////////////////////////////////////////

jump_load_smp_addr:

	mrr	$ACM0, $IX3  // load ADDRL_SMP
	asr	$ACC0, #1
	andi	$ACM0, #0xf
	jeq	jump_load_smp_dma
	
	addi	$ACM0, #MEM_SAMP
	mrr	$AR2, $ACM0
	jmpr	$AR3

jump_load_smp_dma:

	sr	@DSMAH, $IX2
	sr	@DSMAL, $IX3
	si	@DSPA, #MEM_SAMP
	;si	@DSCR, #DMA_TO_DSP // to gain some cycles
	si	@DSBL, #0x20

wait_dma2:
	lrs	$ACM0, @DSCR
	andcf	$ACM0, #0x4
	jlz	wait_dma2
	
	lri	$AR2, #MEM_SAMP
	jmpr	$AR3
		

/**************************************************************/
/*            Octave: THE DSP-ADPCM VOICE                     */
/**************************************************************/
// The accelerator decodes Nintendo's DSP-ADPCM (8-byte frames: a predictor/scale byte, 14 4-bit samples) from ARAM,
// one sample a read of ACDAT. Its state (address, ps, history) is the voice's, saved and loaded for every line, so
// each channel, and each voice, has it to itself. The register order follows AX: the format first, the history last.
// The code below borrows the shape of libansnd's accelerator set-up (Zlib licence):
//   libansnd -- Copyright (c) 2025 James Sawyer. This software is provided 'as-is', without any express or implied
//   warranty. In no event will the authors be held liable for any damages arising from the use of this software.
//   Permission is granted to anyone to use this software for any purpose, including commercial applications, and to
//   alter it and redistribute it freely, subject to the following restrictions: 1. The origin of this software must
//   not be misrepresented; you must not claim that you wrote the original software. If you use this software in a
//   product, an acknowledgment in the product documentation would be appreciated but is not required. 2. Altered
//   source versions must be plainly marked as such, and must not be misrepresented as being the original software.
//   3. This notice may not be removed or altered from any source distribution.
// Registers: these use $ACC0 (and $AXL1, $AR2 where said) only; the mixer's others are kept.

// The voice's block in (start_main) and out (end_main): 128 bytes at the address in its last word
adpcm_block_in:
	clr	$ACC0
	lr	$ACM0, @ADPCMH_SMP
	lr	$ACL0, @ADPCML_SMP
	lri	$AR0, #MEM_ADPCM
	lris	$AXL1, #DMA_TO_DSP
	lri	$AXL0, #128
	jmp	do_dma

adpcm_block_out:
	clr	$ACC0
	lr	$ACM0, @ADPCMH_SMP
	lr	$ACL0, @ADPCML_SMP
	lri	$AR0, #MEM_ADPCM
	lris	$AXL1, #DMA_TO_CPU
	lri	$AXL0, #128
	jmp	do_dma

// The first line of a mix (at the block start, the voice's state as saved): decoded, not fresh -- the first fetch
// steps past the current sample, so landing on the next line it goes on to it
adpcm_first_line:
	call	adpcm_decode
	lris	$AXL1, #0
	sr	@ADPCM_FRESH, $AXL1
	lri	$AR2, #MEM_SAMP
	jmp	first_line_done

// get_sample2's (pitch over 48 kHz): onto the next line as it crosses it (keeps $ACC1, the counter)
adpcm_line_next:
	call	adpcm_next_line
	lris	$AXL1, #1
	sr	@ADPCM_FRESH, $AXL1
	ret

// After a change of buffer (the loop's state set by adpcm_loop_state): its first line
adpcm_line_reload:
	call	adpcm_decode
	lris	$AXL1, #1
	sr	@ADPCM_FRESH, $AXL1
	ret

// change_buffer's: the loop's state for the line the address goes back to (none: the voice has ended). Keeps $ACC0
// (the new address) and $AXH0/$AXH1; uses $AXL1
adpcm_loop_state:
	tst	$ACC0
	reteq
	lr	$AXL1, @0x0864
	sr	@0x0860, $AXL1
	lr	$AXL1, @0x0865
	sr	@0x0861, $AXL1
	lr	$AXL1, @0x0869
	sr	@0x0866, $AXL1
	lr	$AXL1, @0x086a
	sr	@0x0867, $AXL1
	lr	$AXL1, @0x086b
	sr	@0x0868, $AXL1
	lr	$AXL1, @0x0884
	sr	@0x0880, $AXL1
	lr	$AXL1, @0x0885
	sr	@0x0881, $AXL1
	lr	$AXL1, @0x0889
	sr	@0x0886, $AXL1
	lr	$AXL1, @0x088a
	sr	@0x0887, $AXL1
	lr	$AXL1, @0x088b
	sr	@0x0888, $AXL1
	ret

// jump_load_smp_addr's: the sample at $IX2:$IX3 from MEM_SAMP, a new line decoded when the address has come onto one
// (unless it already was: ADPCM_FRESH); then to the format's routine ($AR3), as for PCM
adpcm_jump_load:
	mrr	$ACM0, $IX3
	asr	$ACC0, #1
	andi	$ACM0, #0xf
	jne	adpcm_jump_mid
	lr	$ACM0, @ADPCM_FRESH
	andf	$ACM0, #0x1
	jlnz	adpcm_jump_fresh
	call	adpcm_next_line
adpcm_jump_fresh:
	lris	$AXL1, #0
	sr	@ADPCM_FRESH, $AXL1
	lri	$AR2, #MEM_SAMP
	jmpr	$AR3
adpcm_jump_mid:
	addi	$ACM0, #MEM_SAMP
	mrr	$AR2, $ACM0
	lris	$AXL1, #0
	sr	@ADPCM_FRESH, $AXL1
	jmpr	$AR3

// get_sample, get_sample2 and get_new_buffer as an ADPCM voice has them: the same, with its line routines
adpcm_get_sample2: // slow method
	sub	$ACC1,$ACC0 // restore the counter
	clr	$ACC0
	mrr	$ACM0, $IX2 // load ADDRH_SMP
	mrr	$ACL0, $IX3 // load ADDRL_SMP
	lr	$AXL1, @FLAGSH_SMP // add the step to get the next samples
	addaxl  $ACC0, $AXL1
	mrr	$IX2, $ACM0 // store ADDRH_SMP
	mrr	$IX3, $ACL0 // store ADDRL_SMP
	mrr	$ACM0, $ACL0
	andf	$ACM0, #0x1f
	calllz	adpcm_line_next
	clr	$ACC0
	mrr	$ACM0, $R08
	cmp
	jle	adpcm_get_sample2
	sr	@COUNTERH_SMP, $ACH1
	sr	@COUNTERL_SMP, $ACM1
	mrr	$ACM0, $IX2 // load ADDRH_SMP
	mrr	$ACL0, $IX3 // load ADDRL_SMP
	clr	$ACC1
	mrr	$ACM1, $r0a // load ADDREH_SMP
	mrr	$ACL1, $r0b // load ADDREL_SMP
	cmp
	jc	adpcm_get_new_buffer
	jmp	adpcm_jump_load

adpcm_get_sample: // fast method
	sub	$ACC1,$ACC0 // restore the counter
	sr	@COUNTERH_SMP, $ACH1
	sr	@COUNTERL_SMP, $ACM1
	clr	$ACC0
	mrr	$ACM0, $IX2 // load ADDRH_SMP
	mrr	$ACL0, $IX3 // load ADDRL_SMP
	lr	$AXL1, @FLAGSH_SMP // add the step to get the next samples
	addaxl  $ACC0, $AXL1
	clr	$ACC1
	mrr	$ACM1, $r0a // load ADDREH_SMP
	mrr	$ACL1, $r0b // load ADDREL_SMP
	cmp
	jc	adpcm_get_new_buffer
	mrr	$IX2, $ACM0 // store ADDRH_SMP
	mrr	$IX3, $ACL0 // store ADDRL_SMP
	jmp	adpcm_jump_load

adpcm_get_new_buffer:
	lris	$AXL0, #0x0004
	sr	@RETURN, $AXL0
	call	change_buffer // load add from addr2 (and the loop's state: adpcm_loop_state)
	tst	$acc0
	jeq	zero_samples
	call	adpcm_line_reload
	jmp	adpcm_jump_load

// The next line: the state the last line left (ADPCM_NEXT) made the voice's, then decoded
adpcm_next_line:
	lr	$ACM0, @0x08a0
	sr	@0x0860, $ACM0
	lr	$ACM0, @0x08a1
	sr	@0x0861, $ACM0
	lr	$ACM0, @0x08a2
	sr	@0x0866, $ACM0
	lr	$ACM0, @0x08a3
	sr	@0x0867, $ACM0
	lr	$ACM0, @0x08a4
	sr	@0x0868, $ACM0
	lr	$ACM0, @0x08a8
	sr	@0x0880, $ACM0
	lr	$ACM0, @0x08a9
	sr	@0x0881, $ACM0
	lr	$ACM0, @0x08aa
	sr	@0x0886, $ACM0
	lr	$ACM0, @0x08ab
	sr	@0x0887, $ACM0
	lr	$ACM0, @0x08ac
	sr	@0x0888, $ACM0

// A line of MEM_SAMP decoded from the voice's state: 16 samples (mono), or 8 of each channel in turn (stereo, the
// channels' samples interleaved as a stereo 16-bit voice's); the state after it kept in ADPCM_NEXT
adpcm_decode:
	lr	$ACM0, @FLAGSL_SMP
	andcf	$ACM0, #0x2
	jlz	adpcm_decode_stereo
	call	adpcm_set_ch0
	lrs	$ACM0, @ACDAT
	sr	@0x0034, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0035, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0036, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0037, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0038, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0039, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003a, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003b, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003c, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003d, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003e, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003f, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0040, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0041, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0042, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0043, $ACM0
	jmp	adpcm_get_ch0

adpcm_decode_stereo:
	call	adpcm_set_ch0
	lrs	$ACM0, @ACDAT
	sr	@0x0034, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0036, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0038, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003a, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003c, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003e, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0040, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0042, $ACM0
	call	adpcm_get_ch0
	call	adpcm_set_ch1
	lrs	$ACM0, @ACDAT
	sr	@0x0035, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0037, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0039, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003b, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003d, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x003f, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0041, $ACM0
	lrs	$ACM0, @ACDAT
	sr	@0x0043, $ACM0
	jmp	adpcm_get_ch1

// The accelerator set to channel 0's state
adpcm_set_ch0:
	si	@ACFMT, #0x0000
	si	@ACGAN, #0x0000
	lr	$ACM0, @0x0864
	srs	@ACSAH, $ACM0
	lr	$ACM0, @0x0865
	srs	@ACSAL, $ACM0
	lr	$ACM0, @0x0862
	srs	@ACEAH, $ACM0
	lr	$ACM0, @0x0863
	srs	@ACEAL, $ACM0
	lr	$ACM0, @0x0860
	srs	@ACCAH, $ACM0
	lr	$ACM0, @0x0861
	srs	@ACCAL, $ACM0
	lr	$ACM0, @0x0870
	srs	@ACCOEF+0, $ACM0
	lr	$ACM0, @0x0871
	srs	@ACCOEF+1, $ACM0
	lr	$ACM0, @0x0872
	srs	@ACCOEF+2, $ACM0
	lr	$ACM0, @0x0873
	srs	@ACCOEF+3, $ACM0
	lr	$ACM0, @0x0874
	srs	@ACCOEF+4, $ACM0
	lr	$ACM0, @0x0875
	srs	@ACCOEF+5, $ACM0
	lr	$ACM0, @0x0876
	srs	@ACCOEF+6, $ACM0
	lr	$ACM0, @0x0877
	srs	@ACCOEF+7, $ACM0
	lr	$ACM0, @0x0878
	srs	@ACCOEF+8, $ACM0
	lr	$ACM0, @0x0879
	srs	@ACCOEF+9, $ACM0
	lr	$ACM0, @0x087a
	srs	@ACCOEF+10, $ACM0
	lr	$ACM0, @0x087b
	srs	@ACCOEF+11, $ACM0
	lr	$ACM0, @0x087c
	srs	@ACCOEF+12, $ACM0
	lr	$ACM0, @0x087d
	srs	@ACCOEF+13, $ACM0
	lr	$ACM0, @0x087e
	srs	@ACCOEF+14, $ACM0
	lr	$ACM0, @0x087f
	srs	@ACCOEF+15, $ACM0
	lr	$ACM0, @0x0866
	srs	@ACPDS, $ACM0
	lr	$ACM0, @0x0867
	srs	@ACYN1, $ACM0
	lr	$ACM0, @0x0868
	srs	@ACYN2, $ACM0
	ret

// Channel 0's state after its line
adpcm_get_ch0:
	lrs	$ACM0, @ACCAH
	sr	@0x08a0, $ACM0
	lrs	$ACM0, @ACCAL
	sr	@0x08a1, $ACM0
	lrs	$ACM0, @ACPDS
	sr	@0x08a2, $ACM0
	lrs	$ACM0, @ACYN1
	sr	@0x08a3, $ACM0
	lrs	$ACM0, @ACYN2
	sr	@0x08a4, $ACM0
	ret

// The accelerator set to channel 1's state
adpcm_set_ch1:
	si	@ACFMT, #0x0000
	si	@ACGAN, #0x0000
	lr	$ACM0, @0x0884
	srs	@ACSAH, $ACM0
	lr	$ACM0, @0x0885
	srs	@ACSAL, $ACM0
	lr	$ACM0, @0x0882
	srs	@ACEAH, $ACM0
	lr	$ACM0, @0x0883
	srs	@ACEAL, $ACM0
	lr	$ACM0, @0x0880
	srs	@ACCAH, $ACM0
	lr	$ACM0, @0x0881
	srs	@ACCAL, $ACM0
	lr	$ACM0, @0x0890
	srs	@ACCOEF+0, $ACM0
	lr	$ACM0, @0x0891
	srs	@ACCOEF+1, $ACM0
	lr	$ACM0, @0x0892
	srs	@ACCOEF+2, $ACM0
	lr	$ACM0, @0x0893
	srs	@ACCOEF+3, $ACM0
	lr	$ACM0, @0x0894
	srs	@ACCOEF+4, $ACM0
	lr	$ACM0, @0x0895
	srs	@ACCOEF+5, $ACM0
	lr	$ACM0, @0x0896
	srs	@ACCOEF+6, $ACM0
	lr	$ACM0, @0x0897
	srs	@ACCOEF+7, $ACM0
	lr	$ACM0, @0x0898
	srs	@ACCOEF+8, $ACM0
	lr	$ACM0, @0x0899
	srs	@ACCOEF+9, $ACM0
	lr	$ACM0, @0x089a
	srs	@ACCOEF+10, $ACM0
	lr	$ACM0, @0x089b
	srs	@ACCOEF+11, $ACM0
	lr	$ACM0, @0x089c
	srs	@ACCOEF+12, $ACM0
	lr	$ACM0, @0x089d
	srs	@ACCOEF+13, $ACM0
	lr	$ACM0, @0x089e
	srs	@ACCOEF+14, $ACM0
	lr	$ACM0, @0x089f
	srs	@ACCOEF+15, $ACM0
	lr	$ACM0, @0x0886
	srs	@ACPDS, $ACM0
	lr	$ACM0, @0x0887
	srs	@ACYN1, $ACM0
	lr	$ACM0, @0x0888
	srs	@ACYN2, $ACM0
	ret

// Channel 1's state after its line
adpcm_get_ch1:
	lrs	$ACM0, @ACCAH
	sr	@0x08a8, $ACM0
	lrs	$ACM0, @ACCAL
	sr	@0x08a9, $ACM0
	lrs	$ACM0, @ACPDS
	sr	@0x08aa, $ACM0
	lrs	$ACM0, @ACYN1
	sr	@0x08ab, $ACM0
	lrs	$ACM0, @ACYN2
	sr	@0x08ac, $ACM0
	ret

// exception table

exception0:	// RESET
	rti

exception1:	// STACK OVERFLOW
	rti

exception2:
	rti
	
exception3:
	rti

exception4:
	rti

exception5:	// ACCELERATOR ADDRESS OVERFLOW
	rti
	
exception6:
	rti

exception7:
	rti

// routine to read a word of the IROM space 

rom_dump_word:

	clr $ACC0

	lr	$ACM0, @CMBH
	ori	$ACM0, #0x8000 
	mrr	$AR0, $ACM0
	clr	$ACC0
	ilrr	$ACM0, @$AR0
	sr	@DMBH, $ACL0
	sr	@DMBL, $ACM0
	;si	@DIRQ, #0x1 // set the interrupt
	clr	$ACC0
	jmp	recv_cmd

polla_loca:

	clr $ACC0
	lri $acm0, #0x0
	andf $acm0,#0x1
	
	sr	@DMBH, $sr
	sr	@DMBL, $acm0
	;si	@DIRQ, #0x1 // set the interrupt
	clr	$ACC0
	jmp	recv_cmd


