#include <stdint.h>
#include "syscall.h"
#include "task.h"

void puts(const char *s);
extern struct task* task_list;

#define KB_PORT 0x60
#define KB_BUF_SIZE 256

static uint8_t kb_buf[KB_BUF_SIZE];
static int kb_head = 0;
static int kb_tail = 0;

static struct key_event key_events[KB_BUF_SIZE];
static int ev_head = 0;
static int ev_tail = 0;

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %w1, %b0" : "=a"(ret) : "Nd"(port) );
    return ret;
}

// 簡易USキーボードマップ (シフトなし、スキャンコードセット1)
static const char keymap[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
  '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0,  'a','s','d','f','g','h','j','k','l',';','\'','`',  0, '\\',
    'z','x','c','v','b','n','m',',','.','/',  0, '*',
    0, ' ', 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static void send_sigint_to_foreground_pgrp(void) {
    struct task* t = task_list;
    extern int tty_get_foreground_pgrp(void);
    int fg = tty_get_foreground_pgrp();
    while (t) {
        if (t->pgid == fg && t->pid != 1) {
            t->sig_pending |= (1ULL << 2);
            t->exit_status = 130;
            t->state = TASK_ZOMBIE;
        }
        t = t->next;
    }
}

void keyboard_handler(void) {
    uint8_t scancode = inb(KB_PORT);
    uint8_t pressed = (scancode & 0x80) ? 0 : 1;
    uint8_t code = scancode & 0x7F;
    char c = keymap[code];

    int next_ev_head = (ev_head + 1) % KB_BUF_SIZE;
    if (next_ev_head != ev_tail) {
        key_events[ev_head].pressed = pressed;
        key_events[ev_head].scancode = code;
        key_events[ev_head].ascii = (uint16_t)(uint8_t)c;
        ev_head = next_ev_head;
    }

    if (pressed && c) {
        if (c == 3) {
            send_sigint_to_foreground_pgrp();
            return;
        }
        int next_head = (kb_head + 1) % KB_BUF_SIZE;
        if (next_head != kb_tail) {
            kb_buf[kb_head] = c;
            kb_head = next_head;
        }
    }
}

void serial_handler(void) {
    uint8_t c = inb(0x3f8);
    if (c == 3) {
        send_sigint_to_foreground_pgrp();
        return;
    }
    if (c == '\r') c = '\n'; // terminal CR to LF

    int next_head = (kb_head + 1) % KB_BUF_SIZE;
    if (next_head != kb_tail) {
        kb_buf[kb_head] = c;
        kb_head = next_head;
    }
}

// ユーザー空間から呼ばれる (sys_read fd=0)
int kb_read(char* buf, int count) {
    int read = 0;
    while (read < count) {
        if (kb_head == kb_tail) break; // バッファ空
        buf[read++] = kb_buf[kb_tail];
        kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    }
    return read;
}

int kb_get_event(struct key_event* ev) {
    if (ev_head == ev_tail) return 0;
    *ev = key_events[ev_tail];
    ev_tail = (ev_tail + 1) % KB_BUF_SIZE;
    return 1;
}
