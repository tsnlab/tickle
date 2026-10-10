// qemu_tb_count: reads a qemu `-d in_asm,exec,nochain` log on stdin and prints, for each translated block that ran,
// its start pc, function, instruction count and executions ("PC <pc> <fn> <insns> <execs>"), then the total ("TOTAL").
// An "IN:" block gives a block's start pc and length; each "Trace" line is one execution of the block at its pc.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { PC_BITS = 64, TABLE_BITS = 20, TABLE_SIZE = 1U << TABLE_BITS, NAME_BYTES = 48, LINE_BYTES = 4096, HEX = 16 };

static uint64_t g_pc[TABLE_SIZE];
static uint64_t g_execs[TABLE_SIZE];
static uint32_t g_insns[TABLE_SIZE];
static char g_name[TABLE_SIZE][NAME_BYTES];

static uint32_t slot_of(uint64_t start_pc) {
    const uint64_t golden = 0x9E3779B97F4A7C15ULL;
    uint32_t index = (uint32_t)(((start_pc >> 2) * golden) >> (PC_BITS - TABLE_BITS));
    while (g_pc[index] != 0 && g_pc[index] != start_pc) {
        index = (index + 1) & (TABLE_SIZE - 1);
    }
    g_pc[index] = start_pc;
    return index;
}

static void count_execution(const char* line) {
    const char* slash = strchr(line, '/');
    if (slash != NULL) {
        g_execs[slot_of(strtoull(slash + 1, NULL, HEX))]++;
    }
}

int main(void) {
    static char line[LINE_BYTES];
    char function[NAME_BYTES] = "?";
    uint64_t block_pc = 0;
    uint32_t block_insns = 0;
    int at_block_start = 0;
    while (fgets(line, sizeof line, stdin) != NULL) {
        if (strncmp(line, "Trace ", strlen("Trace ")) == 0) {
            count_execution(line);
        } else if (strncmp(line, "IN:", strlen("IN:")) == 0) {
            if (block_pc != 0) {
                g_insns[slot_of(block_pc)] = block_insns;
            }
            block_pc = 0;
            block_insns = 0;
            at_block_start = 1;
            char* label = line + strlen("IN:");
            while (*label == ' ') {
                label++;
            }
            label[strcspn(label, "\n")] = '\0';
            (void)snprintf(function, sizeof function, "%.40s", *label != '\0' ? label : "?");
        } else if (line[0] == '0' && line[1] == 'x') {
            if (at_block_start) {
                block_pc = strtoull(line, NULL, HEX);
                (void)snprintf(g_name[slot_of(block_pc)], NAME_BYTES, "%s", function);
                at_block_start = 0;
            }
            block_insns++;
        }
    }
    if (block_pc != 0) {
        g_insns[slot_of(block_pc)] = block_insns;
    }
    uint64_t total = 0;
    uint64_t unknown = 0;
    for (uint32_t i = 0; i < TABLE_SIZE; i++) {
        if (g_pc[i] == 0 || g_execs[i] == 0) {
            continue;
        }
        if (g_insns[i] == 0) {
            unknown += g_execs[i];
            continue;
        }
        total += g_execs[i] * g_insns[i];
        printf("PC %llx %s %u %llu\n", (unsigned long long)g_pc[i], g_name[i], g_insns[i],
               (unsigned long long)g_execs[i]);
    }
    printf("TOTAL %llu unknown_tb_execs %llu\n", (unsigned long long)total, (unsigned long long)unknown);
    return 0;
}
