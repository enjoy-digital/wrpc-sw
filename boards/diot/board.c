/*
 * This work is part of the White Rabbit project
 *
 * Copyright (C) 2026 CERN (www.cern.ch)
 * Author: Alen Arias Vazquez <alen.arias.vazquez@cern.ch>
 *
 * Released according to the GNU LGPL, version 2.1 or any later version.
 */
#include <stdint.h>

#include "board-config.h"
#include "storage.h"
#include "dev/bb_i2c.h"
#include "dev/endpoint.h"
#include "dev/i2c_eeprom.h"
#include "dev/syscon.h"
#include "wrc-debug.h"

static struct i2c_eeprom_device mac_eeprom;
static struct i2c_eeprom_device storage_eeprom;
static struct i2c_bus i2c_wrc;

int wrc_board_early_init(void)
{

    board_dbg("WRC early board init.\n");
    bb_i2c_create(&i2c_wrc, &pin_sysc_fmc_scl, &pin_sysc_fmc_sda);
    bb_i2c_init(&i2c_wrc);

    board_dbg("Creating storage device.\n");
    i2c_eeprom_create(&storage_eeprom, &i2c_wrc, I2C_STORAGE_EEPROM_ADR, 2);
    storage_i2ceeprom_create(&wrc_storage_dev, &storage_eeprom);

    board_dbg("Mounting storage device.\n");
    storage_mount(&wrc_storage_dev);

    return 0;
}

static int board_get_persistent_mac(uint8_t *mac)
{

    board_dbg("Getting MAC address.\n");
    i2c_eeprom_create(&mac_eeprom, &i2c_wrc, I2C_MAC_EEPROM_ADR, 0);
    if (i2c_eeprom_read(&mac_eeprom, I2C_MAC_EEPROM_OFFSET, mac, 6) != 6) {
        mac[0] = 0x22;
        mac[1] = 0x33;
        mac[2] = 0x44;
        mac[3] = 0x55;
        mac[4] = 0x66;
        mac[5] = 0x77;
        return -1;
    }
    board_dbg("MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return 0;
}

int wrc_board_init()
{
    uint8_t mac_addr[6];
    /*
     * Try reading MAC addr stored in flash
     */
    if (board_get_persistent_mac(mac_addr) < 0) {
        board_dbg("Failed to get MAC address from the flash. Using fallback address.\n");
    }
    ep_set_mac_addr(&wrc_endpoint_dev, mac_addr);
    ep_pfilter_init_default(&wrc_endpoint_dev);

    return 0;
}
