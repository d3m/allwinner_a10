/*
 * Copyright (C) 2013 Ganbold Tsagaankhuu
 * Copyright (C) 2010 Andrew Turner
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
 *
 */

/*
 * A simple driver for the A10 WEMAC, based on Davicom 9000
 */

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/mbuf.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/gpio.h>

#include <machine/bus.h>
#include <machine/resource.h>
#include <machine/intr.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/if_mib.h>
#include <net/ethernet.h>
#include <net/if_vlan_var.h>

#ifdef INET
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#endif

#include <net/bpf.h>
#include <net/bpfdesc.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>

#include <arm/allwinner/if_wemacreg.h>
#include <arm/allwinner/if_wemacvar.h>

#include "miibus_if.h"

#include "gpio_if.h"

struct wemac_softc {
	struct ifnet		*wemac_ifp;
	device_t		wemac_dev;
	device_t		wemac_miibus;
	bus_space_handle_t	wemac_handle;
	bus_space_tag_t		wemac_tag;
	struct resource		*wemac_res;
	struct resource		*wemac_irq;
	void			*wemac_intrhand;
#define WEMAC_FLAG_LINK		(1 << 0)
	uint32_t		wemac_flags;
	struct mtx		wemac_mtx;
	struct callout		wemac_tick_ch;
	int			wemac_tx_fifo_stat;
	int			wemac_watchdog_timer;
};

static int wemac_probe(device_t);
static int wemac_attach(device_t);
static int wemac_detach(device_t);
 
static void wemac_intr(void *);

static void wemac_watchdog(struct wemac_softc *);

static void wemac_init_locked(struct wemac_softc *);
static int wemac_miibus_readreg(device_t dev, int phy, int reg);
static int wemac_miibus_writereg(device_t dev, int phy, int reg, int data);
static void wemac_miibus_statchg(device_t);

#define WEMAC_MAX_FRAME_LEN	0x0600
#define WEMAC_PHY		0x100 /* PHY address 0x01 */

#define SW_CCM_AHB_GATING	0xe1c20060
#define AHB_GATE_OFFSET_EMAC	17

#define GPIO_WEMAC_PWR		243

#define REG_READ(reg)		(*(volatile uint32_t *)(reg))
#define REG_WRITE(reg, val)	(*(volatile uint32_t *)(reg) = (val))

#define wemac_read_reg(sc, reg)		\
	bus_space_read_4(sc->wemac_tag, sc->wemac_handle, reg)
#define wemac_write_reg(sc, reg, val)	\
	bus_space_write_4(sc->wemac_tag, sc->wemac_handle, reg, val)

static int emacrx_completed_flag = 1;

static void
wemac_reset(struct wemac_softc *sc)
{
	/* Send reset */
	wemac_write_reg(sc, EMAC_CTL, 0);
	DELAY(200);

	wemac_write_reg(sc, EMAC_CTL, EMAC_CTL_RST);
	DELAY(200); /* Wait for the MAC to reset */
}


static void
wemac_start_locked(struct ifnet *ifp)
{
	struct wemac_softc *sc;
	struct mbuf *m, *mp;
	int len, total_len;
	uint32_t channel, reg_val;

	sc = ifp->if_softc;

	channel = sc->wemac_tx_fifo_stat & 3;
	if (channel == 3)
		return;

	WEMAC_ASSERT_LOCKED(sc);

	/* Select channel */
	wemac_write_reg(sc, EMAC_TX_INS, channel);

	/* Set TX mode */
	wemac_write_reg(sc, EMAC_TX_MODE, EMAC_TX_TM);

	sc->wemac_tx_fifo_stat |= 1 << channel;

	if (ifp->if_drv_flags & IFF_DRV_OACTIVE)
		return;

	while (!IFQ_DRV_IS_EMPTY(&ifp->if_snd)) {
		IFQ_DRV_DEQUEUE(&ifp->if_snd, m);
		if (m == NULL)
			break;

		/*
		 * TODO: Fix the case where an mbuf is
		 * not a multiple of the write size.
		 */
		total_len = 0;
		for (mp = m; mp != NULL; mp = mp->m_next) {
			len = mp->m_len;

			/* Ignore empty parts */
			if (len == 0)
				continue;

			total_len += len;

			/* 
			 * XXX Write the data.
			 * Maybe need to try bus_space_write_multi_(1-4)).
			 */
			bus_space_write_multi_2(sc->wemac_tag, sc->wemac_handle,
			    EMAC_TX_IO_DATA, mtod(mp, uint16_t *), (len + 1) / 2);
		}

		/* 
		 * Send the data lengh and the packet.
		 * Start translate from fifo to phy.
		 */
		if (channel == 0) {
			wemac_write_reg(sc, EMAC_TX_PL0, total_len);

			reg_val = wemac_read_reg(sc, EMAC_TX_CTL0);
			reg_val |= 1;
			wemac_write_reg(sc, EMAC_TX_CTL0, reg_val);

		} else if (channel == 1) {
			wemac_write_reg(sc, EMAC_TX_PL1, total_len);

			reg_val = wemac_read_reg(sc, EMAC_TX_CTL1);
			reg_val |= 1;
			wemac_write_reg(sc, EMAC_TX_CTL1, reg_val);
		}

		if((sc->wemac_tx_fifo_stat & 3) == 3) {
			/* Second packet */
			IFQ_DRV_DEQUEUE(&ifp->if_snd, m);
		}

		BPF_MTAP(ifp, m);

		m_freem(m);
	}
	/* set timeout */
	sc->wemac_watchdog_timer = 5;
}

static void
wemac_start(struct ifnet *ifp)
{
	struct wemac_softc *sc;

	sc = ifp->if_softc;
	WEMAC_LOCK(sc);
	wemac_start_locked(ifp);
	WEMAC_UNLOCK(sc);
}

static void
wemac_stop(struct wemac_softc *sc)
{
	WEMAC_ASSERT_LOCKED(sc);
	callout_stop(&sc->wemac_tick_ch);
}

static int
wemac_rxeof(struct wemac_softc *sc)
{
	struct ifnet *ifp;
	struct mbuf *m;
	int len;
	uint32_t reg_val;

	WEMAC_ASSERT_LOCKED(sc);

	ifp = sc->wemac_ifp;

	/* Read the first byte to check it correct */
	reg_val = wemac_read_reg(sc, EMAC_RX_FBC);
	if (!reg_val) {
		emacrx_completed_flag = 1;
		reg_val = wemac_read_reg(sc, EMAC_INT_CTL);
		reg_val |= (0xf << 0) | (0x01 << 8);
		wemac_write_reg(sc, EMAC_INT_CTL, reg_val);

		/* Had one stuck? */
		reg_val = wemac_read_reg(sc, EMAC_RX_FBC);
		if (!reg_val)
			return -1;
	}

	/* Read the data */
	reg_val = wemac_read_reg(sc, EMAC_RX_IO_DATA);
	len = reg_val & 0xFFFF;

	if (reg_val != 0x0143414d) {
		/* Disable RX */
		reg_val = wemac_read_reg(sc, EMAC_CTL);
		reg_val &= ~EMAC_CTL_RX_EN;
		wemac_write_reg(sc, EMAC_CTL, reg_val);

		/* Flush RX FIFO */
		reg_val = wemac_read_reg(sc, EMAC_RX_CTL);
		reg_val |= (1 << 3);
		wemac_write_reg(sc, EMAC_RX_CTL, reg_val);
		while (wemac_read_reg(sc, EMAC_RX_CTL) & (1 << 3))
			;

		/* Enable RX */
		reg_val = wemac_read_reg(sc, EMAC_CTL);
		reg_val |= EMAC_CTL_RX_EN;
		wemac_write_reg(sc, EMAC_CTL, reg_val);

		reg_val = wemac_read_reg(sc, EMAC_INT_CTL);
		reg_val |= (0xf << 0) | (0x01 << 8);
		wemac_write_reg(sc, EMAC_INT_CTL, reg_val);

		emacrx_completed_flag = 1;

		return 0;
	}

	m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return (ENOBUFS);

	m->m_pkthdr.rcvif = ifp;
	m->m_len = m->m_pkthdr.len = len;
	m_adj(m, ETHER_ALIGN);

	/* XXX Read the data (maybe need to try bus_space_read_multi_(1-4)) */
	bus_space_read_multi_2(sc->wemac_tag, sc->wemac_handle, EMAC_RX_IO_DATA,
	    mtod(m, uint16_t *), (len + 1) / 2);

	ifp->if_ipackets++;
	WEMAC_UNLOCK(sc);
	(*ifp->if_input)(ifp, m);
	WEMAC_LOCK(sc);

	return 0;
}

static void
wemac_watchdog(struct wemac_softc *sc)
{
	struct ifnet *ifp;

	WEMAC_ASSERT_LOCKED(sc);

	if (sc->wemac_watchdog_timer == 0 || --sc->wemac_watchdog_timer)
		return;

	ifp = sc->wemac_ifp;
	if_printf(sc->wemac_ifp, "watchdog timeout -- resetting\n");
	ifp->if_oerrors++;
	ifp->if_drv_flags &= ~IFF_DRV_RUNNING;
	wemac_init_locked(sc);
	if (!IFQ_DRV_IS_EMPTY(&ifp->if_snd))
		wemac_start_locked(ifp);
}

static void
wemac_tick(void *arg)
{
	struct wemac_softc *sc;
	struct mii_data *mii;

	sc = (struct wemac_softc *)arg;

	mii = device_get_softc(sc->wemac_miibus);
	mii_tick(mii);
        if ((sc->wemac_flags & WEMAC_FLAG_LINK) == 0)
                wemac_miibus_statchg(sc->wemac_dev);

	wemac_watchdog(sc);

	/* Read the packets off the device */
	while (wemac_rxeof(sc) == 0)
		continue;

	callout_reset(&sc->wemac_tick_ch, hz/100, wemac_tick, sc);
}

static void
wemac_intr(void *arg)
{
        struct wemac_softc *sc = (struct wemac_softc *)arg;
        uint32_t intstatus, reg_val;

	/* Disable all interrupts */
	wemac_write_reg(sc, EMAC_INT_CTL, 0);

	/* Got WEMAC interrupt status */
	/* Got ISR */
	intstatus = wemac_read_reg(sc, EMAC_INT_STA);

	/* Clear ISR status */
	wemac_write_reg(sc, EMAC_INT_STA, intstatus);

	WEMAC_ASSERT_LOCKED(sc);

	/* Received the coming packet */
	if ((intstatus & 0x100) && (emacrx_completed_flag == 1)) {
		/* carrier lost */
		emacrx_completed_flag = 0;
		wemac_rxeof(sc);
	}

	/* Transmit Interrupt check */
	if (intstatus & (0x01 | 0x02))
		wemac_tx_done(sc, intstatus);

	/* Re-enable interrupt mask */
	if (emacrx_completed_flag == 1) {
		reg_val = wemac_read_reg(sc, EMAC_INT_CTL);
		reg_val |= (0xf << 0) | (0x01 << 8);
		wemac_write_reg(sc, EMAC_INT_CTL, reg_val);
	}
}

static void
wemac_setmode(struct wemac_softc *sc)
{
}

static int
wemac_ioctl(struct ifnet *ifp, u_long command, caddr_t data)
{
	struct wemac_softc *sc;
	struct mii_data *mii;
	struct ifreq *ifr;
	int error = 0;

	sc = ifp->if_softc;
	ifr = (struct ifreq *)data;

	switch (command) {
	case SIOCSIFFLAGS:
		/*
		 * Switch interface state between "running" and
		 * "stopped", reflecting the UP flag.
		 */
		WEMAC_LOCK(sc);
		if (ifp->if_flags & IFF_UP) {
			if ((ifp->if_drv_flags & IFF_DRV_RUNNING)==0) {
				wemac_init_locked(sc);
			}
		} else {
			if ((ifp->if_drv_flags & IFF_DRV_RUNNING) != 0) {
				wemac_stop(sc);
			}
		}		
		wemac_setmode(sc);
		WEMAC_UNLOCK(sc);
		break;
	case SIOCGIFMEDIA:
	case SIOCSIFMEDIA:
		mii = device_get_softc(sc->wemac_miibus);
		error = ifmedia_ioctl(ifp, ifr, &mii->mii_media, command);
		break;
	default:
		error = ether_ioctl(ifp, command, data);
		break;
	}
	return (error);
}

static void wemac_init_locked(struct wemac_softc *sc)
{
	struct ifnet *ifp = sc->wemac_ifp;
	uint32_t reg_val;
	int phy_reg;
	device_t dev;

	dev = sc->wemac_dev;

	wemac_reset(sc);

	/* PHY POWER UP */
	phy_reg = wemac_miibus_readreg(dev, WEMAC_PHY, 0);
	wemac_miibus_writereg(dev, WEMAC_PHY, 0, phy_reg & (~(1 << 11)));
	DELAY(1000);

	phy_reg = wemac_miibus_readreg(dev, WEMAC_PHY, 0);

	/* set EMAC SPEED, depend on PHY */
	reg_val = wemac_read_reg(sc, EMAC_MAC_SUPP);
	reg_val &= (~(0x1 << 8));
	reg_val |= (((phy_reg & (1 << 13)) >> 13) << 8);
	wemac_write_reg(sc, EMAC_MAC_SUPP, reg_val);

	/* set duplex depend on phy */
	reg_val = wemac_read_reg(sc, EMAC_MAC_CTL1);
	reg_val &= (~(0x1 << 0));
	reg_val |= (((phy_reg & (1 << 8)) >> 8) << 0);
	wemac_write_reg(sc, EMAC_MAC_CTL1, reg_val);

	/* enable RX/TX */
	reg_val = wemac_read_reg(sc, EMAC_CTL);
	wemac_write_reg(sc, EMAC_CTL, reg_val | EMAC_CTL_RST | EMAC_CTL_TX_EN | EMAC_CTL_RX_EN);

	/* enable RX/TX0/RX Hlevel interrupt */
	reg_val = wemac_read_reg(sc, EMAC_INT_CTL);
	reg_val |= (0xf << 0) | (0x01 << 8);
	wemac_write_reg(sc, EMAC_INT_CTL, reg_val);

	/* Set up TX */
	reg_val = wemac_read_reg(sc, EMAC_TX_MODE);
	reg_val |= EMAC_TX_AB_M;
	wemac_write_reg(sc, EMAC_TX_MODE, reg_val);

	/* Set up RX */
	reg_val = wemac_read_reg(sc, EMAC_RX_CTL);
	reg_val |= EMAC_RX_SETUP;
	wemac_write_reg(sc, EMAC_RX_CTL, reg_val);

	ifp->if_drv_flags |= IFF_DRV_RUNNING;
	ifp->if_drv_flags &= ~IFF_DRV_OACTIVE;

	sc->wemac_tx_fifo_stat = 0;

	callout_reset(&sc->wemac_tick_ch, hz/100, wemac_tick, sc);
}

static void
wemac_init(void *xcs)
{
	struct wemac_softc *sc = xcs;

	WEMAC_LOCK(sc);
	wemac_init_locked(sc);
	WEMAC_UNLOCK(sc);
}

static int
wemac_ifmedia_upd(struct ifnet *ifp)
{
	struct wemac_softc *sc;
	struct mii_data *mii;

	sc = ifp->if_softc;
	mii = device_get_softc(sc->wemac_miibus);

	WEMAC_LOCK(sc);
	mii_mediachg(mii);
	WEMAC_UNLOCK(sc);

	return (0);
}

static void
wemac_ifmedia_sts(struct ifnet *ifp, struct ifmediareq *ifmr)
{
	struct wemac_softc *sc;
	struct mii_data *mii;

	sc = ifp->if_softc;
	mii = device_get_softc(sc->wemac_miibus);

	WEMAC_LOCK(sc);
	mii_pollstat(mii);
	ifmr->ifm_active = mii->mii_media_active;
	ifmr->ifm_status = mii->mii_media_status;
	WEMAC_UNLOCK(sc);
}

static int
wemac_probe(device_t dev)
{

	if (!ofw_bus_is_compatible(dev, "allwinner,wemac"))
		return (ENXIO);

	device_set_desc(dev, "Allwinner A10 WEMAC");
	return (0);
}

static int
wemac_attach(device_t dev)
{
	uint8_t eaddr[ETHER_ADDR_LEN];
	struct wemac_softc *sc;
        device_t sc_gpio_dev;
	struct ifnet *ifp;
	int error, rid;
	uint32_t reg_val;
	int phy_val;
	uint32_t duplex_flag;

	sc = device_get_softc(dev);
	sc->wemac_dev = dev;

	error = 0;

	mtx_init(&sc->wemac_mtx, device_get_nameunit(dev), MTX_NETWORK_LOCK,
	    MTX_DEF);
	callout_init_mtx(&sc->wemac_tick_ch, &sc->wemac_mtx, 0);

	rid = 0;
	sc->wemac_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);

	if (sc->wemac_res == NULL) {
		device_printf(dev, "unable to map memory\n");
		error = ENXIO;
		goto fail;
	}

	sc->wemac_tag = rman_get_bustag(sc->wemac_res);
	sc->wemac_handle = rman_get_bushandle(sc->wemac_res);

	/* Get the GPIO device, we need this to give power to wemac */
	sc_gpio_dev = devclass_get_device(devclass_find("gpio"), 0);
	if (sc_gpio_dev == NULL) {
		device_printf(dev, "Error: failed to get the GPIO device\n");
		error = ENXIO;
		goto fail;
	}

	/* Give power to wemac */
	GPIO_PIN_SETFLAGS(sc_gpio_dev, GPIO_WEMAC_PWR, GPIO_PIN_OUTPUT);
	GPIO_PIN_SET(sc_gpio_dev, GPIO_WEMAC_PWR, GPIO_PIN_HIGH);

	/* initial EMAC */
	/* flush RX FIFO */
	reg_val = wemac_read_reg(sc, EMAC_RX_CTL);
	reg_val |= 0x8;
	wemac_write_reg(sc, EMAC_RX_CTL, reg_val);
	DELAY(1);

	/* soft reset MAC */
	reg_val = wemac_read_reg(sc, EMAC_MAC_CTL0);
	reg_val &= ~EMAC_MAC_CTL0_SOFT_RST;
	wemac_write_reg(sc, EMAC_MAC_CTL0, reg_val);

	/* set MII clock */
	reg_val = wemac_read_reg(sc, EMAC_MAC_MCFG);
	reg_val &= (~(0xf << 2));
	reg_val |= (0xd << 2);
	wemac_write_reg(sc, EMAC_MAC_MCFG, reg_val);

	/* clear RX counter */
	wemac_write_reg(sc, EMAC_RX_FBC, 0);

	/* disable all interrupt and clear interrupt status */
	wemac_write_reg(sc, EMAC_INT_CTL, 0);
	reg_val = wemac_read_reg(sc, EMAC_INT_STA);
	wemac_write_reg(sc, EMAC_INT_STA, reg_val);

	DELAY(1);

	/* 
	 * Set up EMAC.
	 * Set MAC CTL0. 
	 */
	reg_val = wemac_read_reg(sc, EMAC_MAC_CTL0);
	reg_val |= EMAC_MAC_CTL0_SETUP;
	wemac_write_reg(sc, EMAC_MAC_CTL0, reg_val);

	/* Set MAC CTL1 */
	reg_val = wemac_read_reg(sc, EMAC_MAC_CTL1);
	phy_val = wemac_miibus_readreg(dev, WEMAC_PHY, 0);
	duplex_flag = !!(phy_val & EMAC_PHY_DUPLEX);

	if (duplex_flag)
		reg_val |= EMAC_MAC_CTL1_DUP;

	reg_val |= EMAC_MAC_CTL1_SETUP;
	wemac_write_reg(sc, EMAC_MAC_CTL1, reg_val);

	/* Set up IPGT */
	wemac_write_reg(sc, EMAC_MAC_IPGT, EMAC_MAC_IPGT_FD);

	/* Set up IPGR */
	wemac_write_reg(sc, EMAC_MAC_IPGR, EMAC_MAC_NBTB_IPG2 | (EMAC_MAC_NBTB_IPG1 << 8));

	/* Set up Collison window */
	wemac_write_reg(sc, EMAC_MAC_CLRT, EMAC_MAC_RM | (EMAC_MAC_CW << 8));

	/* Set up Max Frame Length */
	wemac_write_reg(sc, EMAC_MAC_MAXF, EMAC_MAC_MFL);

	/* Reset */
	wemac_reset(sc);

	ifp = sc->wemac_ifp = if_alloc(IFT_ETHER);
	if (ifp == NULL) {
		device_printf(dev, "unable to allocate ifp\n");
		error = ENOSPC;
		goto fail;
	}
	ifp->if_softc = sc;

	/* Setup MII */
	error = mii_attach(dev, &sc->wemac_miibus, ifp, wemac_ifmedia_upd,
	    wemac_ifmedia_sts, BMSR_DEFCAPMASK, MII_PHY_ANY, MII_OFFSET_ANY, 0);

	if (error != 0) {
		device_printf(dev, "PHY probe failed\n");
		goto fail;
	}

	/* Gating AHB clock for EMAC */
	REG_WRITE(SW_CCM_AHB_GATING, 1 << AHB_GATE_OFFSET_EMAC);

	if_initname(ifp, device_get_name(dev), device_get_unit(dev));
	ifp->if_flags = IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST;
	ifp->if_start = wemac_start;
	ifp->if_ioctl = wemac_ioctl;
	ifp->if_init = wemac_init;
	IFQ_SET_MAXLEN(&ifp->if_snd, IFQ_MAXLEN);

	/* XXX: Hardcode the ethernet address for now */
	eaddr[0] = 0x08;
	eaddr[1] = 0x08;
	eaddr[2] = 0x11;
	eaddr[3] = 0x18;
	eaddr[4] = 0x12;
	eaddr[5] = 0x27;

	/* Write ethernet address to register */
	wemac_write_reg(sc, EMAC_MAC_A1, eaddr[0] << 16 | eaddr[1] << 8 |
	    eaddr[2]);
	wemac_write_reg(sc, EMAC_MAC_A0, eaddr[3] << 16 | eaddr[4] << 8 |
	    eaddr[5]);

	ether_ifattach(ifp, eaddr);

	/* Enable RX/TX */
	wemac_write_reg(sc, EMAC_CTL, 0x7);

	/* VLAN capability setup. */
	ifp->if_capabilities |= IFCAP_VLAN_MTU;
	ifp->if_capenable = ifp->if_capabilities;
	/* Tell the upper layer we support VLAN over-sized frames. */
	ifp->if_hdrlen = sizeof(struct ether_vlan_header);

	error = bus_setup_intr(dev, sc->wemac_irq, INTR_TYPE_NET | INTR_MPSAFE,
	    NULL, wemac_intr, sc, &sc->wemac_intrhand);
	if (error != 0) {
		device_printf(dev, "could not set up interrupt handler.\n");
		ether_ifdetach(ifp);
		goto fail;
	}

fail:
	if (error != 0)
		wemac_detach(dev);
	return (error);
}

static int
wemac_detach(device_t dev)
{
	struct wemac_softc *sc;

	sc = device_get_softc(dev);
	KASSERT(mtx_initialized(&sc->wemac_mtx), ("wemac mutex not initialized"));

	/* TODO: Cleanup correctly */
	if (sc->wemac_res)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->wemac_res);

	mtx_destroy(&sc->wemac_mtx);

	return (0);
}

/*
 * The MII bus interface
 */
static int
wemac_miibus_readreg(device_t dev, int phy, int reg)
{
	struct wemac_softc *sc;
	int rval;

	/* We have up to 4 PHY's */
	if (phy >= 4)
		return (0);

	sc = device_get_softc(dev);

	/* issue the phy address and reg */
	wemac_write_reg(sc, EMAC_MAC_MADR, WEMAC_PHY | reg);

	/* pull up the phy io line */
	wemac_write_reg(sc, EMAC_MAC_MCMD, 0x1);
	DELAY(1000);

	/* push down the phy io line */
	wemac_write_reg(sc, EMAC_MAC_MCMD, 0x0);

	/* and read data */
	rval = wemac_read_reg(sc, EMAC_MAC_MRDD);

	return (rval);
}

static int
wemac_miibus_writereg(device_t dev, int phy, int reg, int data)
{
	struct wemac_softc *sc;

	/* We have up to 4 PHY's */
	if (phy > 3)
		return (0);

	sc = device_get_softc(dev);

	/* issue the phy address and reg */
	wemac_write_reg(sc, EMAC_MAC_MADR, WEMAC_PHY | reg);

	/* pull up the phy io line */
	wemac_write_reg(sc, EMAC_MAC_MCMD, 0x1);
	DELAY(1000);

	/* push down the phy io line */
	wemac_write_reg(sc, EMAC_MAC_MCMD, 0x0);

	/* and write data */
	wemac_write_reg(sc, EMAC_MAC_MWTD, data);

	return (0);
}

static void
wemac_miibus_statchg(device_t dev)
{
	struct wemac_softc *sc;
	struct mii_data *mii;
	struct ifnet *ifp;

	sc = device_get_softc(dev);

	mii = device_get_softc(sc->wemac_miibus);
	ifp = sc->wemac_ifp;
	if ((ifp->if_drv_flags & IFF_DRV_RUNNING) == 0)
		return;

	if ((mii->mii_media_status & (IFM_ACTIVE | IFM_AVALID)) ==
	    (IFM_ACTIVE | IFM_AVALID))
		sc->wemac_flags |= WEMAC_FLAG_LINK;
	else
		sc->wemac_flags &= ~WEMAC_FLAG_LINK;
}


static device_method_t wemac_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		wemac_probe),
	DEVMETHOD(device_attach,	wemac_attach),
	DEVMETHOD(device_detach,	wemac_detach),

	/* bus interface, for miibus */
	DEVMETHOD(bus_print_child,	bus_generic_print_child),
	DEVMETHOD(bus_driver_added,	bus_generic_driver_added),

	/* MII interface */
	DEVMETHOD(miibus_readreg,	wemac_miibus_readreg),
	DEVMETHOD(miibus_writereg,	wemac_miibus_writereg),
	DEVMETHOD(miibus_statchg,	wemac_miibus_statchg),

	DEVMETHOD_END
};

static driver_t wemac_driver = {
	"wemac",
	wemac_methods,
	sizeof(struct wemac_softc)
};

static devclass_t wemac_devclass;

DRIVER_MODULE(wemac, simplebus, wemac_driver, wemac_devclass, 0, 0);
DRIVER_MODULE(miibus, wemac, miibus_driver, miibus_devclass, 0, 0);
MODULE_DEPEND(wemac, miibus, 1, 1, 1);
MODULE_DEPEND(wemac, ether, 1, 1, 1);

