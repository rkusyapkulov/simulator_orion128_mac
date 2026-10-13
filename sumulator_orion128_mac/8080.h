// 8080.h
#ifndef CPU_8080_H
#define CPU_8080_H

#include <stdint.h>
#include <stdbool.h>

// --- СТРУКТУРЫ ТАЙМЕРА КР580ВИ53 ---
typedef struct {
    uint16_t count;
    uint16_t latch;
    uint8_t mode;
    uint8_t bcd;
    uint8_t access_phase;
    bool latched;
    double phase;
} PITChannel;

// --- СТРУКТУРА ПРОЦЕССОРА INTEL 8080 (КР580ВМ80А) ---
typedef struct {
    uint8_t A, B, C, D, E, H, L;            // Основные регистры
    uint16_t PC, SP;                        // Регистр счетчика команд и указатель стека
    
    // Флаги процессора
    bool S, Z, AC, P, CY;                   // Sign, Zero, Auxiliary Carry, Parity, Carry
    bool EI;                                // Разрешение прерываний
    bool halted;                            // Состояние останова процессора (HLT)
    
    int cycles_until_interrupt;             // Счетчик тактов до генерации прерывания
    bool int_pending;                       // Флаг отложенного аппаратного прерывания
    
    // Порты КР580ВВ55 (ППА)
    uint8_t ppi0_pa, ppi0_pb, ppi0_pc, ppi0_ctrl; // 0xF400 (Клавиатура РК-86)
    uint8_t ppi1_pa, ppi1_pb, ppi1_pc, ppi1_ctrl; // 0xF500 (Интерфейс ROM-диска)
    uint8_t ppi2_pa, ppi2_pb, ppi2_pc, ppi2_ctrl; // 0xF600 (Пользовательский порт №2)
    
    // Состояние КР580ВИ53 (Таймер/Звук)
    PITChannel pit_channels[3];             // Три независимых канала таймера
    uint8_t pit_control;                    // Управляющее слово таймера
} CPU8080;

// --- ИНТЕРФЕЙС УПРАВЛЕНИЯ ПРОЦЕССОРОМ ---
void cpu8080_reset(CPU8080 *cpu);
int cpu8080_step(CPU8080 *cpu);
uint8_t cpu8080_read(CPU8080 *cpu, uint16_t addr);
void cpu8080_write(CPU8080 *cpu, uint16_t addr, uint8_t val);

#endif // CPU_8080_H

