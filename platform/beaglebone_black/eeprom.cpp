/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2016 John Seamons, ZL/KF6VO

#include "types.h"
#include "config.h"
#include "kiwi.h"
#include "misc.h"
#include "peri.h"
#include "spi.h"
#include "coroutines.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#define	SEQ_SERNO_FILE "/root/kiwi.config/seq_serno.txt"
#define	SEQ_START		"1014"

int eeprom_next_serno(next_serno_e type, int set_serno)
{
	int n, next_serno, serno = -1;
	FILE *fp;

	system("cp " SEQ_SERNO_FILE " " SEQ_SERNO_FILE ".bak");

retry:
	if ((fp = fopen(SEQ_SERNO_FILE, "r+")) == NULL) {
		if (errno == ENOENT) {
			system("echo " SEQ_START " > " SEQ_SERNO_FILE);
			goto retry;
		}
		mlprintf("EEPROM next: open %s %s\n", SEQ_SERNO_FILE, strerror(errno));
		return -1;
	}
	if ((n = fscanf(fp, "%d\n", &serno)) != 1) {
		mlprintf("EEPROM next: serial_no file scan\n");
		return -1;
	}
	
	if (type == SERNO_READ)
		return serno;
	
	if (type == SERNO_WRITE)
		next_serno = serno = set_serno;
	
	if (type == SERNO_ALLOC)
		next_serno = serno + 1;
	
	rewind(fp);
	if ((n = fprintf(fp, "%d\n", next_serno)) <= 0) {
		mlprintf("EEPROM next: write %s\n", strerror(errno));
		return -1;
	}
	fclose(fp);
	
	mprintf("EEPROM next: new seq serno = %d\n", serno);
	return serno;
	
}

struct eeprom_t {
	#define	EE_HEADER	0xAA5533EE
	u4_t header;
	
	#define	EE_FMT_REV	"A1"
	char fmt_rev[2];
	
	char board_name[32];
	char version[4];
	char mfg[16];
	char part_no[16];
	char week[2];
	char year[2];
	char assembly[4];
	char serial_no[4];
	
	u2_t n_pins;
	u2_t io_pins[EE_NPINS];
	
	#define	EE_MA_3V3	250
	#define	EE_MA_5INT	0
	#define	EE_MA_5EXT	0
	#define	EE_MA_DC	1500
	u2_t mA_3v3, mA_5int, mA_5ext, mA_DC;
	
	u1_t free[1];
} __attribute__((packed));

static eeprom_t eeprom;
static bool debian8 = false;

#define EEPROM_DEV_DEBIAN7	"/sys/bus/i2c/devices/1-0054/eeprom"
#define EEPROM_DEV_DEBIAN8	"/sys/bus/i2c/devices/2-0054/eeprom"

int eeprom_check()
{
	eeprom_t *e = &eeprom;
	const char *fn;
	FILE *fp;
	int n;
	
	fn = EEPROM_DEV_DEBIAN7;
	fp = fopen(fn, "r");
	
	if (fp == NULL && errno == ENOENT) {
		fn = EEPROM_DEV_DEBIAN8;
		debian8 = true;
		fp = fopen(fn, "r");
	}
	
	if (fp == NULL) {
		mlprintf("EEPROM check: open %s %s\n", fn, strerror(errno));
		return -1;
	}

	memset(e, 0, sizeof(eeprom_t));
	if ((n = fread(e, 1, sizeof(eeprom_t), fp)) < 0) {
		mlprintf("EEPROM check: read %s\n", strerror(errno));
		return -1;
	}
	if (n < sizeof(eeprom_t)) {
		mlprintf("EEPROM check: WARNING short read %d\n", n);
		return -1;
	}
	fclose(fp);
	
	u4_t header = FLIP32(e->header);
	if (header != EE_HEADER) {
		mlprintf("EEPROM check: bad header, got 0x%08x want 0x%08x\n", header, EE_HEADER);
		return -1;
	}
	
	char serial_no[sizeof(e->serial_no)+1];
	GET_CHARS(e->serial_no, serial_no);
	int serno = -1;
	n = sscanf(serial_no, "%d", &serno);
	mprintf("EEPROM check: read serial_no \"%s\" %d\n", serial_no, serno);
	if (n != 1) {
		mlprintf("EEPROM check: scan failed\n");
		return -1;
	}
	
	return serno;
}

void eeprom_write()
{
	int n, next_serno = eeprom_next_serno(SERNO_ALLOC, 0);
	const char *fn;
	FILE *fp;
	
	eeprom_t *e = &eeprom;
	
	e->header = FLIP32(EE_HEADER);
	SET_CHARS(e->fmt_rev, EE_FMT_REV, ' ');
	
	SET_CHARS(e->board_name, "KiwiSDR", ' ');
	SET_CHARS(e->version, "v0.5", ' ');		// fixme
	SET_CHARS(e->mfg, "Seed.cc", ' ');
	SET_CHARS(e->part_no, "(fixme part_no)", ' ');

	SET_CHARS(e->week, "WW", ' ');
	SET_CHARS(e->year, "YY", ' ');
	SET_CHARS(e->assembly, "&&&&", ' ');
	sprintf(e->serial_no, "%4d", next_serno);	// caution: leaves '\0' at start of next field (n_pins)
	
	e->n_pins = FLIP16(2*26);
	int pin;
	for (pin = 0; pin < EE_NPINS; pin++) {
		pin_t *p = &eeprom_pins[pin];
		u4_t off = EE_PINS_OFFSET_BASE + pin*2;
		if (p->gpio.eeprom_off) {
			e->io_pins[pin] = FLIP16(p->attrs);
			assert(p->gpio.eeprom_off == off);
			//printf("EEPROM %3d 0x%x %s-%02d 0x%04x\n", off, off,
			//	(p->gpio.pin & P9)? "P9":"P8", p->gpio.pin & PIN_BITS, p->attrs);
		} else {
			//printf("EEPROM %3d 0x%x not used\n", off, off);
		}
	}
	
	e->mA_3v3 = FLIP16(EE_MA_3V3);
	e->mA_5int = FLIP16(EE_MA_5INT);
	e->mA_5ext = FLIP16(EE_MA_5INT);
	e->mA_DC = FLIP16(EE_MA_DC);
	
	ctrl_clr_set(CTRL_EEPROM_WP, 0);
	
	fn = debian8? EEPROM_DEV_DEBIAN8 : EEPROM_DEV_DEBIAN7;

	if ((fp = fopen(fn, "r+")) == NULL) {
		mlprintf("EEPROM write: open %s %s\n", fn, strerror(errno));
		return;
	}

	if ((n = fwrite(e, 1, sizeof(eeprom_t), fp)) != sizeof(eeprom_t)) {
		mlprintf("EEPROM write: write %s\n", strerror(errno));
		return;
	}

	mprintf("EEPROM write: wrote %dB\n", n);
	fclose(fp);
	ctrl_clr_set(0, CTRL_EEPROM_WP);
}
