#include "board_cal_store.h"
#include "stm32g474_bare.h"
#include <stddef.h>

#define FLASH_CS_PIN 9U
#define SRAM_CS_PIN 2U
#define STORE_SLOT0_ADDRESS 0x00FFE000UL
#define STORE_SLOT1_ADDRESS 0x00FFF000UL
#define FAULT_SLOT_BASE_ADDRESS 0x00FFA000UL
#define FAULT_SLOT_COUNT 4UL
#define FAULT_SECTOR_SIZE 0x1000UL
#define STORE_MAGIC 0x324C4143UL /* "CAL2" little-endian. */
#define STORE_COMMIT 0xA55A3CC3UL
#define FAULT_MAGIC 0x33544C46UL /* "FLT3" little-endian. */
#define FAULT_COMMIT 0x5AA5C33CUL
#define W25Q_JEDEC_ID 0x00EF4018UL
#define SPI_TIMEOUT 100000UL
#define FLASH_BUSY_TIMEOUT 2000000UL

typedef struct {
    uint32_t magic;
    uint32_t schema;
    uint32_t sequence;
    uint32_t payload_size;
    power_calibration_t calibration;
    uint32_t crc32;
    uint32_t commit;
} store_record_t;

typedef struct {
    uint32_t magic;
    uint32_t sequence;
    uint32_t payload_size;
    board_fault_record_t fault;
    uint32_t crc32;
    uint32_t commit;
} fault_store_record_t;

static board_cal_store_diag_t store_diag;

_Static_assert(sizeof(store_record_t) <= 256U,
               "Calibration record must fit one W25Q page");
_Static_assert(sizeof(fault_store_record_t) <= 256U,
               "Fault record must fit one W25Q page");

static void gpio_output(GPIO_TypeDef *port, uint32_t pin, uint32_t high)
{
    uint32_t shift = pin * 2U;
    port->BSRR = high ? (1UL << pin) : (1UL << (pin + 16U));
    port->MODER = (port->MODER & ~(3UL << shift)) | (1UL << shift);
    port->OTYPER &= ~(1UL << pin);
    port->OSPEEDR = (port->OSPEEDR & ~(3UL << shift)) | (2UL << shift);
    port->PUPDR &= ~(3UL << shift);
}

static uint8_t spi_transfer(uint8_t value, uint32_t *ok)
{
    uint32_t timeout = SPI_TIMEOUT;
    while ((SPI1->SR & SPI_SR_TXE) == 0UL && timeout-- != 0UL) {}
    if ((SPI1->SR & SPI_SR_TXE) == 0UL) { *ok = 0UL; return 0xFFU; }
    *(__IO uint8_t *)&SPI1->DR = value;
    timeout = SPI_TIMEOUT;
    while ((SPI1->SR & SPI_SR_RXNE) == 0UL && timeout-- != 0UL) {}
    if ((SPI1->SR & SPI_SR_RXNE) == 0UL) { *ok = 0UL; return 0xFFU; }
    return *(__IO uint8_t *)&SPI1->DR;
}

static void flash_select(void) { GPIOB->BSRR = 1UL << (FLASH_CS_PIN + 16U); }

static uint32_t flash_release(void)
{
    uint32_t timeout = SPI_TIMEOUT;
    while ((SPI1->SR & SPI_SR_BSY) != 0UL && timeout-- != 0UL) {}
    GPIOB->BSRR = 1UL << FLASH_CS_PIN;
    return ((SPI1->SR & SPI_SR_BSY) == 0UL) ? 1UL : 0UL;
}

static uint8_t read_status(uint32_t *ok)
{
    uint8_t status;
    flash_select();
    (void)spi_transfer(0x05U, ok);
    status = spi_transfer(0xFFU, ok);
    if (flash_release() == 0UL) *ok = 0UL;
    return status;
}

static uint32_t wait_ready(void)
{
    uint32_t timeout = FLASH_BUSY_TIMEOUT;
    uint32_t ok = 1UL;
    while (timeout-- != 0UL) {
        if ((read_status(&ok) & 1U) == 0U && ok != 0UL) return 1UL;
        IWDG_KR = 0xAAAAUL;
    }
    return 0UL;
}

static uint32_t write_enable(void)
{
    uint32_t ok = 1UL;
    flash_select();
    (void)spi_transfer(0x06U, &ok);
    if (flash_release() == 0UL) ok = 0UL;
    if (ok == 0UL) return 0UL;
    return ((read_status(&ok) & 2U) != 0U && ok != 0UL) ? 1UL : 0UL;
}

static uint32_t read_bytes(uint32_t address, uint8_t *data, uint32_t length)
{
    uint32_t i, ok = 1UL;
    if (data == (uint8_t *)0) return 0UL;
    flash_select();
    (void)spi_transfer(0x03U, &ok);
    (void)spi_transfer((uint8_t)(address >> 16), &ok);
    (void)spi_transfer((uint8_t)(address >> 8), &ok);
    (void)spi_transfer((uint8_t)address, &ok);
    for (i = 0UL; i < length && ok != 0UL; ++i)
        data[i] = spi_transfer(0xFFU, &ok);
    if (flash_release() == 0UL) ok = 0UL;
    return ok;
}

static uint32_t erase_sector(uint32_t address)
{
    uint32_t ok = 1UL;
    if (wait_ready() == 0UL || write_enable() == 0UL) return 0UL;
    flash_select();
    (void)spi_transfer(0x20U, &ok);
    (void)spi_transfer((uint8_t)(address >> 16), &ok);
    (void)spi_transfer((uint8_t)(address >> 8), &ok);
    (void)spi_transfer((uint8_t)address, &ok);
    if (flash_release() == 0UL) ok = 0UL;
    return (ok != 0UL && wait_ready() != 0UL) ? 1UL : 0UL;
}

static uint32_t page_program(uint32_t address, const uint8_t *data,
                             uint32_t length)
{
    uint32_t i, ok = 1UL;
    if (data == (const uint8_t *)0 || length == 0UL || length > 256UL ||
        ((address & 0xFFUL) + length) > 256UL) return 0UL;
    if (wait_ready() == 0UL || write_enable() == 0UL) return 0UL;
    flash_select();
    (void)spi_transfer(0x02U, &ok);
    (void)spi_transfer((uint8_t)(address >> 16), &ok);
    (void)spi_transfer((uint8_t)(address >> 8), &ok);
    (void)spi_transfer((uint8_t)address, &ok);
    for (i = 0UL; i < length && ok != 0UL; ++i)
        (void)spi_transfer(data[i], &ok);
    if (flash_release() == 0UL) ok = 0UL;
    return (ok != 0UL && wait_ready() != 0UL) ? 1UL : 0UL;
}

static uint32_t crc32_bytes(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL, i, bit;
    for (i = 0UL; i < length; ++i) {
        crc ^= data[i];
        for (bit = 0UL; bit < 8UL; ++bit)
            crc = (crc >> 1) ^ ((crc & 1UL) ? 0xEDB88320UL : 0UL);
    }
    return crc ^ 0xFFFFFFFFUL;
}

static uint32_t record_valid(const store_record_t *record)
{
    if (record->magic != STORE_MAGIC ||
        record->schema != POWER_CALIBRATION_SCHEMA ||
        record->payload_size != sizeof(power_calibration_t) ||
        record->commit != STORE_COMMIT ||
        record->calibration.schema != POWER_CALIBRATION_SCHEMA) return 0UL;
    return (record->crc32 == crc32_bytes((const uint8_t *)record,
            (uint32_t)offsetof(store_record_t, crc32))) ? 1UL : 0UL;
}

static uint32_t fault_record_valid(const fault_store_record_t *record)
{
    if (record->magic != FAULT_MAGIC ||
        record->payload_size != sizeof(board_fault_record_t) ||
        record->commit != FAULT_COMMIT) return 0UL;
    return (record->crc32 == crc32_bytes((const uint8_t *)record,
            (uint32_t)offsetof(fault_store_record_t, crc32))) ? 1UL : 0UL;
}

static uint32_t read_fault_record(uint32_t slot,
                                  fault_store_record_t *record)
{
    uint32_t address;
    if (slot >= FAULT_SLOT_COUNT || record == (fault_store_record_t *)0)
        return 0UL;
    address = FAULT_SLOT_BASE_ADDRESS + slot * FAULT_SECTOR_SIZE;
    return (read_bytes(address, (uint8_t *)record, sizeof(*record)) != 0UL &&
            fault_record_valid(record) != 0UL) ? 1UL : 0UL;
}

static void scan_fault_slots(void)
{
    uint32_t slot;
    store_diag.fault_valid_slot_mask = 0UL;
    store_diag.fault_latest_sequence = 0UL;
    for (slot = 0UL; slot < FAULT_SLOT_COUNT; ++slot) {
        fault_store_record_t record;
        if (read_fault_record(slot, &record) != 0UL) {
            store_diag.fault_valid_slot_mask |= 1UL << slot;
            if (store_diag.fault_latest_sequence == 0UL ||
                (int32_t)(record.sequence -
                          store_diag.fault_latest_sequence) > 0)
                store_diag.fault_latest_sequence = record.sequence;
        }
    }
}

static uint32_t read_record(uint32_t address, store_record_t *record)
{
    return (read_bytes(address, (uint8_t *)record, sizeof(*record)) != 0UL &&
            record_valid(record) != 0UL) ? 1UL : 0UL;
}

uint32_t board_cal_store_init(void)
{
    uint32_t ok = 1UL;
    uint8_t manufacturer, type, capacity;
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIODEN;
    RCC_APB2ENR |= RCC_APB2ENR_SPI1EN;
    RCC_APB2RSTR |= RCC_APB2RSTR_SPI1RST;
    RCC_APB2RSTR &= ~RCC_APB2RSTR_SPI1RST;
    gpio_output(GPIOB, FLASH_CS_PIN, 1UL);
    gpio_output(GPIOD, SRAM_CS_PIN, 1UL);
    GPIOB->AFR[0] = (GPIOB->AFR[0] &
        ~((0xFUL << 12) | (0xFUL << 16) | (0xFUL << 20))) |
        (5UL << 12) | (5UL << 16) | (5UL << 20);
    GPIOB->MODER = (GPIOB->MODER &
        ~((3UL << 6) | (3UL << 8) | (3UL << 10))) |
        (2UL << 6) | (2UL << 8) | (2UL << 10);
    GPIOB->OSPEEDR |= (3UL << 6) | (3UL << 8) | (3UL << 10);
    GPIOB->PUPDR &= ~((3UL << 6) | (3UL << 8) | (3UL << 10));
    SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                SPI_CR1_BR_1 | SPI_CR1_BR_0;
    SPI1->CR2 = SPI_CR2_DS_2 | SPI_CR2_DS_1 | SPI_CR2_DS_0 |
                SPI_CR2_FRXTH;
    SPI1->CR1 |= SPI_CR1_SPE;
    flash_select();
    (void)spi_transfer(0x9FU, &ok);
    manufacturer = spi_transfer(0xFFU, &ok);
    type = spi_transfer(0xFFU, &ok);
    capacity = spi_transfer(0xFFU, &ok);
    if (flash_release() == 0UL) ok = 0UL;
    store_diag.jedec_id = ((uint32_t)manufacturer << 16) |
                          ((uint32_t)type << 8) | capacity;
    store_diag.ready = (ok != 0UL && store_diag.jedec_id == W25Q_JEDEC_ID) ?
                       1UL : 0UL;
    store_diag.valid_slot_mask = 0UL;
    store_diag.active_sequence = 0UL;
    store_diag.load_count = 0UL;
    store_diag.save_count = 0UL;
    store_diag.erase_count = 0UL;
    store_diag.last_result = store_diag.ready ? 0 : -1;
    store_diag.fault_append_count = 0UL;
    store_diag.fault_erase_count = 0UL;
    if (store_diag.ready != 0UL) scan_fault_slots();
    return store_diag.ready;
}

int board_cal_store_load(power_calibration_t *calibration)
{
    store_record_t a, b;
    uint32_t va, vb;
    const store_record_t *selected;
    if (calibration == (power_calibration_t *)0 || store_diag.ready == 0UL)
        return -1;
    va = read_record(STORE_SLOT0_ADDRESS, &a);
    vb = read_record(STORE_SLOT1_ADDRESS, &b);
    store_diag.valid_slot_mask = va | (vb << 1);
    if (va == 0UL && vb == 0UL) {
        store_diag.last_result = -2;
        return -2;
    }
    if (va != 0UL && vb != 0UL)
        selected = ((int32_t)(a.sequence - b.sequence) > 0) ? &a : &b;
    else selected = (va != 0UL) ? &a : &b;
    *calibration = selected->calibration;
    store_diag.active_sequence = selected->sequence;
    ++store_diag.load_count;
    store_diag.last_result = 0;
    return 0;
}

int board_cal_store_save(const power_calibration_t *calibration)
{
    store_record_t record, verify, a, b;
    uint32_t va, vb, target;
    if (calibration == (const power_calibration_t *)0 ||
        calibration->schema != POWER_CALIBRATION_SCHEMA ||
        store_diag.ready == 0UL) return -1;
    va = read_record(STORE_SLOT0_ADDRESS, &a);
    vb = read_record(STORE_SLOT1_ADDRESS, &b);
    store_diag.valid_slot_mask = va | (vb << 1);
    if (va != 0UL && vb != 0UL) {
        if ((int32_t)(a.sequence - b.sequence) > 0) {
            record.sequence = a.sequence + 1UL; target = STORE_SLOT1_ADDRESS;
        } else {
            record.sequence = b.sequence + 1UL; target = STORE_SLOT0_ADDRESS;
        }
    } else if (va != 0UL) {
        record.sequence = a.sequence + 1UL; target = STORE_SLOT1_ADDRESS;
    } else if (vb != 0UL) {
        record.sequence = b.sequence + 1UL; target = STORE_SLOT0_ADDRESS;
    } else {
        record.sequence = 1UL; target = STORE_SLOT0_ADDRESS;
    }
    record.magic = STORE_MAGIC;
    record.schema = POWER_CALIBRATION_SCHEMA;
    record.payload_size = sizeof(power_calibration_t);
    record.calibration = *calibration;
    record.commit = STORE_COMMIT;
    record.crc32 = crc32_bytes((const uint8_t *)&record,
        (uint32_t)offsetof(store_record_t, crc32));
    if (erase_sector(target) == 0UL) { store_diag.last_result = -2; return -2; }
    if (page_program(target, (const uint8_t *)&record, sizeof(record)) == 0UL) {
        store_diag.last_result = -3; return -3;
    }
    if (read_record(target, &verify) == 0UL ||
        verify.sequence != record.sequence) {
        store_diag.last_result = -4; return -4;
    }
    store_diag.active_sequence = record.sequence;
    store_diag.valid_slot_mask = (target == STORE_SLOT0_ADDRESS) ?
                                 (store_diag.valid_slot_mask | 1UL) :
                                 (store_diag.valid_slot_mask | 2UL);
    ++store_diag.save_count;
    store_diag.last_result = 0;
    return 0;
}

int board_cal_store_erase(void)
{
    if (store_diag.ready == 0UL) return -1;
    if (erase_sector(STORE_SLOT0_ADDRESS) == 0UL) {
        store_diag.last_result = -2; return -2;
    }
    if (erase_sector(STORE_SLOT1_ADDRESS) == 0UL) {
        store_diag.last_result = -3; return -3;
    }
    store_diag.valid_slot_mask = 0UL;
    store_diag.active_sequence = 0UL;
    ++store_diag.erase_count;
    store_diag.last_result = 0;
    return 0;
}

int board_fault_store_append(const board_fault_record_t *fault)
{
    fault_store_record_t record, verify;
    uint32_t target_slot, target_address;
    if (fault == (const board_fault_record_t *)0 || store_diag.ready == 0UL)
        return -1;
    record.magic = FAULT_MAGIC;
    record.sequence = store_diag.fault_latest_sequence + 1UL;
    if (record.sequence == 0UL) record.sequence = 1UL;
    record.payload_size = sizeof(board_fault_record_t);
    record.fault = *fault;
    record.commit = FAULT_COMMIT;
    record.crc32 = crc32_bytes((const uint8_t *)&record,
        (uint32_t)offsetof(fault_store_record_t, crc32));
    target_slot = (record.sequence - 1UL) % FAULT_SLOT_COUNT;
    target_address = FAULT_SLOT_BASE_ADDRESS +
                     target_slot * FAULT_SECTOR_SIZE;
    if (erase_sector(target_address) == 0UL) return -2;
    if (page_program(target_address, (const uint8_t *)&record,
                     sizeof(record)) == 0UL) return -3;
    if (read_fault_record(target_slot, &verify) == 0UL ||
        verify.sequence != record.sequence) return -4;
    store_diag.fault_latest_sequence = record.sequence;
    store_diag.fault_valid_slot_mask |= 1UL << target_slot;
    ++store_diag.fault_append_count;
    return 0;
}

int board_fault_store_read_recent(uint32_t newest_index,
                                  board_fault_record_t *fault,
                                  uint32_t *sequence)
{
    uint32_t slot;
    uint32_t wanted;
    fault_store_record_t record;
    if (fault == (board_fault_record_t *)0 || sequence == (uint32_t *)0 ||
        store_diag.ready == 0UL || newest_index >= FAULT_SLOT_COUNT ||
        store_diag.fault_latest_sequence == 0UL) return -1;
    wanted = store_diag.fault_latest_sequence - newest_index;
    if (wanted == 0UL) return -2;
    for (slot = 0UL; slot < FAULT_SLOT_COUNT; ++slot) {
        if (read_fault_record(slot, &record) != 0UL &&
            record.sequence == wanted) {
            *fault = record.fault;
            *sequence = record.sequence;
            return 0;
        }
    }
    return -3;
}

int board_fault_store_erase(void)
{
    uint32_t slot;
    if (store_diag.ready == 0UL) return -1;
    for (slot = 0UL; slot < FAULT_SLOT_COUNT; ++slot) {
        if (erase_sector(FAULT_SLOT_BASE_ADDRESS +
                         slot * FAULT_SECTOR_SIZE) == 0UL) return -2;
    }
    store_diag.fault_valid_slot_mask = 0UL;
    store_diag.fault_latest_sequence = 0UL;
    ++store_diag.fault_erase_count;
    return 0;
}

void board_cal_store_get_diag(board_cal_store_diag_t *diag)
{
    if (diag != (board_cal_store_diag_t *)0) *diag = store_diag;
}
