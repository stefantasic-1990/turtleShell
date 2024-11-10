#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <termios.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>

#define CMD_HISTORY_SIZE 10

FILE* cmd_history_fp; // file pointer
char cmd_history_fn[] = "./tshcmdhisory.txt"; // command history file name
char* cmd_history[CMD_HISTORY_SIZE] = {NULL}; // command history

struct termios terminal_settings; // original terminal settings
int pid = -1; // global pid to differentiate parent when doing atexit()

int enableRawTerminal() {
    struct termios modified_settings;

    if (tcgetattr(STDIN_FILENO, &terminal_settings) == -1) {return 1;} 

    // check TTY device
    if (!isatty(STDIN_FILENO)) {return -1;} 

    // change terminal settings
    if (tcgetattr(STDIN_FILENO, &modified_settings) == -1) {return 1;} 
    modified_settings.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    modified_settings.c_oflag &= ~(OPOST);
    modified_settings.c_cflag |= (CS8);
    modified_settings.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    modified_settings.c_cc[VMIN] = 1; 
    modified_settings.c_cc[VTIME] = 0;

    // set new terminal settings
    if (tcsetattr(STDIN_FILENO,TCSAFLUSH,&modified_settings) == -1) {return -1;};

    return 0;
}

void disableRawTerminal() {
    // restore initial settings
    if (pid != 0) {tcsetattr(STDIN_FILENO,TCSAFLUSH,&terminal_settings);}
}

int toggleOutputPostprocessing() {
    struct termios modified_settings;

    // change terminal settings
    if (tcgetattr(STDIN_FILENO, &modified_settings) == -1) {return 1;} 
    modified_settings.c_oflag ^= (OPOST);

    // set terminal settings
    if (tcsetattr(STDIN_FILENO,TCSAFLUSH,&modified_settings) == -1) {return -1;};

    return 0;
}

int tshExecuteCommand(char** cmd, int in, int out) {
    int status;

    // check if command is builtin
    if (strcmp(cmd[0], "cd") == 0 && out == 1) {
        if (cmd[1] == NULL) {return 1;}
        if (chdir(cmd[1]) != 0) {return -1;}
    } 
    if (strcmp(cmd[0], "exit") == 0 && out == 1) {
        exit(EXIT_SUCCESS);
    }

    // create child process
    pid = fork();
    if (pid == 0) {
        // reidirect input/output
        if (in != 0) {
            dup2(in, 0);
            close (in);
        }
        if (out != 1) {
            dup2(out, 1);
            close(out);
        }
        // child process execute command
        execvp(cmd[0], cmd);
        // child process exits if execvp can't find the executable in the path
        exit(EXIT_FAILURE);
    } else if (pid < 0 ) {
        // fork error
        return -1;
    } else {
        // parent process wait for child
        do {waitpid(pid, &status, WUNTRACED);}
        while (!WIFEXITED(status) && !WIFSIGNALED(status));
    }

    return 0;
}


int tshParseCommandLine(char** args) {
    int pipefd[2]; // pipe file descriptor array
    int nextin = 0; // file descriptor to be used for next input stream
    int cmd_start = 0; // command string start index
    int cmd_end = 0; // command string end index
    int cmd_len = 0; // command length in arg
    int i = 0; // loop index
    char** cmd; // current command arg array
    char* fn; // file name
    FILE* fp; // file pointer

    // check if token array is empty
    if (strcmp(args[0], "\0") == 0) {return 0;}

    // turn off output postprocessing
    toggleOutputPostprocessing();

    // parse args array from left to right and figure out the next command
    while (1) {
        // if  null string run command and stop parsing
        if (strcmp(args[i], "\0") == 0) {
            cmd_len = cmd_end - cmd_start;
            cmd = calloc(cmd_len, sizeof(char*));
            memcpy(cmd, args + cmd_start, cmd_len*sizeof(char*));
            tshExecuteCommand(cmd, nextin, 1);
            goto end;
        // if && run command and do no piping or redirection
        } else if (strcmp(args[i], "&&") == 0 && strcmp(args[i + 1], "\0") != 0) {
            cmd_len = cmd_end - cmd_start;
            cmd = calloc(cmd_len, sizeof(char*));
            memcpy(cmd, args + cmd_start, cmd_len*sizeof(char*));
            tshExecuteCommand(cmd, nextin, 1);
            nextin = 0;
            cmd_len = 0;
            cmd_end++;
            cmd_start = cmd_end;
        // if | run command and pipe to next
        } else if (strcmp(args[i], "|") == 0 && strcmp(args[i + 1], "\0") != 0) {
            cmd_len = cmd_end - cmd_start;
            cmd = calloc(cmd_len, sizeof(char*));
            memcpy(cmd, args + cmd_start, cmd_len*sizeof(char*));
            pipe(pipefd);
            tshExecuteCommand(cmd, nextin, pipefd[1]);
            close(pipefd[1]);
            nextin = pipefd[0];
            cmd_len = 0;
            cmd_end++;
            cmd_start = cmd_end;
        // if > redirect output stream of current command to given file, run command
        } else if (strcmp(args[i], ">") == 0 && strcmp(args[i+1], "\0") != 0) {
            cmd_len = cmd_end - cmd_start;
            cmd = calloc(cmd_len, sizeof(char*));
            memcpy(cmd, args + cmd_start, cmd_len*sizeof(char*));
            fn = args[cmd_end + 1];
            fp = fopen(fn, "a+");
            tshExecuteCommand(cmd, nextin, fileno(fp));
            fclose(fp);
            memmove(args + 2, args + cmd_start, cmd_len*sizeof(char*));
            cmd_start += 2;
            cmd_end++;
        // if < redirect input stream of current command to given file, run command
        } else if (strcmp(args[i], "<") == 0) {
            continue;
        // add arg to current command
        } else {
            cmd_end++;
            cmd_len++;
        }
        // increment loop index
        i++;
    }

    end:
        // turn on output postprocessing
        toggleOutputPostprocessing();
        return 0;
}

char** tshTokenizeLine(char* line) {
    int line_p = 0; // line buffer position
    int args_s = 10; // args buffer size
    int args_p = 0; // args buffer position
    int arg_s = 20; // arg buffer size
    int arg_p = 0; // arg buffer position
    int qmode = 0; // quoted mode flag 
    char** args = calloc(args_s, sizeof(char*));

    while (1) {
        // allocate space for next token
        args[args_p] =  calloc(arg_s, sizeof(char));

        // parse next token
        while (1) {
            // if end of line is reached
            switch (line[line_p]) {
            case '\0':
                // if we are building a token null-terminate it
                if (arg_p != 0) {
                    args[args_p][arg_p] = '\0';
                    args[args_p+1] = "\0";
                } else {args[args_p] = "\0";}
                return args;
            case '\"':
                // change quoted mode
                qmode = 1 - qmode;
                line_p++;
                break;
            // // if escape character
            case '\\':
                // get next character and determine escape sequence
                // we ignore the escape character by itself
                line_p++;
                switch(line[line_p]) {
                case 'n':
                    args[args_p][arg_p] = '\r';
                    arg_p++;
                    args[args_p][arg_p] = '\n';
                    arg_p++;
                    line_p++;
                    break;
                case '\\':
                    args[args_p][arg_p] = '\\';
                    arg_p++;
                    line_p++;
                    break;
                case '\"':
                    args[args_p][arg_p] = '\"';
                    arg_p++;
                    line_p++;
                    break;
                case '\'':
                    args[args_p][arg_p] = '\'';
                    arg_p++;
                    line_p++;
                    break;
                case 'r':
                    args[args_p][arg_p] = '\r';
                    arg_p++;
                    line_p++;
                    break;
                }
                break;
            // if space character
            case ' ':
                // only treat as a token separator if not in quoted mode
                // otherwise we skip down to default and treat as regular character
                if (qmode == 0) {
                    // if we are building a token null-terminate it, set next token to NULL
                    if (arg_p != 0) {
                        args[args_p][arg_p] = '\0';
                        line_p++;
                        goto nextToken;
                        }
                    // if we are not building a token, ignore the white space
                    line_p++;
                    break;
                }
                // if we are building a token null-terminate it, set next token to NULL
            // if regular character
            default:
                // add character to token string
                args[args_p][arg_p] = line[line_p];
                line_p++;
                arg_p++;
            }
            // reallocate more token characters if required
            if (arg_p >= arg_s) {
            arg_s += arg_s;
            args[args_p] = realloc(args[args_p], arg_s * sizeof(char));
            if (!args[args_p]) {return NULL;}
            }  
        }

        nextToken:
            // set variables for next token string
            args_p++;
            arg_p = 0;

            // reallocate more token pointers if required
            if (args_p >= args_s) {
            args_s += args_s;
            args = realloc(args, args_s * sizeof(char*));
            if (!args) {return NULL;}
            } 
    }
}

char* tshGetLine(char* prompt, int prompt_l, char* cmd_history[CMD_HISTORY_SIZE]) {
    int cmd_history_s = CMD_HISTORY_SIZE; // command history size
    int cmd_history_p = 0; // command history position
    int buffer_s = 100; // line buffer total size
    int buffer_l = 0; // line buffer character length
    int buffer_p = 0; // line buffer cursor position
    int buffer_o = 0; // line buffer display offset
    int buffer_dl = 0; // line buffer display length
    char* buffer; // line buffer
    char cursor_p[10]; // cursor position escape sequence
    char c; // input character
    char eseq[3]; // input escape sequence
    int window_c; // terminal window column width

    // initialize line buffer
    buffer = calloc(buffer_s, sizeof(char)); // line buffer
    buffer[0] = '\0';

    // get line display length
    struct winsize ws;
    if (ioctl(1, TIOCGWINSZ, &ws) == -1) {window_c = 80;} else {window_c = ws.ws_col - 1;}
    buffer_dl = window_c - prompt_l;

    do {
        // refresh line
        snprintf(cursor_p, sizeof(cursor_p), "\x1b[%iG", prompt_l + 1 + buffer_p - buffer_o);
        write(STDOUT_FILENO, "\x1b[0G", strlen("\x1b[0G"));
        write(STDOUT_FILENO, prompt, prompt_l);
        write(STDOUT_FILENO, (buffer + buffer_o), (buffer_s < buffer_dl) ? buffer_s : buffer_dl);
        write(STDOUT_FILENO, "\x1b[0K", strlen("\x1b[0K"));
        write(STDOUT_FILENO, cursor_p, strlen(cursor_p));
        // read-in next character
        read(STDIN_FILENO, &c, 1);

        // handle character
        switch(c) {
            case 13: // enter
                goto returnLine;
            case 8: // ctrl+h
            case 127: // backspace
                if (buffer_p > 0) {
                    memmove(buffer+(buffer_p-1), buffer+buffer_p , buffer_l - buffer_p);
                    buffer_p--;
                    buffer_l--;
                    buffer[buffer_l] = '\0';
                }
                break;
            case 3: // ctrl+c
                exit(EXIT_SUCCESS);
            case 4: // ctrl+d
                break;
            case 20: // ctrl+t
                break; 
            case 16: // ctrl+p
                break; 
            case 14: // ctrl+n
                break;
            case 11: // ctrl+k
                break;
            case 1: // ctrl+a
                break;
            case 5: // ctrl+e
                break;
            case 12: // ctrl+l
                break;
            case 23: // ctrl+w
                break;
            case 21: // ctrl+u
                free(buffer);
                buffer = calloc(buffer_s, sizeof(char));
                buffer_p = 0;
                buffer_o = 0;
                buffer_l = 0;
                break;
            case 27: // escape character
                // read-in the next two characters
                if (read(STDOUT_FILENO, eseq, 1) == -1) {break;}
                if (read(STDOUT_FILENO, eseq+1, 1) == -1) {break;}
                if (eseq[0] == '[') {
                    switch(eseq[1]) {
                        case 'C': // right arrow key
                            if (buffer_p < buffer_l) {buffer_p++;}
                            if ((buffer_p - buffer_o) > buffer_dl) {buffer_o++;}
                            break;
                        case 'D': // left arrow key
                            if (buffer_p > 0) {buffer_p--;}
                            if ((buffer_p - buffer_o) < 0) {buffer_o--;}
                            break;
                        case 'A': // up arrow key
                            // get previous record in history
                            if ((cmd_history[cmd_history_p + 1] != NULL) && cmd_history_p < (cmd_history_s - 1)) {
                                if (cmd_history == 0) {cmd_history[0] = strdup(buffer);}
                                cmd_history_p++;
                                buffer_l = strlen(cmd_history[cmd_history_p]);
                                buffer_s = strlen(cmd_history[cmd_history_p]) + 1;
                                buffer_p = buffer_l;
                                buffer_o = (buffer_l < buffer_dl) ? 0 : buffer_l - buffer_dl;
                                free(buffer);
                                buffer = calloc(buffer_s, sizeof(char));
                                strcpy(buffer, cmd_history[cmd_history_p]);
                            }
                            break;
                        case 'B': // down arrow key
                            // get next record in history
                            if (cmd_history_p > 0) {
                                cmd_history_p--;
                                buffer_l = strlen(cmd_history[cmd_history_p]);
                                buffer_s = strlen(cmd_history[cmd_history_p]) + 1;
                                buffer_p = buffer_l;
                                buffer_o = (buffer_l < buffer_dl) ? 0 : buffer_l - buffer_dl;
                                free(buffer);
                                buffer = calloc(buffer_s, sizeof(char));
                                strcpy(buffer, cmd_history[cmd_history_p]);
                            }    
                            break;
                    }
                }
                break;
            default: // store character in buffer
                memmove(buffer+buffer_p+1, buffer+buffer_p, buffer_l - buffer_p);
                buffer[buffer_p] = c;
                buffer_p++;
                buffer_l++;
                buffer[buffer_l] = '\0';
                if ((buffer_p - buffer_o) > buffer_dl) {buffer_o++;}
                break;
        }
        // allocate more space for buffer if required
        if (buffer_l + 1 >= buffer_s) {
            buffer_s += buffer_s;
            buffer = realloc(buffer, buffer_s);
            if (!buffer) {return NULL;}
        }
    } while (1);

    returnLine:
        // free first history element
        free(cmd_history[0]);
        cmd_history[0] = NULL;
        // if command not empty, copy into history
        if (buffer[0] != '\0') {
            // free last element memory
            free(cmd_history[cmd_history_s - 1]);
            // move elements, ignore first which is reserved for current line edit
            memmove(cmd_history + 2, cmd_history + 1, cmd_history_s*sizeof(char*)*2 - sizeof(char*)*2);
            cmd_history[1] = strdup(buffer);
        }
        write(STDOUT_FILENO, "\r\n", sizeof("\r\n"));
        return buffer;
}

int restoreCmdHistory() {
    int i = 1; // loop index
    char* cmd = NULL; // command
    size_t cmd_len; // command length
    
    cmd_history_fp = fopen(cmd_history_fn, "a+");
    if (cmd_history_fp == NULL) {
        return -1;
    }

    rewind(cmd_history_fp);   

    while (i < CMD_HISTORY_SIZE) {
        getline(&cmd, &cmd_len, cmd_history_fp);
        // if command available and file is not new
        if (strcmp(cmd, "\0") != 0) {
            // overwrite newline character and load
            cmd[strlen(cmd)-1] = '\0';
            cmd_history[i] = strdup(cmd);
        }
        i++;
    }

    return 0;
}

int main(int argc, char **argv) {
    char host[_POSIX_HOST_NAME_MAX]; // machine hostname
    char cwd[PATH_MAX]; // current working directory
    char prompt[50]; // prompt
    char** cmd_tokens; // command line tokens
    char* cmd_line; // command line
    int prompt_l; // prompt character length

    // INITIALIZATION
    if (enableRawTerminal() == -1 || // enable raw terminal mode
        atexit(disableRawTerminal) != 0 || // at exit restore initial terminal settings
        restoreCmdHistory() == -1 // restore command history from file
    ) {
        return -1;
    }

    // COMMAND LOOP
    do {
        // get hostname and current working directory
        if (gethostname(host, sizeof(host)) == -1 || 
            getcwd(cwd, sizeof(cwd)) == NULL
        ) {
            return -1;
        }

        // create shell prompt and get its length
        prompt_l = snprintf(prompt, 50, "%s@%s %s: ", getlogin(), host, strrchr(cwd, '/'));

        // get-parse-execute the command line
        cmd_line = tshGetLine(prompt, prompt_l, cmd_history);
        cmd_tokens = tshTokenizeLine(cmd_line);
        tshParseCommandLine(cmd_tokens);

        // save command history
        cmd_history_fp = fopen(cmd_history_fn, "a+");
        ftruncate(fileno(cmd_history_fp), 0);
        for (int i = 1; i < CMD_HISTORY_SIZE; i++) {
            if (cmd_history[i] != NULL) {
                fprintf(cmd_history_fp, "%s\n", cmd_history[i]);
                fflush(cmd_history_fp);
            }
        }
        free(cmd_line);
        free(cmd_tokens);
    } while (1);
}