#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <libgen.h>
#include <locale.h>
#include <curses.h>
#include <err.h>
#include <assert.h>

#define abs(x) ((x) < 0 ? -(x) : (x))
#define floor(x) ((x) >= 0 ? (int)(x) : (int)((x) - 0.9999f))

struct {
    int ymax, xmax;
} scr;

struct window {
    WINDOW *win;
    int ymax, xmax;
} waddr, wmain, wascii, wnumeric, wstatus;

struct hex {
    char digits[2];
    long fpos;
};

struct hexgrid {
    struct hex *data;
    int cols, rows;
    long fpos;
    uint16_t *lookup;
    int ypos, xpos;
    int digpos;
    long cursfpos;
} hexgrid;

struct buf {
    char *data;
    char *filename;
    long size;
} filebuf;

int intsize = 0;

int replace_mode = 0;
int insert_mode = 0;

int running = 1;

uint64_t reverse_bytes(uint64_t b, int nbytes)
{
    uint64_t revb = 0;

    for (int i = 0; i < nbytes; ++i) {
        int oldbitpos = ((nbytes - 1) - i) * 8;
        int newbitpos = i * 8;
        revb |= ((b >> oldbitpos) & 0xff) << newbitpos;
    }

    return revb;
}

void init_curses()
{
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    nonl();
    intrflush(stdscr, FALSE);
    keypad(stdscr, TRUE);
    set_escdelay(50);
}

void init_layout()
{
    getmaxyx(stdscr, scr.ymax, scr.xmax);

    int waddr_w = 10;
    int tw = (scr.xmax - waddr_w) / 4;
    int wascii_w = tw;
    int wmain_w = (tw * 3);
    int wnumeric_h = 8;
    int wstatus_h = 1;

    hexgrid.ypos = 0;
    hexgrid.xpos = 0;
    hexgrid.digpos = 0;

    wstatus.ymax = wstatus_h;
    wstatus.xmax = scr.xmax;
    delwin(wstatus.win);
    wstatus.win = newwin(wstatus.ymax, wstatus.xmax, 0, 0);

    waddr.ymax = scr.ymax - wnumeric_h - wstatus_h;
    waddr.xmax = waddr_w;
    delwin(waddr.win);
    waddr.win = newwin(waddr.ymax, waddr.xmax, wstatus.ymax, 0);

    wmain.ymax = scr.ymax - wnumeric_h - wstatus_h;
    wmain.xmax = wmain_w;
    delwin(wmain.win);
    wmain.win = newwin(wmain.ymax, wmain.xmax, wstatus.ymax, waddr.xmax);

    wascii.ymax = scr.ymax - wnumeric_h - wstatus_h;
    wascii.xmax = wascii_w;
    delwin(wascii.win);
    wascii.win = newwin(wascii.ymax, wascii.xmax, wstatus.ymax, scr.xmax - wascii.xmax);

    wnumeric.ymax = wnumeric_h;
    wnumeric.xmax = scr.xmax;
    delwin(wnumeric.win);
    wnumeric.win = newwin(wnumeric.ymax, wnumeric.xmax, scr.ymax - wnumeric_h, 0);

    hexgrid.cols = wmain.xmax / 3;
    hexgrid.rows = wmain.ymax;
    int hexgrid_memsize = hexgrid.cols * hexgrid.rows * sizeof(struct hex);
    hexgrid.data = realloc(hexgrid.data, hexgrid_memsize);

    clear();
    refresh();
}

void load_file(const char *filename, struct buf *filebuf)
{
    FILE *fp;

    if (!(fp = fopen(filename, "r")))
        errx(1, "load_file: failed to open %s", filename);

    int fnlen = strlen(filename);
    filebuf->filename = malloc(fnlen + 1);
    strncpy(filebuf->filename, filename, fnlen);
    fseek(fp, 0L, SEEK_END);
    filebuf->size = ftell(fp);
    rewind(fp);
    filebuf->data = malloc(filebuf->size);
    long nbytes;

    if ((nbytes = fread(filebuf->data, 1, filebuf->size, fp)) < filebuf->size)
        errx(1, "fread: read %ld bytes, but file is %ld bytes", nbytes, filebuf->size);
}

char *to_binary(uint64_t n)
{
    char digits[64];
    int i = 0;

    while (n > 0) {
        digits[i++] = (n % 2 == 0) ? '0' : '1';
        n /= 2;
    }

    char *s, *sp;

    if (i == 0) {
        s = malloc(2);
        sp = s;
        *sp++ = '0';
    } else if (i > 0) {
        s = malloc(i + 1);
        sp = s;

        while (i > 0) {
            *sp++ = digits[--i];
        }
    }

    *sp = '\0';
    return s;
}

void draw_hexgrid()
{
    char *fbp = filebuf.data + hexgrid.fpos;
    int y = 0;
    int x = 0;

    werase(wmain.win);
    werase(wascii.win);

    for (int hpos = 0; hpos < hexgrid.cols * hexgrid.rows; ++hpos) {
        struct hex *h = hexgrid.data + hpos;
        h->fpos = fbp - filebuf.data;
        unsigned char n = *fbp++;
        h->digits[1] = hexgrid.lookup[n];
        h->digits[0] = hexgrid.lookup[n] >> 8;
        unsigned char ch = n;

        if (ch < 32 || ch > 126)
            ch = '.';

        if (x == 0) {
            char addr[11];
            snprintf(addr, 11, "%08lx: ", hexgrid.fpos + hpos);
            mvwaddstr(waddr.win, y, x, addr);
        }

        mvwaddch(wmain.win, y, (x * 3), h->digits[0]);
        mvwaddch(wmain.win, y, (x * 3) + 1, h->digits[1]);
        mvwaddch(wascii.win, y, x, ch);
        ++x;

        if (x >= hexgrid.cols) {
            x = 0;
            ++y;
        }

        if (h->fpos >= filebuf.size)
            break;
    }
}

void draw_numeric()
{
    char *faddr = (char *)(filebuf.data + hexgrid.cursfpos);
    uint64_t value = 0;

    werase(wnumeric.win);

    int bits = 1 << (abs(intsize) + 3);
    uint64_t mask = ~0ULL >> (64 - bits);
    value = *(uint64_t *)faddr & mask;

    if (intsize > 0)
        value = reverse_bytes(value, 1 << intsize);

    mvwprintw(wnumeric.win, 0, 0, "%s%s", (intsize > -3 ? "<" : "|"), (intsize < 3 ? ">" : "|"));
    mvwprintw(wnumeric.win, 0, 3, "%d-bit", bits);
    mvwprintw(wnumeric.win, 0, 10, (intsize != 0) ? (intsize < 0 ? "l-e" : "b-e") : "");
    mvwprintw(wnumeric.win, 2, 0, "int%d:  %ld", bits, value);
    mvwprintw(wnumeric.win, 3, 0, "uint%d: %lu", bits, value);
    mvwprintw(wnumeric.win, 5, 0, "hex: %lx", value);
    mvwprintw(wnumeric.win, 6, 0, "oct: %lo", value);

    char *binary = to_binary(value);
    mvwprintw(wnumeric.win, 7, 0, "bin: %s", binary);
    free(binary);

    werase(wstatus.win);
    mvwprintw(wstatus.win, 0, scr.xmax - strlen(filebuf.filename), filebuf.filename);
    if (replace_mode)
        mvwprintw(wstatus.win, 0, 10, "REPLACE");
    else if (insert_mode)
        mvwprintw(wstatus.win, 0, 10, "INSERT");
    mvwprintw(wstatus.win, 0, 0, "%x", hexgrid.cursfpos);
}

void init_hex_lookup()
{
    hexgrid.lookup = malloc(256 * sizeof(uint16_t));

    char hex_digits[] = {
        '0', '1', '2', '3',
        '4', '5', '6', '7',
        '8', '9', 'a', 'b',
        'c', 'd', 'e', 'f'
    };

    for (int n = 0; n < 256; ++n) {
        hexgrid.lookup[n]  = hex_digits[(int)(n / 16)] << 8;
        hexgrid.lookup[n] |= hex_digits[n % 16];
    }
}

void move_cursor(int yrel, int xrel)
{
    int top  = 0, bot   = hexgrid.rows - 1;
    int left = 0, right = hexgrid.cols - 1;

    struct hexgrid tgrid = hexgrid;

    tgrid.ypos += yrel;
    tgrid.digpos += xrel;

    if (tgrid.digpos < 0 || tgrid.digpos > 1) {
        tgrid.xpos += floor(tgrid.digpos / 2.0);
        tgrid.digpos = (tgrid.digpos + 2) % 2;
    }

    if (tgrid.xpos < left) {
        tgrid.xpos = right;
        --tgrid.ypos;
    } else if (tgrid.xpos > right) {
        tgrid.xpos = 0;
        ++tgrid.ypos;
    }

    if (tgrid.ypos < top) {
        tgrid.ypos = top;
        tgrid.fpos -= tgrid.cols;
    } else if (tgrid.ypos > bot) {
        tgrid.ypos = bot;
        tgrid.fpos += tgrid.cols;
    }

    tgrid.cursfpos = tgrid.fpos + (tgrid.ypos * tgrid.cols) + tgrid.xpos;

    if (tgrid.cursfpos >= 0 && tgrid.cursfpos <= filebuf.size) {
        hexgrid = tgrid;
        hexgrid = tgrid;
    }
}

void shift_intsize(int dir)
{
    intsize += dir;

    if (intsize < -3) {
        intsize = -3;
    } else if (intsize > 3) {
        intsize = 3;
    }
}

int interpret_hex_input(int ch)
{
    int value;

    if (ch >= '0' && ch <= '9')
        value = ch - '0';
    else if (ch >= 'A' && ch <= 'F')
        value = ch - 'A' + 10;
    else if (ch >= 'a' && ch <= 'f')
        value = ch - 'a' + 10;
    else
        return -1;

    int hex = *(filebuf.data + hexgrid.cursfpos);

    if (hexgrid.digpos == 0) {
        hex &= 0x0f;
        hex |= value << 4;
    } else if (hexgrid.digpos == 1) {
        hex &= 0xf0;
        hex |= value;
    }

    return hex;
}

void delete_hex()
{
    memmove( /* backspace in insert mode, else delete in place */
        filebuf.data + hexgrid.cursfpos - (insert_mode ? 1 : 0),
        filebuf.data + hexgrid.cursfpos + (insert_mode ? 0 : 1),
        filebuf.size - hexgrid.cursfpos - (insert_mode ? 0 : 1)
    );
    --filebuf.size;
    filebuf.data = realloc(filebuf.data, filebuf.size);
}


void modify_buffer(int ch)
{
    switch (ch) {
    case 27: insert_mode = 0; replace_mode = 0; return;
    case KEY_BACKSPACE: case 127:
        if (insert_mode && hexgrid.cursfpos > 0) {
            delete_hex();
            move_cursor(0, -2);
            return;
        }
        break;
    }

    int hex = interpret_hex_input(ch);

    if (hex >= 0) {
        if (insert_mode && hexgrid.digpos == 0) {
            ++filebuf.size;
            filebuf.data = realloc(filebuf.data, filebuf.size);

            memmove(
                filebuf.data + hexgrid.cursfpos + 1,
                filebuf.data + hexgrid.cursfpos,
                filebuf.size - hexgrid.cursfpos - 1
            );

            hex &= 0xf0;
        }

        *(filebuf.data + hexgrid.cursfpos) = hex;

        move_cursor(0, 1);
    }
}

void quit_program()
{
    running = 0;
}

void prompt(const char *msg, void (*f)())
{
    mvwprintw(wstatus.win, 0, 0, "%s (y/n) ", msg);
    wrefresh(wstatus.win);

    int ch = getch();

    if (ch == 'y')
        (*f)();
}

void write_file()
{
    FILE *fp;
    char *filename = filebuf.filename;

    if (!(fp = fopen(filename, "w")))
        errx(1, "load_file: failed to open %s", filename);

    int nbytes = 0;
    if ((nbytes = fwrite(filebuf.data, 1, filebuf.size, fp)) < filebuf.size)
        errx(1, "fwrite: wrote %d bytes, but data is %ld bytes", nbytes, filebuf.size);
}

void process_normal_mode(int ch)
{
    switch (ch) {
    case KEY_DOWN:
    case 'j':
        move_cursor(1, 0);  break;
    case KEY_UP:
    case 'k':
        move_cursor(-1, 0); break;
    case KEY_LEFT:
    case 'h':
        move_cursor(0, -1); break;
    case KEY_RIGHT:
    case 'l':
        move_cursor(0, 1);  break;
    case '<':
        shift_intsize(-1);  break;
    case '>':
        shift_intsize(1);   break;
    case 'r':
        replace_mode = 1;   break;
    case 'i':
        insert_mode = 1;    break;
    case 'x':
        delete_hex();       break;
    case 'w':
        prompt(
            "Save?",
            write_file
        );                  break;
    case 'q':
        prompt(
            "Quit?",
            quit_program
        );                  break;
    }
}

int main(int argc, char *argv[])
{
    char *filename;

    if (argc > 1)
        filename = argv[1];
    else
        errx(0, "Usage: %s <filename>", basename(argv[0]));

    init_hex_lookup();
    load_file(filename, &filebuf);
    init_curses();
    init_layout();

    hexgrid.fpos = hexgrid.cursfpos = 0;

    int ch = 0;
    while (1) {
        if (is_term_resized(scr.ymax, scr.xmax)) {
            init_layout();
        }

        if (replace_mode || insert_mode)
            modify_buffer(ch);
        else
            process_normal_mode(ch);

        if (!running)
            break;

        draw_hexgrid();
        draw_numeric();

        wmove(wmain.win, hexgrid.ypos, (hexgrid.xpos * 3) + hexgrid.digpos);

        wrefresh(waddr.win);
        wrefresh(wascii.win);
        wrefresh(wnumeric.win);
        wrefresh(wstatus.win);
        wrefresh(wmain.win);

        ch = getch();
    }

    endwin();

    free(filebuf.data);
    free(hexgrid.data);
    free(hexgrid.lookup);

    return 0;
}
