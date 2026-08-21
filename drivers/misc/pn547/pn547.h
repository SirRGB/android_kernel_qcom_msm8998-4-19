/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _PN547_H
#define _PN547_H

#include <linux/ioctl.h>

#define PN547_MAGIC	0xE9
#define PN547_SET_PWR	_IOW(PN547_MAGIC, 0x01, unsigned int)

enum pn547_state {
	PN547_STATE_UNKNOWN,
	PN547_STATE_OFF,
	PN547_STATE_ON,
	PN547_STATE_FWDL,
};

#endif
