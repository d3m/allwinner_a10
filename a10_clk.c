/*-
 * Copyright (c) 2013 Ganbold Tsagaankhuu <ganbold@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* Simple clock driver for Allwinner A10 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>
#include <sys/rman.h>
#include <sys/timeet.h>
#include <sys/timetc.h>
#include <sys/watchdog.h>
#include <machine/bus.h>
#include <machine/cpu.h>
#include <machine/frame.h>
#include <machine/intr.h>

#include <dev/fdt/fdt_common.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <machine/bus.h>
#include <machine/fdt.h>

#include "a10_clk.h"

struct a10_ccm_softc {
	struct resource		*res[2];
	bus_space_tag_t		bst;
	bus_space_handle_t	bsh;
};

static struct resource_spec a10_ccm_spec[] = {
	{ SYS_RES_MEMORY,	0,	RF_ACTIVE },
	{ -1, 0 }
};

static struct a10_ccm_softc *a10_ccm_sc = NULL;

#define ccm_read_4(reg)			\
	bus_space_read_4(a10_ccm_sc->bst, a10_ccm_sc->bsh, reg)
#define ccm_write_4(reg, val)		\
	bus_space_write_4(a10_ccm_sc->bst, a10_ccm_sc->bsh, reg, val)

static int
a10_ccm_probe(device_t dev)
{
	if (ofw_bus_is_compatible(dev, "allwinner,sun4i-ccm")) {
		device_set_desc(dev, "Allwinner Simple Clock Management");
		return(BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
a10_ccm_attach(device_t dev)
{
	struct a10_ccm_softc *sc = device_get_softc(dev);

	if (a10_ccm_sc)
		return (ENXIO);

	if (bus_alloc_resources(dev, a10_ccm_spec, sc->res)) {
		device_printf(dev, "could not allocate resources\n");
		return (ENXIO);
	}

	sc->bst = rman_get_bustag(sc->res[0]);
	sc->bsh = rman_get_bushandle(sc->res[0]);

	a10_ccm_sc = sc;

	return (0);
}

static device_method_t a10_ccm_methods[] = {
	DEVMETHOD(device_probe,		a10_ccm_probe),
	DEVMETHOD(device_attach,	a10_ccm_attach),
	{ 0, 0 }
};

static driver_t a10_ccm_driver = {
	"a10_ccm",
	a10_ccm_methods,
	sizeof(struct a10_ccm_softc),
};

static devclass_t a10_ccm_devclass;

DRIVER_MODULE(a10_ccm, simplebus, a10_ccm_driver, a10_ccm_devclass, 0, 0);

int
a10_clk_usb_activate(void)
{
	struct a10_ccm_softc *sc = a10_ccm_sc;
	uint32_t reg_value = 0;

	if (sc == NULL)
		return ENXIO;

	/* Gating AHB clock for USB */
	reg_value = ccm_read_4(CCM_AHB_GATING0);
	reg_value |= (1 << 0); /* AHB clock gate usb0 */
	reg_value |= (1 << 3); /* AHB clock gate ehci1 */
	ccm_write_4(CCM_AHB_GATING0, reg_value);

	/* Enable clock for USB */
	reg_value = ccm_read_4(CCM_USB_CLK);
	reg_value |= (1 << 8); /* USBPHY */
	reg_value |= (1 << 0); /* disable reset for USB0 */
	reg_value |= (1 << 1); /* disable reset for USB1 */
	reg_value |= (1 << 2); /* disable reset for USB2 */
	ccm_write_4(CCM_USB_CLK, reg_value);

	return (0);
}

int
a10_clk_usb_deactivate(void)
{
	struct a10_ccm_softc *sc = a10_ccm_sc;
	uint32_t reg_value = 0;

	if (sc == NULL)
		return ENXIO;

	/* Disable clock for USB */
	reg_value = ccm_read_4(CCM_USB_CLK);
	reg_value &= ~(1 << 8); /* USBPHY */
	reg_value &= ~(1 << 0); /* reset for USB0 */
	reg_value &= ~(1 << 1); /* reset for USB1 */
	reg_value &= ~(1 << 2); /* reset for USB2 */
	ccm_write_4(CCM_USB_CLK, reg_value);

	/* Disable gating AHB clock for USB */
	reg_value = ccm_read_4(CCM_AHB_GATING0);
	reg_value &= ~(1 << 0); /* AHB clock gate usb0 */
	reg_value &= ~(1 << 3); /* AHB clock gate ehci1 */
	ccm_write_4(CCM_AHB_GATING0, reg_value);

	return (0);
}