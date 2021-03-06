/*
* (C) Copyright 2007-2012
* Allwinner Technology Co., Ltd. <www.allwinnertech.com>
* Tom Cubie <tangliang@allwinnertech.com>
*
* See file CREDITS for list of people who contributed to this
* project.
*
* This program is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License as
* published by the Free Software Foundation; either version 2 of
* the License, or (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston,
* MA 02111-1307 USA
*/
 
#ifndef _A10_GPIO_H
#define _A10_GPIO_H

#define A10_PIO_BASE		0xe1c20800

/*
* A10 have 9 banks of gpio:
* PA0 - PA17 | PB0 - PB23 | PC0 - PC24
* PD0 - PD27 | PE0 - PE31 | PF0 - PF5
* PG0 - PG9 | PH0 - PH27 | PI0 - PI12
*/

#define A10_GPIO_A		0
#define A10_GPIO_B		1
#define A10_GPIO_C		2
#define A10_GPIO_D		3
#define A10_GPIO_E		4
#define A10_GPIO_F		5
#define A10_GPIO_G		6
#define A10_GPIO_H		7
#define A10_GPIO_I		8

struct a10_gpio {
	uint32_t cfg[4];
	uint32_t dat;
	uint32_t drv[2];
	uint32_t pull[2];
};

/* gpio interrupt control */
struct a10_gpio_int {
	uint32_t cfg[3];
	uint32_t ctl;
	uint32_t sta;
	uint32_t deb;		/* interrupt debounce */
};

struct a10_gpio_reg {
	struct a10_gpio gpio_bank[9];
	uint8_t res[0xbc];
	struct a10_gpio_int gpio_int;
};

#define GPIO_BANK(pin)		((pin) >> 5)
#define GPIO_NUM(pin)		((pin) & 0x1f)

#define GPIO_CFG_INDEX(pin)	(((pin) & 0x1f) >> 3)
#define GPIO_CFG_OFFSET(pin)	((((pin) & 0x1f) & 0x7) << 2)

#define GPIO_PULL_INDEX(pin)	(((pin) & 0x1f) >> 4)
#define GPIO_PULL_OFFSET(pin)	((((pin) & 0x1f) & 0xf) << 1)


/* GPIO bank sizes */
#define A10_GPIO_A_NR		32
#define A10_GPIO_B_NR		32
#define A10_GPIO_C_NR		32
#define A10_GPIO_D_NR		32
#define A10_GPIO_E_NR		32
#define A10_GPIO_F_NR		32
#define A10_GPIO_G_NR		32
#define A10_GPIO_H_NR		32
#define A10_GPIO_I_NR		32

#define A10_GPIO_NEXT(__gpio) \
	((__gpio##_START) + (__gpio##_NR) + 0)

enum a10_gpio_number {
	A10_GPIO_A_START = 0,
	A10_GPIO_B_START = A10_GPIO_NEXT(A10_GPIO_A),
	A10_GPIO_C_START = A10_GPIO_NEXT(A10_GPIO_B),
	A10_GPIO_D_START = A10_GPIO_NEXT(A10_GPIO_C),
	A10_GPIO_E_START = A10_GPIO_NEXT(A10_GPIO_D),
	A10_GPIO_F_START = A10_GPIO_NEXT(A10_GPIO_E),
	A10_GPIO_G_START = A10_GPIO_NEXT(A10_GPIO_F),
	A10_GPIO_H_START = A10_GPIO_NEXT(A10_GPIO_G),
	A10_GPIO_I_START = A10_GPIO_NEXT(A10_GPIO_H),
};

/* A10 GPIO number definitions */
#define A10_GPA(_nr)		(A10_GPIO_A_START + (_nr))
#define A10_GPB(_nr)		(A10_GPIO_B_START + (_nr))
#define A10_GPC(_nr)		(A10_GPIO_C_START + (_nr))
#define A10_GPD(_nr)		(A10_GPIO_D_START + (_nr))
#define A10_GPE(_nr)		(A10_GPIO_E_START + (_nr))
#define A10_GPF(_nr)		(A10_GPIO_F_START + (_nr))
#define A10_GPG(_nr)		(A10_GPIO_G_START + (_nr))
#define A10_GPH(_nr)		(A10_GPIO_H_START + (_nr))
#define A10_GPI(_nr)		(A10_GPIO_I_START + (_nr))

/* GPIO pin function config */
#define A10_GPIO_INPUT		0
#define A10_GPIO_OUTPUT		1

#define A10_GPA0_ERXD3		2
#define A10_GPA0_SPI1_CS0	3
#define A10_GPA0_UART2_RTS	4

#define A10_GPA1_ERXD2		2
#define A10_GPA1_SPI1_CLK	3
#define A10_GPA1_UART2_CTS	4

#define A10_GPA2_ERXD1		2
#define A10_GPA2_SPI1_MOSI	3
#define A10_GPA2_UART2_TX	4

#define A10_GPA10_UART1_TX	4
#define A10_GPA11_UART1_RX	4

#define A10_GPB22_UART0_TX	2
#define A10_GPB23_UART0_RX	2

#define A10_GPC2_NCLE		2
#define A10_GPC2_SPI0_CLK	3

#define A10_GPC6_NRB0		2
#define A10_GPC6_SDC2_CMD	3

#define A10_GPC7_NRB1		2
#define A10_GPC7_SDC2_CLK	3

#define A10_GPC8_NDQ0		2
#define A10_GPC8_SDC2_D0	3

#define A10_GPC9_NDQ1		2
#define A10_GPC9_SDC2_D1	3

#define A10_GPC10_NDQ2		2
#define A10_GPC10_SDC2_D2	3

#define A10_GPC11_NDQ3		2
#define A10_GPC11_SDC2_D3	3

#define A10_GPF2_SDC0_CLK	2
#define A10_GPF2_UART0_TX	4

#define A10_GPF4_SDC0_D3	2
#define A10_GPF4_UART0_RX	4

int a10_gpio_set_cfgpin(uint32_t pin, uint32_t val);
int a10_gpio_get_cfgpin(uint32_t pin);
int a10_gpio_output(uint32_t pin, uint32_t val);
int a10_gpio_input(uint32_t pin);

int a10_gpio_request(unsigned gpio, const char *label);
int a10_gpio_free(unsigned gpio);
int a10_gpio_direction_input(unsigned gpio);
int a10_gpio_direction_output(unsigned gpio, int value);
int a10_gpio_get_value(unsigned gpio);
int a10_gpio_set_value(unsigned gpio, int value);

#endif /* _A10_GPIO_H */
