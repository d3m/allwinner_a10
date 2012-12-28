/*-
 * Copyright (c) 2012 Ganbold Tsagaankhuu <ganbold@gmail.com>
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Allwinner A10 attachment driver for the USB Enhanced Host Controller.
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include "opt_bus.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/rman.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include <machine/bus.h>
#include <dev/ofw/ofw_bus.h> 
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/usb/usb.h> 
#include <dev/usb/usbdi.h>

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_util.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/ehci.h>
#include <dev/usb/controller/ehcireg.h>

#include "gpio.h"

#define EHCI_HC_DEVSTR			"Allwinner Integrated USB 2.0 controller"

/*
#define USB0_BASE	 		0xe1c13000
#define USB1_BASE 			0xe1c14000
#define USB2_BASE 			0xe1c1c000
#define CCMU_BASE 			0xe1c20000
*/
//#define CCM_AHB_GATING0		(CCMU_BASE - USB1_BASE + 0x60)	/* relative from USB0 base address */
//#define CCM_USB_CLK			(CCMU_BASE - USB1_BASE + 0xcc)  /* relative from USB0 base address */


#define SW_USB1_BASE			0x01c14000
#define SW_USB2_BASE			0x01c1c000
#define SW_USB_EHCI_BASE_OFFSET 	0x00
#define SW_USB_OHCI_BASE_OFFSET 	0x400
#define SW_USB_EHCI_LEN  		0x58
#define SW_USB_OHCI_LEN  		0x58
#define SW_USB_PMU_IRQ_ENABLE 		0x800

#define A10_READ_4(sc, reg) \
	bus_space_read_4((sc)->sc_io_tag, (sc)->sc_io_hdl, reg)

#define A10_WRITE_4(sc, reg, data) \
	bus_space_write_4((sc)->sc_io_tag, (sc)->sc_io_hdl, reg, data)

static device_attach_t a10_ehci_attach;
static device_detach_t a10_ehci_detach;

bs_r_1_proto(reversed);
bs_w_1_proto(reversed);

static int
a10_ehci_probe(device_t self)
{

	if (!ofw_bus_is_compatible(self, "a10,usb-ehci")) 
		return (ENXIO);

	device_set_desc(self, EHCI_HC_DEVSTR);

	return (BUS_PROBE_DEFAULT);
}

static int
a10_ehci_attach(device_t self)
{
	ehci_softc_t *sc = device_get_softc(self);
	bus_space_handle_t bsh;
	int err;
	int rid;
	uint32_t reg_value = 0;

	/* initialise some bus fields */
	sc->sc_bus.parent = self;
	sc->sc_bus.devices = sc->sc_devices;
	sc->sc_bus.devices_max = EHCI_MAX_DEVICES;

	/* get all DMA memory */
	if (usb_bus_mem_alloc_all(&sc->sc_bus,
	    USB_GET_DMA_TAG(self), &ehci_iterate_hw_softc)) {
		return (ENOMEM);
	}

	sc->sc_bus.usbrev = USB_REV_2_0;

	rid = 0;
	sc->sc_io_res = bus_alloc_resource_any(self, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (!sc->sc_io_res) {
		device_printf(self, "Could not map memory\n");
		goto error;
	}

	sc->sc_io_tag = rman_get_bustag(sc->sc_io_res);
	sc->sc_io_hdl = rman_get_bushandle(sc->sc_io_res);
	bsh = rman_get_bushandle(sc->sc_io_res);
	sc->sc_io_size = rman_get_size(sc->sc_io_res);

        if (bus_space_subregion(sc->sc_io_tag, bsh, 0x00,
            sc->sc_io_size, &sc->sc_io_hdl) != 0)
                panic("%s: unable to subregion USB host registers",
                    device_get_name(self));

	rid = 0;
	sc->sc_irq_res = bus_alloc_resource_any(self, SYS_RES_IRQ, &rid,
	    RF_SHAREABLE | RF_ACTIVE);
	if (sc->sc_irq_res == NULL) {
		device_printf(self, "Could not allocate irq\n");
		goto error;
	}
	sc->sc_bus.bdev = device_add_child(self, "usbus", -1);
	if (!sc->sc_bus.bdev) {
		device_printf(self, "Could not add USB device\n");
		goto error;
	}
	device_set_ivars(sc->sc_bus.bdev, &sc->sc_bus);
	device_set_desc(sc->sc_bus.bdev, EHCI_HC_DEVSTR);

	sprintf(sc->sc_vendor, "Allwinner");


	err = bus_setup_intr(self, sc->sc_irq_res, INTR_TYPE_BIO | INTR_MPSAFE,
	    NULL, (driver_intr_t *)ehci_interrupt, sc, &sc->sc_intr_hdl);
	if (err) {
		device_printf(self, "Could not setup irq, %d\n", err);
		sc->sc_intr_hdl = NULL;
		goto error;
	}

	sc->sc_flags |= EHCI_SCFLG_DONTRESET;

	/* Enable power in gpio pin for USB */

	/* testing to see addresses of PIO regs */
//	sunxi_gpio_set_cfgpin(SUNXI_GPB(16), SUNXI_GPIO_OUTPUT);

//	sunxi_gpio_set_cfgpin(SUNXI_GPB(9), SUNXI_GPIO_OUTPUT);
//	sunxi_gpio_set_cfgpin(SUNXI_GPH(6), SUNXI_GPIO_OUTPUT);

	sunxi_gpio_get_cfgpin(SUNXI_GPH(6));
	sunxi_gpio_set_cfgpin(SUNXI_GPH(6), SUNXI_GPIO_OUTPUT);

//	sunxi_gpio_set_cfgpin(SUNXI_GPH(3), SUNXI_GPIO_OUTPUT);

	/* maybe totally wrong hack */
	volatile uint32_t *ccm_ahb_gating = (uint32_t *) 0xe1c20060;
	volatile uint32_t *ccm_usb_clock = (uint32_t *) 0xe1c200cc;

	/* Gating AHB clock for USB_phy0 */
	*ccm_ahb_gating |= (1 << 0);  /* AHB clock gate usb0 */

	reg_value = 10000;
	while(reg_value--);

	/* Enable clock for USB */
	*ccm_usb_clock |= (1 << 8);
	*ccm_usb_clock |= (1 << 0); /* disable reset for USB0 */

        volatile uint32_t *usb_reg_pctl = (uint32_t *) 0xe1c14040;
        volatile uint32_t *usb_reg_iscr = (uint32_t *) 0xe1c14400;

        /* ISCR */
        *usb_reg_iscr |= (1 << 17); /* USBC_BP_ISCR_ID_PULLUP_EN */
        *usb_reg_iscr |= (1 << 16); /* USBC_BP_ISCR_DPDM_PULLUP_EN */

	*usb_reg_iscr |= (0x03 << 12); /* USBC_BP_ISCR_FORCE_VBUS_VALID */

    	uint32_t temp = *usb_reg_iscr;

        temp &= ~(1 << 6);
       	temp &= ~(1 << 5);
        temp &= ~(1 << 4);
        temp |= (1 << 3);

	/*
	#define  USBC_BP_ISCR_VBUS_CHANGE_DETECT        6
	#define  USBC_BP_ISCR_ID_CHANGE_DETECT          5
	#define  USBC_BP_ISCR_DPDM_CHANGE_DETECT        4
	#define  USBC_BP_ISCR_IRQ_ENABLE                3
	#define  USBC_BP_ISCR_VBUS_CHANGE_DETECT_EN     2
	#define  USBC_BP_ISCR_ID_CHANGE_DETECT_EN       1
	*/

        *usb_reg_iscr = temp;

        /* Enable power */
        *usb_reg_pctl |= (1 << 0); /* SUSPEND_EN */
        *usb_reg_pctl |= (1 << 1); /* SUSPEND */
        *usb_reg_pctl |= (1 << 2); /* RESUME */
        *usb_reg_pctl |= (1 << 3); /* RESET */
        *usb_reg_pctl |= (1 << 4); /* HIGH_SPEED_FLAG */
        *usb_reg_pctl |= (1 << 5); /* HIGH_SPEED_EN */

	err = ehci_init(sc);
	if (!err) {
		err = device_probe_and_attach(sc->sc_bus.bdev);
	}
	if (err) {
		device_printf(self, "USB init failed err=%d\n", err);
		goto error;
	}
	return (0);

error:
	a10_ehci_detach(self);
	return (ENXIO);
}

static int
a10_ehci_detach(device_t self)
{
	ehci_softc_t *sc = device_get_softc(self);
	device_t bdev;
	int err;
	uint32_t reg_value = 0;

 	if (sc->sc_bus.bdev) {
		bdev = sc->sc_bus.bdev;
		device_detach(bdev);
		device_delete_child(self, bdev);
	}
	/* during module unload there are lots of children leftover */
	device_delete_children(self);

 	if (sc->sc_irq_res && sc->sc_intr_hdl) {
		/*
		 * only call ehci_detach() after ehci_init()
		 */
		ehci_detach(sc);

		err = bus_teardown_intr(self, sc->sc_irq_res, sc->sc_intr_hdl);

		if (err)
			/* XXX or should we panic? */
			device_printf(self, "Could not tear down irq, %d\n",
			    err);
		sc->sc_intr_hdl = NULL;
	}

 	if (sc->sc_irq_res) {
		bus_release_resource(self, SYS_RES_IRQ, 0, sc->sc_irq_res);
		sc->sc_irq_res = NULL;
	}
	if (sc->sc_io_res) {
		bus_release_resource(self, SYS_RES_MEMORY, 0,
		    sc->sc_io_res);
		sc->sc_io_res = NULL;
	}
	usb_bus_mem_free_all(&sc->sc_bus, &ehci_iterate_hw_softc);

        /* maybe totally wrong hack */
        volatile uint32_t *ccm_ahb_gating = (uint32_t *) 0xe1c20060;
        volatile uint32_t *ccm_usb_clock = (uint32_t *) 0xe1c200cc;

        /* Disable AHB clock for USB_phy0 */
        *ccm_ahb_gating &= ~(1 << 0);  /* AHB clock gate usb0 */

        reg_value = 10000;
        while(reg_value--);

        /* Disable clock for USB */
        *ccm_usb_clock &= ~(1 << 8);
        *ccm_usb_clock &= ~(1 << 0); /* disable reset for USB0 */

        volatile uint32_t *usb_reg_pctl = (uint32_t *) 0xe1c14040;
        volatile uint32_t *usb_reg_iscr = (uint32_t *) 0xe1c14400;

        /* ISCR */
        *usb_reg_iscr &= ~(1 << 17); /* USBC_BP_ISCR_ID_PULLUP_EN */
        *usb_reg_iscr &= ~(1 << 16); /* USBC_BP_ISCR_DPDM_PULLUP_EN */

	*usb_reg_iscr &= ~(0x03 << 12); /* USBC_BP_ISCR_FORCE_VBUS_VALID */
	*usb_reg_iscr |= (0x02 << 12); /* USBC_BP_ISCR_FORCE_VBUS_VALID */

    	uint32_t temp = *usb_reg_iscr;

        temp |= (1 << 6);
       	temp |= (1 << 5);
        temp |= (1 << 4);
        temp &= ~(1 << 3);

	/*
	#define  USBC_BP_ISCR_VBUS_CHANGE_DETECT        6
	#define  USBC_BP_ISCR_ID_CHANGE_DETECT          5
	#define  USBC_BP_ISCR_DPDM_CHANGE_DETECT        4
	#define  USBC_BP_ISCR_IRQ_ENABLE                3
	#define  USBC_BP_ISCR_VBUS_CHANGE_DETECT_EN     2
	#define  USBC_BP_ISCR_ID_CHANGE_DETECT_EN       1
	*/

        *usb_reg_iscr = temp;

        /* Disable power */
        *usb_reg_pctl &= ~(1 << 0); /* SUSPEND_EN */
        *usb_reg_pctl &= ~(1 << 1); /* SUSPEND */
        *usb_reg_pctl &= ~(1 << 2); /* RESUME */
        *usb_reg_pctl &= ~(1 << 3); /* RESET */
        *usb_reg_pctl &= ~(1 << 4); /* HIGH_SPEED_FLAG */
        *usb_reg_pctl &= ~(1 << 5); /* HIGH_SPEED_EN */

	return (0);
}

static device_method_t ehci_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, a10_ehci_probe),
	DEVMETHOD(device_attach, a10_ehci_attach),
	DEVMETHOD(device_detach, a10_ehci_detach),
	DEVMETHOD(device_suspend, bus_generic_suspend),
	DEVMETHOD(device_resume, bus_generic_resume),
	DEVMETHOD(device_shutdown, bus_generic_shutdown),

	DEVMETHOD_END
};

static driver_t ehci_driver = {
	.name = "ehci",
	.methods = ehci_methods,
	.size = sizeof(ehci_softc_t),
};

static devclass_t ehci_devclass;

DRIVER_MODULE(ehci, simplebus, ehci_driver, ehci_devclass, 0, 0);
MODULE_DEPEND(ehci, usb, 1, 1, 1);
