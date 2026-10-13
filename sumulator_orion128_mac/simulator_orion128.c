// simulator_orion128.c
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "8080.h"
#include "simulator_orion128.h"

// Эмулируемое пространство ОЗУ и дисплея Орион-128
static uint8_t orion_ram[4][65536];         // 256 Кб ОЗУ (4 банка по 64 Кб)
static uint8_t rom_disk_storage[65536];     // Внешний ROM-диск 64 Кб
static uint32_t pixel_buffer[SCREEN_WIDTH * SCREEN_HEIGHT];

static uint8_t orion_page;                  // Номер текущего банка памяти (Порт 0xF900)
static uint8_t orion_sys_port1;             // Управление графикой/цветностью (Порт 0xF800)
static uint8_t orion_sys_port3;             // Переключение экранной области (Порт 0xFA00)

bool rk_cc_pressed;                  // Модификатор CC (Shift) -> PC5
bool rk_us_pressed;                  // Модификатор УС (Ctrl) -> PC6
bool rk_rus_lat_pressed;             // Модификатор РУС/ЛАТ -> PC7

// Матрица клавиатуры РК-86 / Орион-128 (8 строк по 8 бит)
static uint8_t keyboard_matrix[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Эталонная 16-цветная палитра Ориона-128 (в формате ARGB/RGBA для Mac)
static const uint32_t orion_palette[16] = {
    0x00000000, 0x000000AA, 0x0000AA00, 0x0000AAAA, 0x00AA0000, 0x00AA00AA,
    0x00AA5500, 0x00AAAAAA, 0x00555555, 0x005555FF, 0x0055FF55, 0x0055FFFF,
    0x00FF5555, 0x00FF55FF, 0x00FFFF55, 0x00FFFFFF
};

#ifndef PIT_CLOCK_HZ
#define PIT_CLOCK_HZ 2500000   // частота CLK таймера; подставьте значение из схемы вашего Ориона
#endif

static uint16_t pit_bcd2bin(uint16_t v) {
    return (uint16_t)(((v >> 12) & 15) * 1000 + ((v >> 8) & 15) * 100 + ((v >> 4) & 15) * 10 + (v & 15));
}
static uint16_t pit_bin2bcd(uint16_t v) {
    v %= 10000;
    return (uint16_t)(((v / 1000) << 12) | (((v / 100) % 10) << 8) | (((v / 10) % 10) << 4) | (v % 10));
}
static uint16_t pit_norm(uint32_t v, int bcd) { return (uint16_t)(bcd ? v % 10000u : v & 0xFFFFu); }
static uint16_t pit_dec(uint16_t c, int bcd) { return c == 0 ? (uint16_t)(bcd ? 9999 : 0xFFFF) : (uint16_t)(c - 1); }

static void pit_load(PITChannel *ch, uint16_t raw) {
    uint16_t v = ch->bcd ? pit_norm(pit_bcd2bin(raw), 1) : raw;
    ch->reload = v;
    switch (ch->mode) {
    case 0: ch->count = v; ch->armed = true; ch->out = false; ch->state = 0; break;
    case 4: ch->count = v; ch->armed = true; ch->out = true;  ch->state = 0; break;
    case 2: case 3:
        if (!ch->armed) {   // новый счёт подхватывается в конце периода, если счётчик уже идёт
            ch->armed = true; ch->out = true;
            ch->count = (ch->mode == 3) ? pit_norm((uint32_t)v + (v & 1), ch->bcd) : v;
        }
        break;
    default: break;
    }
}

static void pit_write_control(CPU8080 *cpu, uint8_t val) {
    cpu->pit_control = val;
    int sel = (val >> 6) & 3;
    if (sel == 3) return;                       // у ВИ53 нет команды Read-Back
    PITChannel *ch = &cpu->pit_channels[sel];
    int rw = (val >> 4) & 3;
    if (rw == 0) {                              // защёлкивание: режим и счёт не затрагиваются
        if (!ch->latched) { ch->latch = ch->count; ch->latched = true; }
        return;
    }
    int m = (val >> 1) & 7;
    if (m >= 6) m -= 4;                         // режимы 6 и 7 - синонимы 2 и 3
    ch->mode = (uint8_t)m;
    ch->bcd = val & 1;
    ch->rw_mode = (uint8_t)rw;
    ch->access_phase = ch->read_phase = 0;
    ch->latched = false; ch->armed = false; ch->state = 0;
    ch->out = (m != 0);
}

static void pit_write_count(PITChannel *ch, uint8_t val) {
    uint16_t v;
    if (ch->rw_mode == 0) return;               // слово режима ещё не записано
    if (ch->rw_mode == 1) v = val;
    else if (ch->rw_mode == 2) v = (uint16_t)(val << 8);
    else if (ch->access_phase == 0) {
        ch->wr_lo = val; ch->access_phase = 1;
        if (ch->mode == 0) { ch->armed = false; ch->out = false; }  // режим 0: счёт стоит до записи MSB
        return;
    } else {
        v = (uint16_t)((val << 8) | ch->wr_lo); ch->access_phase = 0;
    }
    pit_load(ch, v);
}

static uint8_t pit_read_count(PITChannel *ch) {
    uint16_t c = ch->latched ? ch->latch : ch->count;
    if (ch->bcd) c = pit_bin2bcd(c);
    uint8_t r;
    if (ch->rw_mode == 2)      { r = (uint8_t)(c >> 8); ch->latched = false; }
    else if (ch->rw_mode == 1) { r = (uint8_t)(c & 0xFF); ch->latched = false; }
    else if (ch->read_phase == 0) { r = (uint8_t)(c & 0xFF); ch->read_phase = 1; }
    else                       { r = (uint8_t)(c >> 8); ch->read_phase = 0; ch->latched = false; }
    return r;
}

static void pit_tick_channel(PITChannel *ch, int n) {
    for (; n > 0 && ch->armed; n--) {
        switch (ch->mode) {
        case 0:
            ch->count = pit_dec(ch->count, ch->bcd);
            if (ch->count == 0) ch->out = true;
            break;
        case 2:
            ch->count = pit_dec(ch->count, ch->bcd);
            if (ch->count == 1) ch->out = false;
            else if (ch->count == 0) { ch->count = ch->reload; ch->out = true; }
            break;
        case 3:
            ch->count = pit_dec(pit_dec(ch->count, ch->bcd), ch->bcd);
            if (ch->count == 0) {
                ch->out = !ch->out;
                uint32_t r = ch->reload;
                ch->count = pit_norm(ch->out ? r + (r & 1) : (r & ~1u), ch->bcd);
            }
            break;
        case 4:
            ch->count = pit_dec(ch->count, ch->bcd);
            if (ch->state == 1) { ch->out = true; ch->state = 2; }
            else if (ch->state == 0 && ch->count == 0) { ch->out = false; ch->state = 1; }
            break;
        default: return;
        }
    }
}

static void pit_tick(CPU8080 *cpu, int n) {
    for (int i = 0; i < 3; i++) pit_tick_channel(&cpu->pit_channels[i], n);
}

// =========================================================================
// ИСПРАВЛЕННЫЙ АППАРАТНЫЙ ДИСПЕТЧЕР ЧТЕНИЯ ПАМЯТИ И ПОРТОВ (ОРИОН-128)
// =========================================================================
uint8_t cpu8080_read(CPU8080 *cpu, uint16_t addr) {
    // 1. Зона системного Монитора / BIOS (0xF800 - 0xFFFF)
    if (addr >= 0xF800) {
        return orion_ram[0][addr];
    }

    int current_bank = orion_page & 3;
    
    // 2. Дешифрация аппаратных портов ввода-вывода (0xF400 - 0xF7FF)
    // В Орионе порты доступны только при определенных конфигурациях, проверим стандартное окно:
    if ((current_bank == 0 || current_bank == 1) && addr >= 0xF400 && addr <= 0xF7FF) {
        
        // К580ВВ55 №1: Порт клавиатуры (0xF400 - 0xF403)
        if ((addr & 0xFF00) == 0xF400) {
            uint16_t reg = addr & 3;
            if (reg == 0) return cpu->ppi0_pa;
            if (reg == 1) {
                uint8_t result_pb = 0xFF;
                for (int col = 0; col < 8; col++) {
                    // Если бит в PA сброшен в 0, значит Монитор проверяет этот ряд клавиатуры
                    if ((cpu->ppi0_pa & (1 << col)) == 0) {
                        result_pb &= keyboard_matrix[col];
                    }
                }
                // Если Монитор выставил в PA значение 0x00 (проверить все клавиши разом)
                if (cpu->ppi0_pa == 0x00) {
                    for (int i = 0; i < 8; i++) {
                        result_pb &= keyboard_matrix[i];
                    }
                }
                return result_pb;
            }
            if (reg == 2) {
                uint8_t result_pc = cpu->ppi0_pc | 0xE0;
                if (rk_cc_pressed) result_pc &= ~(1 << 5);
                if (rk_us_pressed) result_pc &= ~(1 << 6);
                if (rk_rus_lat_pressed) result_pc &= ~(1 << 7);
                return result_pc;
            }
            return cpu->ppi0_ctrl;
        }
        
        // К580ВВ55 №2: Интерфейс ROM-диска (0xF500 - 0xF503)
        if ((addr & 0xFF00) == 0xF500) {
            uint16_t reg = addr & 3;
            if (reg == 0) {
                uint16_t rom_address = (cpu->ppi1_pc << 8) | cpu->ppi1_pb;
                return rom_disk_storage[rom_address];
            }
            if (reg == 1) return cpu->ppi1_pb;
            if (reg == 2) return cpu->ppi1_pc;
            return cpu->ppi1_ctrl;
        }
        
        // ППА №3: 0xF600–0xF603
        if ((addr & 0xFF00) == 0xF600) {
            switch (addr & 3) {
                case 0: return cpu->ppi2_pa;
                case 1: return cpu->ppi2_pb;
                case 2: return cpu->ppi2_pc;
                default: return cpu->ppi2_ctrl;
            }
        }

        // Таймер КР580ВИ53: 0xF730–0xF733
        if (addr >= 0xF730 && addr <= 0xF733) {
            uint16_t reg = addr & 3;

            if (reg < 3) return pit_read_count(&cpu->pit_channels[reg]);

            return cpu->pit_control;
        }

        return 0xFF; // Неподключённый адрес окна ввода-вывода: шина "плавает"
    }

    // 3. Служебная область системных переменных Монитора (0xF000 - 0xF3FF)
    if ((current_bank == 0 || current_bank == 1) && addr >= 0xF000 && addr <= 0xF3FF) {
        return orion_ram[0][addr];
    }

    // 4. Чтение из выбранного банка
    return orion_ram[current_bank][addr];
}


// =========================================================================
// ИСПРАВЛЕННЫЙ АППАРАТНЫЙ ДИСПЕТЧЕР ЗАПИСИ В ПАМЯТЬ И ПОРТОВ (ОРИОН-128)
// =========================================================================
void cpu8080_write(CPU8080 *cpu, uint16_t addr, uint8_t val) {

    // 1. ПЗУ Монитора защищено от записи аппаратно!
    if (addr >= 0xF800) {
        uint16_t port_type = addr & 0xFF00;
        if (port_type == 0xF800) { orion_sys_port1 = val; return; }
        if (port_type == 0xF900) { orion_page = val & 3; return; }
        if (port_type == 0xFA00) { orion_sys_port3 = val & 3; return; }
        return;
    }

    int current_bank = orion_page & 3;

    // 2. Запись в периферийные БИС К580ВВ55 и КР580ВИ53 (0xF400 - 0xF7FF)
    if ((current_bank == 0 || current_bank == 1) && addr >= 0xF400 && addr <= 0xF7FF) {
        if ((addr & 0xFF00) == 0xF400) {
            uint16_t reg = addr & 3;
            if (reg == 0) cpu->ppi0_pa = val;
            else if (reg == 1) cpu->ppi0_pb = val;
            else if (reg == 2) {
                cpu->ppi0_pc = val;
            }
            else {
                cpu->ppi0_ctrl = val;
                if ((val & 0x80) == 0) {
                    int bit_to_change = (val >> 1) & 7;
                    if (val & 1) cpu->ppi0_pc |= (1 << bit_to_change);
                    else cpu->ppi0_pc &= ~(1 << bit_to_change);
                }
            }
            return;
        }
        if ((addr & 0xFF00) == 0xF500) {
            uint16_t reg = addr & 3;
            if (reg == 0) cpu->ppi1_pa = val;
            else if (reg == 1) cpu->ppi1_pb = val;
            else if (reg == 2) cpu->ppi1_pc = val;
            else cpu->ppi1_ctrl = val;
            return;
        }
        if ((addr & 0xFF00) == 0xF600) {
            uint16_t reg = addr & 3;
            if (reg == 0) { cpu->ppi2_pa = val; }
            else if (reg == 1) { cpu->ppi2_pb = val; }
            else if (reg == 2) { cpu->ppi2_pc = val; }
            else cpu->ppi2_ctrl = val;
            return;
        }
        // Системный таймер КР580ВИ53 (PIT)
        if (addr >= 0xF730 && addr <= 0xF733) {
            uint16_t reg = addr & 3;
            if (reg < 3) pit_write_count(&cpu->pit_channels[reg], val);
            else pit_write_control(cpu, val);
            return;
        }
        return;
    }

    if ((current_bank == 0 || current_bank == 1) && addr >= 0xF000 && addr <= 0xF3FF) {
        orion_ram[0][addr] = val;
        return;
    }

    orion_ram[current_bank][addr] = val;
}

// Аудио-подсистема
#define AUDIO_RING_SIZE 16384
static int16_t audio_ring_buffer[AUDIO_RING_SIZE];
static _Atomic uint32_t audio_ring_head = 0;   // пишет только основной поток
static _Atomic uint32_t audio_ring_tail = 0;   // пишет только аудиопоток
static uint8_t current_speaker_bit = 0;

void audio_ring_push(int16_t sample) {
    uint32_t head = atomic_load_explicit(&audio_ring_head, memory_order_relaxed);
    uint32_t next_head = (head + 1u) % AUDIO_RING_SIZE;
    uint32_t tail = atomic_load_explicit(&audio_ring_tail, memory_order_acquire);
    if (next_head != tail) {
        audio_ring_buffer[head] = sample;
        atomic_store_explicit(&audio_ring_head, next_head, memory_order_release);
    }
}

// Экземпляр процессора КР580ВМ80А внутри симулятора
static CPU8080 global_cpu;

void orion128_get_audio_samples(int16_t *buffer, int sample_count) {
    if (!buffer || sample_count <= 0) return;
    for (int i = 0; i < sample_count; i++) {
        uint32_t tail = atomic_load_explicit(&audio_ring_tail, memory_order_relaxed);
        uint32_t head = atomic_load_explicit(&audio_ring_head, memory_order_acquire);
        if (tail != head) {
            buffer[i] = audio_ring_buffer[tail];
            atomic_store_explicit(&audio_ring_tail, (tail + 1u) % AUDIO_RING_SIZE, memory_order_release);
        } else {
            // ИСПРАВЛЕНО: Заполняем актуальным уровнем постоянного тока из структуры CPU
            buffer[i] = global_cpu.EI ? 4000 : -4000;
        }
    }
}

#define CPU_CLOCK_HZ 2500000
#define AUDIO_SAMPLE_RATE 44100

static uint64_t audio_sample_phase = 0;
static uint64_t pit_accum = 0;   // дробный остаток тактов CLK таймера

void orion128_execute_cycles(int cycles)
{
    int executed = 0;

    while (executed < cycles) {
        int step_cycles = cpu8080_step(&global_cpu);

        if (step_cycles <= 0) {
            break;
        }

        executed += step_cycles;

        pit_accum += (uint64_t)step_cycles * PIT_CLOCK_HZ;
        if (pit_accum >= CPU_CLOCK_HZ) {
            int ticks = (int)(pit_accum / CPU_CLOCK_HZ);
            pit_accum %= CPU_CLOCK_HZ;
            pit_tick(&global_cpu, ticks);
        }

        audio_sample_phase +=
            (uint64_t)step_cycles * AUDIO_SAMPLE_RATE;

        while (audio_sample_phase >= CPU_CLOCK_HZ) {
            audio_sample_phase -= CPU_CLOCK_HZ;

            int16_t sample = global_cpu.EI ? 4000 : -4000;
            audio_ring_push(sample);
        }
    }
}

void RenderOrionScreen(void) {
    // 1. Определение стартового адреса экрана на основе Системного порта №3
    uint16_t screen_base = 0xC000;
    switch (orion_sys_port3 & 3) {
        case 0: screen_base = 0xC000; break;
        case 1: screen_base = 0x8000; break;
        case 2: screen_base = 0x4000; break;
        case 3: screen_base = 0x0000; break;
    }

    // 2. Дешифрация режима цветности из системного порта 1 (orion_sys_port1)
    bool color_mode = (orion_sys_port1 & 0x04) != 0;    // Бит D2: 1 - Цвет, 0 - Монохром
    bool mode_4_color = (orion_sys_port1 & 0x02) == 0;  // Бит D1: 0 - 4 цвета, 1 - 16 цветов
    uint8_t palette_idx = orion_sys_port1 & 0x01;       // Бит D0: Выбор подпалитры

    // Разделение банков для маски и цвета (Бит D3 переключает экранные страницы)
    int bank_mask = 0;
    int bank_color = 1;
    if ((orion_sys_port1 & 0x08) != 0) {
        bank_mask = 2;
        bank_color = 3;
    }

    // 3. Рендеринг кадра
    for (int x_byte = 0; x_byte < 48; x_byte++) {
        for (int y = 0; y < 256; y++) {
            uint16_t addr = screen_base + (x_byte * 256) + y;
            
            uint8_t layer0 = orion_ram[bank_mask][addr];

            if (!color_mode) {
                // Монохромный режим
                uint32_t color_on = (palette_idx == 0) ? 0x0000FF00 : 0x0000FFFF; // Зеленый или Желтый (порядок байт как в orion_palette)
                uint32_t color_off = 0x00000000; // Черный
                
                for (int bit = 0; bit < 8; bit++) {
                    bool dot_state = (layer0 & (1 << (7 - bit))) != 0;
                    int dest_idx = (y * SCREEN_WIDTH) + (x_byte * 8) + bit;
                    pixel_buffer[dest_idx] = dot_state ? color_on : color_off;
                }
            } else {
                // Цветные режимы
                uint8_t layer1 = orion_ram[bank_color][addr];

                if (mode_4_color) {
                    // Игровой 4-цветный режим (Побитная склейка двух слоев)
                    for (int bit = 0; bit < 8; bit++) {
                        int dot = 7 - bit;
                        int bit0 = (layer0 & (1 << dot)) ? 1 : 0;
                        int bit1 = (layer1 & (1 << dot)) ? 2 : 0;
                        int color_code = bit0 | bit1;

                        uint8_t final_idx = 0;
                        if (palette_idx == 0) {
                            static const uint8_t pal1[] = { 0, 4, 2, 1 }; // черный, красный, зеленый, синий
                            final_idx = pal1[color_code];
                        } else {
                            static const uint8_t pal2[] = { 0, 5, 3, 15 };
                            final_idx = pal2[color_code];
                        }
                        int dest_idx = (y * SCREEN_WIDTH) + (x_byte * 8) + bit;
                        pixel_buffer[dest_idx] = orion_palette[final_idx & 0x0F];
                    }
                } else {
                    // Системный 16-цветный режим ОРДОС
                    uint8_t bg_idx = (layer1 >> 4) & 0x0F;
                    uint8_t fg_idx = layer1 & 0x0F;

                    for (int bit = 0; bit < 8; bit++) {
                        bool bit0 = (layer0 & (1 << (7 - bit))) != 0;
                        uint8_t final_idx = bit0 ? fg_idx : bg_idx;
                        int dest_idx = (y * SCREEN_WIDTH) + (x_byte * 8) + bit;
                        pixel_buffer[dest_idx] = orion_palette[final_idx];
                    }
                }
            }
        }
    }
}

void orion128_init(const char *monitor_path, const char *romdisk_path) {
    memset(orion_ram, 0, sizeof(orion_ram));
    memset(rom_disk_storage, 0xFF, sizeof(rom_disk_storage));
    memset(pixel_buffer, 0, sizeof(pixel_buffer));
    
    FILE *f = fopen(monitor_path, "rb");
    if (f) {
        fread(&orion_ram[0][0xF800], 1, 2048, f); // Стандартный Монитор-1 / Монитор-2
        fclose(f);
    }
    f = fopen(romdisk_path, "rb");
    if (f) {
        fread(&rom_disk_storage, 1, sizeof(rom_disk_storage), f); // ROMDISK
        fclose(f);
    }
    // Первоначальный сброс регистров
    orion128_reset();
}

void orion128_reset(void) {
    current_speaker_bit = 0;

    // Очередь сбрасывает только производитель: head := tail (tail принадлежит аудиопотоку)
    atomic_store_explicit(&audio_ring_head,
        atomic_load_explicit(&audio_ring_tail, memory_order_acquire), memory_order_release);
    audio_sample_phase = 0;
    pit_accum = 0;

    orion_page = 0;
    orion_sys_port1 = 0;
    orion_sys_port3 = 0;

    rk_cc_pressed = false;
    rk_us_pressed = false;
    rk_rus_lat_pressed = false;

    for (int i = 0; i < 8; i++) {
        keyboard_matrix[i] = 0xFF;
    }

    cpu8080_reset(&global_cpu);
}

void orion128_run_frame(void) {
    // Количество тактов на 1 кадр для частоты 2.5 МГц (50 кадров в секунду = 50000 тактов)
    int cycles_per_frame = 50000;
    orion128_execute_cycles(cycles_per_frame);
    
    // Генерация прерывания 50 Гц (если требуется ОС КР580ВМ80А)
    RenderOrionScreen();
}

const uint32_t* orion128_get_pixel_buffer(void) {
    return pixel_buffer;
}

void orion128_press_key(int row, int bit, bool isPressed) {
    if (row < 0 || row > 7 || bit < 0 || bit > 7) return;
    if (isPressed) {
        keyboard_matrix[row] &= ~(1 << bit); // 0 означает, что клавиша замкнута
    } else {
        keyboard_matrix[row] |= (1 << bit);  // 1 означает отпущена
    }
}

// Автоматический поиск смещения для записи нового файла в RAM-диск B (orion_ram страница 1)
static uint16_t GetNextOrdosOffset(void) {
    uint16_t offset = 0;
    const uint16_t RAMDISK_TOP = 0xF000; // Верхняя граница рамдиска под системные нужды

    while (offset < RAMDISK_TOP - 16) {
        // Если встретили маркер пустого пространства или конца цепочки
        if (orion_ram[1][offset] == 0xFF || orion_ram[1][offset] == 0x00) {
            return offset;
        }

        // Читаем длину данных из ORDOS-заголовка текущего файла (смещение 10, 11)
        uint16_t datasize = orion_ram[1][offset + 10] | (orion_ram[1][offset + 11] << 8);

        // Защита от зацикливания при поврежденном каталоге
        if (datasize == 0 || (offset + 16 + datasize) > RAMDISK_TOP) {
            return offset;
        }

        offset += 16 + datasize;
    }
    return offset;
}

// Загрузка бинарных дампов Ориона (.ORD / .RKO / .BRU / .BIN)
bool orion128_load_file(const char* filepath, char* out_msg, size_t msg_max_len) {
    FILE* file = fopen(filepath, "rb");
    if (!file) {
        if (out_msg) snprintf(out_msg, msg_max_len, "Не удалось открыть файл!");
        return false;
    }

    // Вычисляем точный физический размер файла на диске
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (file_size <= 0) {
        fclose(file);
        if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка: файл пуст или его размер не определён.");
        return false;
    }

    // Извлекаем расширение файла вручную (в нижнем регистре)
    char ext[8] = { 0 };
    const char* dot = strrchr(filepath, '.');
    if (dot) {
        int i = 0;
        while (dot[i] && i < 7) {
            // Приведение символов к нижнему регистру
            char c = dot[i];
            if (c >= 'A' && c <= 'Z') c = c + ('a' - 'A');
            ext[i] = c;
            i++;
        }
    }

    // =================================================================
    // 1. ВАРИАНТ: КЛАССИЧЕСКАЯ ЗАГРУЗКА И СТАРТ СЫРЫХ БИНАРНИКОВ (.BIN)
    // =================================================================
    if (strcmp(ext, ".bin") == 0) {
        uint16_t load_address = 0x0000;
        uint16_t file_length = (file_size > 48 * 1024) ? 48 * 1024 : (uint16_t)file_size;

        // Читаем исполняемый код программы строго в БАНК 0 по вычисленному смещению (как было ранее)
        size_t read_bytes = fread(&orion_ram[0][load_address], 1, file_length, file);
        fclose(file);

        if (read_bytes != file_length) {
            if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка чтения данных из файла .bin");
            return false;
        }

        // Аппаратный сброс и настройка окружения для старта игры
        orion_page = 0;        // Принудительно включаем Банк ОЗУ №0
        orion_sys_port1 = 0x24; // Форсируем игровой цветной 4-цветный режим (Бит D5=1, D2=1)
        orion_sys_port3 = 0;   // Видеовыход строго на стандартный адрес 0xC000

        global_cpu.EI = false;        // Выключаем прерывания
        global_cpu.halted = false;    // Выходим из HLT, иначе программа не стартует
        global_cpu.int_pending = false;
        global_cpu.PC = load_address; // Устанавливаем точку старта процессора (0x0000)
        global_cpu.SP = 0xBFFF;       // Стек ниже экранной памяти (C000-EFFF), чтобы не портить картинку

        // Заглушка под ORDOS (RET) - только если она не затирает данные самой программы
        if (file_length <= 0xBE03) {
            orion_ram[0][0xBE03] = 0xC9;
        }
        for (int o_addr = 0xF000; o_addr < 0xF800; o_addr++) {
            orion_ram[0][o_addr] = 0x00; // Очищаем буфер системных переменных (NOP)
        }

        // Сброс физической матрицы клавиатуры в исходное ненажатое состояние
        for (int i = 0; i < 8; i++) {
            keyboard_matrix[i] = 0xFF;
        }

        if (out_msg) {
            snprintf(out_msg, msg_max_len,
                "Бинарный файл успешно запущен!\nАдрес старта: %04Xh\nРазмер кода: %d байт",
                load_address, file_length);
        }
        return true;
    }

    // =================================================================
    // 2. ВАРИАНТ: ЗАГРУЗКА ФАЙЛОВ СТРУКТУРЫ ORDOS (.ORD, .RKO, .BRU) В ДИСК B
    // =================================================================
    bool is_rko = (strcmp(ext, ".rko") == 0);
    if (is_rko) {
        if (file_size > 77) {
            fseek(file, 77, SEEK_SET); // Пропускаем RKO заголовок ленты (77 байт)
            file_size -= 77;
        }
        else {
            fclose(file);
            if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка: Файл .rko слишком мал.");
            return false;
        }
    }

    const uint16_t RAMDISK_TOP = 0xF000;
    uint16_t next_ordos = GetNextOrdosOffset();

    if (next_ordos + 16 >= RAMDISK_TOP) {
        fclose(file);
        if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка: На RAM-диске B нет свободного места под заголовок.");
        return false;
    }

    uint8_t ordos_header[16];
    if (fread(ordos_header, 1, 16, file) != 16) {
        fclose(file);
        if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка: Не удалось прочитать ORDOS-заголовок файла.");
        return false;
    }

    // Извлекаем длину данных файла из заголовка ORDOS (индексы 10 и 11)
    uint16_t datasize = ordos_header[10] | (ordos_header[11] << 8);

    if (next_ordos + 16 + datasize >= RAMDISK_TOP) {
        fclose(file);
        if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка: Файл размером %d байт не помещается на RAM-диск B.", datasize);
        return false;
    }

    // Копируем заголовок в RAM-диск B (orion_ram страница 1)
    memcpy(&orion_ram[1][next_ordos], ordos_header, 16);

    // Читаем тело файла следом за заголовком
    size_t read_bytes = fread(&orion_ram[1][next_ordos + 16], 1, datasize, file);
    fclose(file);

    if (read_bytes != datasize) {
        if (out_msg) snprintf(out_msg, msg_max_len, "Ошибка: Фактический размер данных не совпадает с заголовком.");
        return false;
    }

    // Сдвигаем указатель и обязательно записываем маркер конца цепочки файлов каталога ORDOS
    next_ordos += 16 + datasize;
    orion_ram[1][next_ordos] = 0xFF;

    if (out_msg) {
        snprintf(out_msg, msg_max_len,
            "Файл успешно записан на RAM-диск B!\nИмя в ORDOS: %.8s\nРазмер данных: %d байт\nПерезапустите оболочку (кнопка F4)",
            (char*)ordos_header, datasize);
    }

    return true;
}

