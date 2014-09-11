//-----------------------------------------------------------------------------
// Copyright (C) 2009 Michael Gernoth <michael at gernoth.net>
// Copyright (C) 2010 iZsh <izsh at fail0verflow.com>
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// UI utilities
//-----------------------------------------------------------------------------

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <readline/readline.h>
#include <pthread.h>

#include "ui.h"

double CursorScaleFactor;
int PlotGridX, PlotGridY, PlotGridXdefault= 64, PlotGridYdefault= 64;
int offline;
int flushAfterWrite = 0;  //buzzy
extern pthread_mutex_t print_lock;

static char *logfilename = "proxmark3.log";

void PrintAndLog(char *fmt, ...)
{
	char *saved_line;
	int saved_point;
	va_list argptr, argptr2;
	static FILE *logfile = NULL;
	static int logging=1;

	// lock this section to avoid interlacing prints from different threats
	pthread_mutex_lock(&print_lock);
  
	if (logging && !logfile) {
		logfile=fopen(logfilename, "a");
		if (!logfile) {
			fprintf(stderr, "Can't open logfile, logging disabled!\n");
			logging=0;
		}
	}
	
	int need_hack = (rl_readline_state & RL_STATE_READCMD) > 0;

	if (need_hack) {
		saved_point = rl_point;
		saved_line = rl_copy_text(0, rl_end);
		rl_save_prompt();
		rl_replace_line("", 0);
		rl_redisplay();
	}
	
	va_start(argptr, fmt);
	va_copy(argptr2, argptr);
	vprintf(fmt, argptr);
	printf("          "); // cleaning prompt
	va_end(argptr);
	printf("\n");

	if (need_hack) {
		rl_restore_prompt();
		rl_replace_line(saved_line, 0);
		rl_point = saved_point;
		rl_redisplay();
		free(saved_line);
	}
	
	if (logging && logfile) {
		vfprintf(logfile, fmt, argptr2);
		fprintf(logfile,"\n");
		fflush(logfile);
	}
	va_end(argptr2);

	if (flushAfterWrite == 1)  //buzzy
	{
		fflush(NULL);
	}
	//release lock
	pthread_mutex_unlock(&print_lock);  
}


void SetLogFilename(char *fn)
{
  logfilename = fn;
}


uint8_t manchester_decode(const uint8_t * data, const size_t len, uint8_t * dataout){
	
	size_t bytelength = len;
	
	uint8_t bitStream[bytelength];
	memset(bitStream, 0x00, bytelength);
	
	int clock,high, low, bit, hithigh, hitlow, first, bit2idx, lastpeak;
	int i,invert, lastval;
	int bitidx = 0;
	int lc = 0;
	int warnings = 0;
	high = 1;
	low =  bit = bit2idx = lastpeak = invert = lastval = hithigh = hitlow = first = 0;
	clock = 0xFFFF;

	/* Detect high and lows */
	for (i = 0; i < bytelength; i++) {
		if (data[i] > high)
			high = data[i];
		else if (data[i] < low)
			low = data[i];
	}
	
	/* get clock */
	int j=0;
	for (i = 1; i < bytelength; i++) {
		/* if this is the beginning of a peak */
		j = i-1;
		if ( data[j] != data[i] && 
		     data[i] == high)
		{
		  /* find lowest difference between peaks */
			if (lastpeak && i - lastpeak < clock)
				clock = i - lastpeak;
			lastpeak = i;
		}
	}
    
	int tolerance = clock/4;
	PrintAndLog(" Detected clock: %d",clock);

	/* Detect first transition */
	  /* Lo-Hi (arbitrary)       */
	  /* skip to the first high */
	  for (i= 0; i < bytelength; i++)
		if (data[i] == high)
		  break;
		  
	  /* now look for the first low */
	  for (; i < bytelength; i++) {
		if (data[i] == low) {
			lastval = i;
			break;
		}
	  }
	  
	/* If we're not working with 1/0s, demod based off clock */
	if (high != 1)
	{
		bit = 0; /* We assume the 1st bit is zero, it may not be
			  * the case: this routine (I think) has an init problem.
			  * Ed.
			  */
		for (; i < (int)(bytelength / clock); i++)
		{
		hithigh = 0;
		hitlow = 0;
		first = 1;

		/* Find out if we hit both high and low peaks */
		for (j = 0; j < clock; j++)
		{
			if (data[(i * clock) + j] == high)
				hithigh = 1;
			else if (data[(i * clock) + j] == low)
				hitlow = 1;

			/* it doesn't count if it's the first part of our read
			   because it's really just trailing from the last sequence */
			if (first && (hithigh || hitlow))
			  hithigh = hitlow = 0;
			else
			  first = 0;

			if (hithigh && hitlow)
			  break;
		  }

		  /* If we didn't hit both high and low peaks, we had a bit transition */
		  if (!hithigh || !hitlow)
			bit ^= 1;

		  bitStream[bit2idx++] = bit ^ invert;
		}
	}
	/* standard 1/0 bitstream */
  else {
		/* Then detect duration between 2 successive transitions */
		for (bitidx = 1; i < bytelength; i++) {
		
			if (data[i-1] != data[i]) {
				lc = i-lastval;
				lastval = i;

				// Error check: if bitidx becomes too large, we do not
				// have a Manchester encoded bitstream or the clock is really
				// wrong!
				if (bitidx > (bytelength*2/clock+8) ) {
					PrintAndLog("Error: the clock you gave is probably wrong, aborting.");
					return 0;
				}
				// Then switch depending on lc length:
				// Tolerance is 1/4 of clock rate (arbitrary)
				if (abs(lc-clock/2) < tolerance) {
					// Short pulse : either "1" or "0"
					bitStream[bitidx++] = data[i-1];
				} else if (abs(lc-clock) < tolerance) {
					// Long pulse: either "11" or "00"
					bitStream[bitidx++] = data[i-1];
					bitStream[bitidx++] = data[i-1];
				} else {
					// Error
					warnings++;
					PrintAndLog("Warning: Manchester decode error for pulse width detection.");
					if (warnings > 10) {
						PrintAndLog("Error: too many detection errors, aborting.");
						return 0;
					}
				}
			}
		}
	}
	// At this stage, we now have a bitstream of "01" ("1") or "10" ("0"), parse it into final decoded bitstream
    // Actually, we overwrite BitStream with the new decoded bitstream, we just need to be careful
    // to stop output at the final bitidx2 value, not bitidx
    for (i = 0; i < bitidx; i += 2) {
		if ((bitStream[i] == 0) && (bitStream[i+1] == 1)) {
			bitStream[bit2idx++] = 1 ^ invert;
		} 
		else if ((bitStream[i] == 1) && (bitStream[i+1] == 0)) {
			bitStream[bit2idx++] = 0 ^ invert;
		} 
		else {
			// We cannot end up in this state, this means we are unsynchronized,
			// move up 1 bit:
			i++;
			warnings++;
			PrintAndLog("Unsynchronized, resync...");
			if (warnings > 10) {
				PrintAndLog("Error: too many decode errors, aborting.");
				return 0;
			}
		}
    }

	  // PrintAndLog(" Manchester decoded bitstream : %d bits", (bit2idx-16));
	  // uint8_t mod = (bit2idx-16) % blocksize;
	  // uint8_t div = (bit2idx-16) / blocksize;
	  
	  // // Now output the bitstream to the scrollback by line of 16 bits
	  // for (i = 0; i < div*blocksize; i+=blocksize) {
		// PrintAndLog(" %s", sprint_bin(bitStream+i,blocksize) );
	  // }
	  // if ( mod > 0 ){
		// PrintAndLog(" %s", sprint_bin(bitStream+i, mod) );
	  // }
	
	if ( bit2idx > 0 )
		memcpy(dataout, bitStream, bit2idx);
	
	free(bitStream);
	return bit2idx;
}

void PrintPaddedManchester( uint8_t* bitStream, size_t len, size_t blocksize){

	  PrintAndLog(" Manchester decoded bitstream : %d bits", len);
	  
	  uint8_t mod = len % blocksize;
	  uint8_t div = len / blocksize;
	  int i;
	  // Now output the bitstream to the scrollback by line of 16 bits
	  for (i = 0; i < div*blocksize; i+=blocksize) {
		PrintAndLog(" %s", sprint_bin(bitStream+i,blocksize) );
	  }
	  if ( mod > 0 ){
		PrintAndLog(" %s", sprint_bin(bitStream+i, mod) );
	  }
}
