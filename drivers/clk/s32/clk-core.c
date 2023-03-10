/*
 * Copyright 2015 Freescale Semiconductor, Inc.
 * Copyright 2017,2021 NXP
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "clk.h"

void s32_check_clocks(struct clk *clks[], unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (IS_ERR(clks[i]))
			pr_err("S32 clk %u: register failed with %ld\n",
			       i, PTR_ERR(clks[i]));
}

static struct clk *s32_obtain_fixed_clock_from_dt(const char *name)
{
	struct of_phandle_args phandle;
	struct clk *clk = ERR_PTR(-ENODEV);
	char *path;

	path = kasprintf(GFP_KERNEL, "/clocks/%s", name);
	if (!path)
		return ERR_PTR(-ENOMEM);

	phandle.np = of_find_node_by_path(path);
	kfree(path);

	if (phandle.np) {
		clk = of_clk_get_from_provider(&phandle);
		of_node_put(phandle.np);
	}
	return clk;
}

struct clk *s32_obtain_fixed_clock(const char *name, unsigned long rate)
{
	struct clk *clk;

	clk = s32_obtain_fixed_clock_from_dt(name);
	if (IS_ERR(clk))
		clk = s32_clk_fixed(name, rate);
	return clk;
}
