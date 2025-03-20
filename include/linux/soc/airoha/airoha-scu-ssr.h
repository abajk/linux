/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __AIROHA_SCU_SSR__
#define __AIROHA_SCU_SSR__

enum airoha_scu_serdes_modes {
	AIROHA_SCU_SERDES_MODE_PCIE0_X1,
	AIROHA_SCU_SERDES_MODE_PCIE0_X2,
	AIROHA_SCU_SERDES_MODE_PCIE1_X1,
	AIROHA_SCU_SERDES_MODE_PCIE2_X1,
	AIROHA_SCU_SERDES_MODE_USB3,
	AIROHA_SCU_SERDES_MODE_ETHERNET,
};

struct airoha_scu_ssr_serdes_info {
	unsigned int *possible_modes;
	unsigned int num_modes;
};

struct airoha_scu_ssr_data {
	const struct airoha_scu_ssr_serdes_info *ports_info;
};

#endif
