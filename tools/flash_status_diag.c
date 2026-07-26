#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#define TEST_FLASH_OFFSET (0x001ff000u)

volatile uint32_t flash_diag_magic;
volatile uint8_t flash_diag_jedec[3];
volatile uint8_t flash_diag_sr1;
volatile uint8_t flash_diag_sr2;
volatile uint8_t flash_diag_sr3;
volatile uint8_t flash_diag_sr3_after_wps_enable;
volatile uint8_t flash_diag_sr3_after_restore;
volatile uint8_t flash_diag_lock_first_sector;
volatile uint8_t flash_diag_lock_test_sector;
volatile uint8_t flash_diag_lock_first_sector_after_unlock;
volatile uint8_t flash_diag_lock_test_sector_after_unlock;
volatile uint32_t flash_test_stage;
volatile uint32_t flash_test_program_mismatches;
volatile uint32_t flash_test_initial_erase_non_ff;
volatile uint32_t flash_test_erase_non_ff;
volatile uint8_t flash_test_sr1_after_wren;
volatile uint8_t flash_test_sr1_after_wrdi;
volatile uint8_t flash_test_sr1_after_program;
volatile uint8_t flash_test_sr1_after_erase;
volatile uint32_t flash_test_page_checksum_before;
volatile uint32_t flash_test_page_checksum_after;
volatile uint32_t flash_test_initial_erase_busy_polls;
volatile uint32_t flash_test_program_busy_polls;
volatile uint32_t flash_test_cleanup_erase_busy_polls;
volatile uint32_t flash_test_continue_after_program;
static uint8_t flash_test_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
static uint8_t flash_test_tx[FLASH_PAGE_SIZE + 4] __attribute__((aligned(4)));
static uint8_t flash_test_rx[FLASH_PAGE_SIZE + 4] __attribute__((aligned(4)));

static uint8_t read_status_register(uint8_t command)
{
    uint8_t tx[2] = {command, 0};
    uint8_t rx[2] = {0};

    flash_do_cmd(tx, rx, sizeof(tx));
    return rx[1];
}

static uint8_t read_block_lock(uint32_t flash_offset)
{
    uint8_t tx[5] = {
        0x3d,
        (uint8_t)(flash_offset >> 16),
        (uint8_t)(flash_offset >> 8),
        (uint8_t)flash_offset,
        0
    };
    uint8_t rx[5] = {0};

    flash_do_cmd(tx, rx, sizeof(tx));
    return rx[4];
}

static uint32_t wait_until_flash_ready(void)
{
    uint32_t polls = 0;

    while ((read_status_register(0x05) & 0x01u) != 0u) {
        if (++polls >= 15000u) {
            return polls | 0x80000000u;
        }
        busy_wait_us_32(1000);
    }
    return polls;
}

static void global_block_unlock(void)
{
    uint8_t tx[1] = {0x06};
    uint8_t rx[1] = {0};

    flash_do_cmd(tx, rx, sizeof(tx));
    tx[0] = 0x98;
    flash_do_cmd(tx, rx, sizeof(tx));
    (void)wait_until_flash_ready();
}

static void write_status_register_3(uint8_t value)
{
    uint8_t tx[2] = {0x06, 0};
    uint8_t rx[2] = {0};

    flash_do_cmd(tx, rx, 1);
    tx[0] = 0x11;
    tx[1] = value;
    flash_do_cmd(tx, rx, sizeof(tx));
    (void)wait_until_flash_ready();
}

static void direct_page_program(uint32_t flash_offset, uint8_t const *page)
{
    uint8_t write_enable_tx[1] = {0x06};
    uint8_t write_enable_rx[1] = {0};

    flash_do_cmd(write_enable_tx, write_enable_rx, sizeof(write_enable_tx));

    flash_test_tx[0] = 0x02;
    flash_test_tx[1] = (uint8_t)(flash_offset >> 16);
    flash_test_tx[2] = (uint8_t)(flash_offset >> 8);
    flash_test_tx[3] = (uint8_t)flash_offset;
    memcpy(&flash_test_tx[4], page, FLASH_PAGE_SIZE);
    memset(flash_test_rx, 0, sizeof(flash_test_rx));
    flash_do_cmd(flash_test_tx, flash_test_rx, sizeof(flash_test_tx));
}

int main(void)
{
    stdio_init_all();
    sleep_ms(100);

    uint8_t jedec_tx[4] = {0x9f, 0, 0, 0};
    uint8_t jedec_rx[4] = {0};
    flash_do_cmd(jedec_tx, jedec_rx, sizeof(jedec_tx));

    uint8_t const sr1 = read_status_register(0x05);
    uint8_t const sr2 = read_status_register(0x35);
    uint8_t const sr3 = read_status_register(0x15);

    flash_diag_jedec[0] = jedec_rx[1];
    flash_diag_jedec[1] = jedec_rx[2];
    flash_diag_jedec[2] = jedec_rx[3];
    flash_diag_sr1 = sr1;
    flash_diag_sr2 = sr2;
    flash_diag_sr3 = sr3;
    flash_diag_lock_first_sector = read_block_lock(0);
    flash_diag_lock_test_sector = read_block_lock(TEST_FLASH_OFFSET);
    write_status_register_3((uint8_t)(sr3 | 0x04u));
    flash_diag_sr3_after_wps_enable = read_status_register(0x15);
    global_block_unlock();
    flash_diag_lock_first_sector_after_unlock = read_block_lock(0);
    flash_diag_lock_test_sector_after_unlock =
        read_block_lock(TEST_FLASH_OFFSET);
    flash_diag_magic = 0xd1a60001u;

    printf("\n=== RP2040 RAM-ONLY FLASH STATUS DIAGNOSTIC ===\n");
    printf("JEDEC ID: %02x %02x %02x\n",
           jedec_rx[1], jedec_rx[2], jedec_rx[3]);
    printf("SR1=0x%02x [BUSY=%u WEL=%u BP=%u TB=%u SEC=%u SRP0=%u]\n",
           sr1,
           (sr1 >> 0) & 1u,
           (sr1 >> 1) & 1u,
           (sr1 >> 2) & 7u,
           (sr1 >> 5) & 1u,
           (sr1 >> 6) & 1u,
           (sr1 >> 7) & 1u);
    printf("SR2=0x%02x [SRP1=%u QE=%u CMP=%u]\n",
           sr2,
           (sr2 >> 0) & 1u,
           (sr2 >> 1) & 1u,
           (sr2 >> 6) & 1u);
    printf("SR3=0x%02x [WPS=%u]\n", sr3, (sr3 >> 2) & 1u);
    fflush(stdout);

    /*
     * Keep program data in main SRAM. RP2040 boot-ROM flash routines use the
     * scratch banks, so a page allocated on the stack can be overwritten
     * before ROM_FUNC_FLASH_RANGE_PROGRAM consumes it.
     */
    for (uint32_t i = 0; i < sizeof(flash_test_page); ++i) {
        flash_test_page[i] = 0;
        flash_test_page_checksum_before += flash_test_page[i];
    }

    uint8_t command_tx[1] = {0x06};
    uint8_t command_rx[1] = {0};
    flash_do_cmd(command_tx, command_rx, sizeof(command_tx));
    flash_test_sr1_after_wren = read_status_register(0x05);
    command_tx[0] = 0x04;
    flash_do_cmd(command_tx, command_rx, sizeof(command_tx));
    flash_test_sr1_after_wrdi = read_status_register(0x05);

    uint32_t const initial_erase_interrupt_state = save_and_disable_interrupts();
    flash_test_stage = 1;
    flash_range_erase(TEST_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_test_stage = 2;
    restore_interrupts(initial_erase_interrupt_state);
    flash_test_initial_erase_busy_polls = wait_until_flash_ready();

    uint8_t const *const initially_erased =
        (uint8_t const *)(XIP_BASE + TEST_FLASH_OFFSET);
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; ++i) {
        if (initially_erased[i] != 0xffu) {
            ++flash_test_initial_erase_non_ff;
        }
    }
    flash_test_stage = 3;

    uint32_t const program_interrupt_state = save_and_disable_interrupts();
    flash_test_stage = 4;
    flash_range_program(TEST_FLASH_OFFSET, flash_test_page,
                        sizeof(flash_test_page));
    flash_test_stage = 5;
    restore_interrupts(program_interrupt_state);
    flash_test_program_busy_polls = wait_until_flash_ready();
    for (uint32_t i = 0; i < sizeof(flash_test_page); ++i) {
        flash_test_page_checksum_after += flash_test_page[i];
    }
    flash_test_sr1_after_program = read_status_register(0x05);

    uint8_t const *const programmed =
        (uint8_t const *)(XIP_BASE + TEST_FLASH_OFFSET);
    for (uint32_t i = 0; i < sizeof(flash_test_page); ++i) {
        if (programmed[i] != flash_test_page[i]) {
            ++flash_test_program_mismatches;
        }
    }
    flash_test_stage = 6;

    while (flash_test_continue_after_program == 0u) {
        tight_loop_contents();
    }

    uint32_t const erase_interrupt_state = save_and_disable_interrupts();
    flash_test_stage = 7;
    flash_range_erase(TEST_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_test_stage = 8;
    restore_interrupts(erase_interrupt_state);
    flash_test_cleanup_erase_busy_polls = wait_until_flash_ready();
    flash_test_sr1_after_erase = read_status_register(0x05);

    uint8_t const *const erased =
        (uint8_t const *)(XIP_BASE + TEST_FLASH_OFFSET);
    for (uint32_t i = 0; i < FLASH_SECTOR_SIZE; ++i) {
        if (erased[i] != 0xffu) {
            ++flash_test_erase_non_ff;
        }
    }
    write_status_register_3((uint8_t)(sr3 & (uint8_t)~0x04u));
    flash_diag_sr3_after_restore = read_status_register(0x15);
    flash_test_stage = 10;
    printf("WRITE TEST: initial_erase_non_ff=%lu program_mismatches=%lu "
           "erase_non_ff=%lu\n",
           (unsigned long)flash_test_initial_erase_non_ff,
           (unsigned long)flash_test_program_mismatches,
           (unsigned long)flash_test_erase_non_ff);
    fflush(stdout);

    while (true) {
        tight_loop_contents();
    }
}
