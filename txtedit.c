
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios og_termios;

void display_error(const char *s)
{
    perror(s);
    exit(1);
}

void disable_raw_mode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &og_termios) == -1) {
        display_error("tcsetattr");
    };
}


void enable_raw_mode()
{
    if (tcgetattr(STDIN_FILENO, &og_termios) == -1) {
        display_error("tcgetattr");
    };
    atexit(disable_raw_mode);

    struct termios raw = og_termios;

    /* disble flags*/
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag &= ~(CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);

    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        display_error("tcsetattr");
    };
    
}

int main()
{
    enable_raw_mode();

    while(1) {
        char c = '\0';
        if (read(STDIN_FILENO, &c, 1) == -1 && errno != EAGAIN) {
            display_error("read");
        }
        if(iscntrl(c)) {
            printf("%d\r\n", c);
        }
        else {
            printf("%d ('%c')\r\n", c, c);
        }
        if (c == 'q') {
            break;
        }
    }

    return 0;
}