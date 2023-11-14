#include <cstdio>
#include <cstdint>
#include <csignal>
/* unix only */
#include <cstdlib>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/mman.h>

#define MEMORY_MAX (1 << 16)

//memory space 2^16, and each address has 16 digit value
uint16_t M[MEMORY_MAX];


//Register
enum {
    R_R0 = 0, R_R1, R_R2, R_R3, R_R4, R_R5, R_R6, R_R7, R_PC, R_COND, R_COUNT
};
uint16_t R[R_COUNT];
//IS opcode follow LC3
enum {
    OP_BR = 0, /* branch */
    OP_ADD,    /* add  */
    OP_LD,     /* load */
    OP_ST,     /* store */
    OP_JSR,    /* jump register */
    OP_AND,    /* bitwise and */
    OP_LDR,    /* load register */
    OP_STR,    /* store register */
    OP_RTI,    /* unused */
    OP_NOT,    /* bitwise not */
    OP_LDI,    /* load indirect */
    OP_STI,    /* store indirect */
    OP_JMP,    /* jump */
    OP_RES,    /* reserved (unused) */
    OP_LEA,    /* load effective address */
    OP_TRAP    /* execute trap */
};
//condition code
enum {
    CD_NEG = 1 << 2,
    CD_ZERO = 1 << 1,
    CD_POS = 1 << 0
};

//Trap routine
enum {
    TRAP_GETC = 0x20,  /* get a  character from keyboard, not echoed onto the terminal */
    TRAP_OUT = 0x21,   /* output a character */
    TRAP_PUTS = 0x22,  /* output a word string */
    TRAP_IN = 0x23,    /* get character from keyboard, echoed onto the terminal */
    TRAP_PUTSP = 0x24, /* output a byte string */
    TRAP_HALT = 0x25   /* halt the program */
};

//Mapped Register 键盘状态和键盘数据
enum
{
    MR_KBSR = 0xFE00, /* keyboard status */
    MR_KBDR = 0xFE02  /* keyboard data */
};

/*
 * Some detail relate to the plateform
 */
struct termios original_tio;

void disable_input_buffering()
{
    tcgetattr(STDIN_FILENO, &original_tio);
    struct termios new_tio = original_tio;
    new_tio.c_lflag &= ~ICANON & ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
}

void restore_input_buffering()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &original_tio);
}

uint16_t check_key()
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(STDIN_FILENO, &readfds);

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return select(1, &readfds, NULL, NULL, &timeout) != 0;
}


uint16_t sign_extend(uint16_t x, int bit_count) {
    //10100 >> 00001 check if neg or not
    if ((x >> (bit_count - 1)) & 1) { //if neg
        x |= (0xFFFF << bit_count); //1111111111110100
    }
    return x;
}

void update_condition_code(uint16_t r) {
    if (R[r] == 0) {
        R[R_COND] = CD_ZERO;
    } else if (R[r] >> 15) {
        R[R_COND] = CD_NEG;
    } else {
        R[R_COND] = CD_POS;
    }
}

/* memo getter and setter */
void memo_write(uint16_t address, uint16_t val)
{
    M[address] = val;
}

uint16_t memo_read(uint16_t address)
{
    if (address == MR_KBSR)
    {
        if (check_key())
        {
            M[MR_KBSR] = (1 << 15);
            M[MR_KBDR] = getchar();
        }
        else
        {
            M[MR_KBSR] = 0;
        }
    }
    return M[address];
}

uint16_t swap16(uint16_t x) {
    return (x << 8) | (x >> 8);
}

void read_image_file(FILE* file)
{
    /* the origin tells us where in memory to place the image */
    uint16_t origin;
    fread(&origin, sizeof(origin), 1, file);
    origin = swap16(origin);

    /* we know the maximum file size so we only need one fread */
    uint16_t max_read = MEMORY_MAX - origin;
    uint16_t* p = M + origin;
    size_t read = fread(p, sizeof(uint16_t), max_read, file);

    /* swap to little endian */
    while (read-- > 0)
    {
        *p = swap16(*p);
        ++p;
    }
}
int read_image(const char* image_path)
{
    FILE* file = fopen(image_path, "rb");
    if (!file) { return 0; };
    read_image_file(file);
    fclose(file);
    return 1;
}

void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}

int main(int argc, char *argv[]) {
    //load args
    if (argc < 2) {
        /* show usage string */
        printf("lc3 [image-file1] ...\n");
        exit(2);
    }

    for (int j = 1; j < argc; ++j) {
        if (!read_image(argv[j]))
        {
            printf("failed to load image: %s\n", argv[j]);
            exit(1);
        }
    }
    //setup
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();

    /* set up code condition. */
    R[R_COND] = CD_ZERO;
    /* set up pc starting point, 0x3000 is default start in LC-3 */
    R[R_PC] = 0x3000;
    int running = 1;

    while (running) {
        uint16_t instr = memo_read(R[R_PC]++);
        uint16_t op = instr >> 12; //first 4 bit
        switch (op) {
            case OP_ADD: {
//                @{ADD}
                uint16_t dest = (instr >> 9) & 0x7, src1 = instr >> 6 & 0x7, imm_code = (instr >> 5) & 0x1;
                if (imm_code) { //imm mode
                    uint16_t imm = sign_extend(instr & 0x1F, 5);
                    R[dest] = R[src1] + imm;
                } else {
                    uint16_t src2 = instr & 0x7;
                    R[dest] = R[src1] + R[src2];
                }
                update_condition_code(dest);
                break;
            }
            case OP_AND: {
//                @{AND}
                uint16_t dest = (instr >> 9) & 0x7, src1 = instr >> 6 & 0x7, imm_code = (instr >> 5) & 0x1;
                if (imm_code) {
                    uint16_t imm = sign_extend(instr & 0x1F, 5);
                    R[dest] = R[src1] & imm;
                } else {
                    uint16_t src2 = instr & 0x7;
                    R[dest] = R[src1] & R[src2];
                }
                update_condition_code(dest);
                break;
            }
            case OP_NOT: {
//                @{NOT}
                uint16_t dest = (instr >> 9) & 0x7, src1 = instr >> 6 & 0x7;
                R[dest] = R[src1] ^ 0xFFFF; //xor
                update_condition_code(dest);
                break;
            }
            case OP_BR: {
                //TODO
//                @{BR}
                uint16_t nzp = (instr >> 9) & 0x7;
                if (nzp & R[R_COND]) {
                    R[R_PC] += sign_extend((instr & 0x1FF), 9);
                }
                //if not keep going
                break;
            }
            case OP_JMP: {
//                @{JMP}
                R[R_PC] = R[(instr >> 6) & 0x7];
                break;
            }
            case OP_JSR: {
//                @{JSR}
                uint16_t flag = (instr >> 11) & 1;
                R[R_R7] = R[R_PC];

                if (flag) {
                    R[R_PC] += sign_extend(instr & 0x7FF, 11);
                } else {
                    uint16_t baseR = (instr >> 6) & 0x7;
                    R[R_PC] = R[baseR];
                }
                break;
            }
            case OP_LD: {
//                @{LD}
                uint16_t dest = (instr >> 9) & 0x7, offset9 = sign_extend((instr & 0x1FF), 9);

                R[dest] = memo_read(R[R_PC] + offset9);
                update_condition_code(dest);
                break;
            }
            case OP_LDI: {
//                @{LDI}
                uint16_t dest = (instr >> 9) & 0x7, offset9 = sign_extend((instr & 0x1FF), 9);
                R[dest] = memo_read(memo_read(R[R_PC] + offset9));
                update_condition_code(dest);
                break;
            }
            case OP_LDR: {
//                @{LDR}
                uint16_t dest = (instr >> 9) & 0x7, base = (instr >> 6) & 0x7, offset6 = sign_extend((instr & 0x3F), 6);
                R[dest] = memo_read(R[base] + offset6);
                update_condition_code(dest);
                break;
            }
            case OP_LEA: {
//                @{LEA indrected mode}
                uint16_t dest = (instr >> 9) & 0x7, offset9 = sign_extend((instr & 0x1FF), 9);
                R[dest] = R[R_PC] + offset9;
                update_condition_code(dest);
                break;
            }
            case OP_ST: {
//                @{ST}
                uint16_t src = (instr >> 9) & 0x7, offset9 = sign_extend((instr & 0x1FF), 9);
                memo_write(R[R_PC] + offset9, R[src]);
                break;
            }
            case OP_STI: {
//                @{STI}
                uint16_t src = (instr >> 9) & 0x7, offset9 = sign_extend((instr & 0x1FF), 9);
                memo_write(memo_read(R[R_PC] + offset9), R[src]);
                break;
            }
            case OP_STR: {
//                @{STR}
                uint16_t src = (instr >> 9) & 0x7, base = (instr >> 6) & 0x7, offset6 = sign_extend((instr & 0x3F), 6);
                memo_write(R[base] + offset6, R[src]);
                break;
            }
//            There is no specification for how trap routines must be implemented, only what they are supposed to do.
            case OP_TRAP: { //TODO some issue here
                R[R_R7] = R[R_PC];
                switch (instr & 0xFF) {

                    case TRAP_GETC: {
//              @{TRAP GETC}
                        R[R_R0] = (uint16_t)getchar();
                        update_condition_code(R_R0);
                        break;
                    }
                    case TRAP_OUT: {
//                        @{TRAP OUT}
                        putc((char) R[R_R0], stdout);
                        fflush(stdout);
                        break;
                    }
                    case TRAP_PUTS: {
//                        @{TRAP PUTS}
                        uint16_t *c = M + R[R_R0];
                        while (*c) {
                            putc((char) *c, stdout);
                            c++;
                        }
                        fflush(stdout);
                        break;
                    }
                    case TRAP_IN: {
//                        @{TRAP IN}
                        printf("Input a character: ");
                        char c = getchar();
                        putc(c, stdout);
                        fflush(stdout);
                        R[R_R0] = (uint16_t) c;
                        update_condition_code(R_R0);
                        break;
                    }
                    case TRAP_PUTSP: {
//                        @{TRAP PUTSP}

                        uint16_t *c = M + R[R_R0];
                        while (*c) {
                            char char1 = (*c) & 0xFF;
                            putc(char1, stdout);
                            char char2 = (*c) >> 8;
                            if (char2) putc(char2, stdout);
                            ++c;
                        }
                        fflush(stdout);
                        break;
                    }
                    case TRAP_HALT: {
//                        @{TRAP HALT}
                        puts("HALT");
                        fflush(stdout);
                        running = 0;
                        break;
                    }
                }
                break;
            }
                //unused
            case OP_RES:{
                abort();
            }
            case OP_RTI: {
                abort();
            }
            default:
//              @{BAD OPCODE}
                break;
        }
    }
    //Settings should also be restored
    restore_input_buffering();
    return 0;
}

