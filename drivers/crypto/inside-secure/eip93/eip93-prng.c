// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 - 2021
 *
 * Richard van Schagen <vschagen@icloud.com>
 */

#include <linux/dma-mapping.h>

#include "eip93-prng.h"
#include "eip93-main.h"
#include "eip93-common.h"
#include "eip93-regs.h"

static int eip93_prng_push_job(struct eip93_device *eip93, bool reset)
{
	struct eip93_prng_device *prng = eip93->prng;
	struct eip93_descriptor cdesc;
	int cur = prng->cur_buf;
	int len, mode, err;

	if (reset) {
		len = 0;
		mode = 1;
	} else {
		len = 4080;
		mode = 2;
	}

	init_completion(&prng->Filled);
	atomic_set(&prng->State, BUF_EMPTY);

	memset(&cdesc, 0, sizeof(struct eip93_descriptor));
	cdesc.pe_ctrl_stat_word = FIELD_PREP(EIP93_PE_CTRL_PE_READY_DES_TRING_OWN,
					     EIP93_PE_CTRL_HOST_READY);
	cdesc.pe_ctrl_stat_word |= FIELD_PREP(EIP93_PE_CTRL_PE_PRNG_MODE,
					      mode);
	cdesc.src_addr = 0;
	cdesc.dst_addr = prng->PRNGBuffer_dma[cur];
	cdesc.sa_addr = prng->PRNGSaRecord_dma;
	cdesc.state_addr = 0;
	cdesc.arc4_addr = 0;
	cdesc.user_id = EIP93_DESC_PRNG | EIP93_DESC_LAST | EIP93_DESC_FINISH;
	cdesc.pe_length_word = FIELD_PREP(EIP93_PE_LENGTH_HOST_PE_READY,
					   EIP93_PE_LENGTH_HOST_READY);
	cdesc.pe_length_word |= FIELD_PREP(EIP93_PE_LENGTH_LENGTH, 4080);

	err = eip93_put_descriptor(eip93, &cdesc);
	/* TODO error handling */
	if (err)
		dev_err(eip93->dev, "PRNG: No Descriptor space");

	writel(1, eip93->base + EIP93_REG_PE_CD_COUNT);

	wait_for_completion(&prng->Filled);

//	if (atomic_read(&prng->State) == PRNG_NEED_RESET)
//		return false;

	return true;
}

/*----------------------------------------------------------------------------
 * eip93_prng_init
 *
 * This function initializes the PE PRNG for the ARM mode.
 *
 * Return Value
 *      true: PRNG is initialized
 *     false: PRNG initialization failed
 */
bool eip93_prng_init(struct eip93_device *eip93, bool fLongSA)
{
	struct eip93_prng_device *prng = eip93->prng;
	struct sa_record *sa_record;
	int i;
	const u32 PRNGKey[]  = {0xe0fc631d, 0xcbb9fb9a,
				0x869285cb, 0xcbb9fb9a};
	const u32 PRNGSeed[]  = {0x758bac03, 0xf20ab39e,
				 0xa569f104, 0x95dfaea6};
	const u32 PRNGDateTime[] = {0, 0, 0, 0};

	if (!eip93)
		return -ENODEV;

	prng->cur_buf = 0;
	/* TODO: check to kzalloc and create free after remove */
	prng->PRNGBuffer[0] = devm_kzalloc(eip93->dev, 4080, GFP_KERNEL);
	prng->PRNGBuffer_dma[0] = (u32)dma_map_single(eip93->dev,
				(void *)prng->PRNGBuffer[0],
				4080, DMA_FROM_DEVICE);

	prng->PRNGBuffer[1] = devm_kzalloc(eip93->dev, 4080, GFP_KERNEL);
	prng->PRNGBuffer_dma[1] = (u32)dma_map_single(eip93->dev,
				(void *)prng->PRNGBuffer[1],
				4080, DMA_FROM_DEVICE);

	prng->prng_sa_record = dmam_alloc_coherent(eip93->dev,
				sizeof(struct sa_record),
				&prng->PRNGSaRecord_dma, GFP_KERNEL);

	if (!prng->prng_sa_record) {
		dev_err(eip93->dev, "PRNG dma_alloc for sa_record failed\n");
		return -ENOMEM;
	}

	sa_record = &prng->prng_sa_record[0];

	sa_record->sa_cmd0_word = 0x00001307;
	sa_record->sa_cmd1_word = 0x02000000;

	for (i = 0; i < 4; i++) {
		sa_record->sa_key[i] = PRNGKey[i];
		sa_record->sa_i_digest[i] = PRNGSeed[i];
		sa_record->sa_o_digest[i] = PRNGDateTime[i];
	}

	return eip93_prng_push_job(eip93, true);
}

void eip93_prng_done(struct eip93_device *eip93, u32 err)
{
	struct eip93_prng_device *prng = eip93->prng;
	int cur = prng->cur_buf;

	if (err) {
		dev_err(eip93->dev, "PRNG error: %d\n", err);
		atomic_set(&prng->State, PRNG_NEED_RESET);
	}

	/* Buffer refilled, invalidate cache */
	dma_unmap_single(eip93->dev, prng->PRNGBuffer_dma[cur],
			 4080, DMA_FROM_DEVICE);

	complete(&prng->Filled);
}

static int get_prng_bytes(char *buf, size_t nbytes, struct eip93_prng_ctx *ctx,
			  int do_cont_test)
{
	int err;

	spin_lock(&ctx->prng_lock);

	err = -EINVAL;
	if (ctx->flags & PRNG_NEED_RESET)
		goto done;

done:
	spin_unlock(&ctx->prng_lock);
	return err;
}

static int eip93_prng_generate(struct crypto_rng *tfm, const u8 *src,
			       unsigned int slen, u8 *dst, unsigned int dlen)
{
	struct eip93_prng_ctx *prng = crypto_rng_ctx(tfm);

	return get_prng_bytes(dst, dlen, prng, 1);
}

static int eip93_prng_seed(struct crypto_rng *tfm, const u8 *seed,
			   unsigned int slen)
{
	return 0;
}

static int reset_prng_context(struct eip93_prng_ctx *ctx,
			      const unsigned char *key,
			      const unsigned char *V,
			      const unsigned char *DT)
{
	spin_lock_bh(&ctx->prng_lock);
	ctx->flags |= PRNG_NEED_RESET;

	if (key)
		memcpy(ctx->PRNGKey, key, DEFAULT_PRNG_KSZ);
	else
		memcpy(ctx->PRNGKey, DEFAULT_PRNG_KEY, DEFAULT_PRNG_KSZ);

	if (V)
		memcpy(ctx->PRNGSeed, V, DEFAULT_BLK_SZ);
	else
		memcpy(ctx->PRNGSeed, DEFAULT_V_SEED, DEFAULT_BLK_SZ);

	if (DT)
		memcpy(ctx->PRNGDateTime, DT, DEFAULT_BLK_SZ);
	else
		memset(ctx->PRNGDateTime, 0, DEFAULT_BLK_SZ);

	memset(ctx->rand_data, 0, DEFAULT_BLK_SZ);
	memset(ctx->last_rand_data, 0, DEFAULT_BLK_SZ);

	ctx->rand_data_valid = DEFAULT_BLK_SZ;

	ctx->flags &= ~PRNG_NEED_RESET;
	spin_unlock_bh(&ctx->prng_lock);

	return 0;
}

static void free_prng_context(struct eip93_prng_ctx *ctx)
{
	crypto_free_cipher(ctx->tfm);
}

static int cprng_init(struct crypto_tfm *tfm)
{
	struct eip93_prng_ctx *ctx = crypto_tfm_ctx(tfm);

	spin_lock_init(&ctx->prng_lock);

	if (reset_prng_context(ctx, NULL, NULL, NULL) < 0)
		return -EINVAL;

	/*
	 * after allocation, we should always force the user to reset
	 * so they don't inadvertently use the insecure default values
	 * without specifying them intentially
	 */
	ctx->flags |= PRNG_NEED_RESET;
	return 0;
}

static void cprng_exit(struct crypto_tfm *tfm)
{
	free_prng_context(crypto_tfm_ctx(tfm));
}

struct eip93_alg_template eip93_alg_prng = {
	.type = EIP93_ALG_TYPE_PRNG,
	.flags = 0,
	.alg.rng = {
		.generate = eip93_prng_generate,
		.seed = eip93_prng_seed,
		.seedsize = 0,
		.base = {
			.cra_name = "stdrng",
			.cra_driver_name = "eip93-prng",
			.cra_priority = 300,
			.cra_ctxsize = sizeof(struct eip93_prng_ctx),
			.cra_module = THIS_MODULE,
			.cra_init = cprng_init,
			.cra_exit = cprng_exit,
		},
	},
};
