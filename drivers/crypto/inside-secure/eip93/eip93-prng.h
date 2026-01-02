/* SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2019 - 2021
 *
 * Richard van Schagen <vschagen@icloud.com>
 */
#ifndef _EIP93_PRNG_H_
#define _EIP93_PRNG_H_

#include "eip93-main.h"
#include "eip93-regs.h"

#define DEFAULT_PRNG_KEY "0123456789abcdef"
#define DEFAULT_PRNG_KSZ 16
#define DEFAULT_BLK_SZ 16
#define DEFAULT_V_SEED "zaybxcwdveuftgsh"

#define BUF_NOT_EMPTY 0
#define BUF_EMPTY 1
#define BUF_PENDING 2
#define PRNG_NEED_RESET 3

extern struct eip93_alg_template eip93_alg_prng;
extern struct eip93_alg_template eip93_alg_cprng;

bool eip93_prng_init(struct eip93_device *eip93, bool fLongSA);

void eip93_prng_done(struct eip93_device *eip93, u32 err);

struct eip93_prng_ctx {
	spinlock_t		prng_lock;
	unsigned char		rand_data[DEFAULT_BLK_SZ];
	unsigned char		last_rand_data[DEFAULT_BLK_SZ];
	u32			PRNGKey[4];
	u32			PRNGSeed[4];
	u32			PRNGDateTime[4];
	struct crypto_cipher	*tfm;
	u32			rand_data_valid;
	u32			flags;
};

#endif /* _EIP93_PRNG_H_ */
