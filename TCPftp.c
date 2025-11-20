 /* TCPftp.c - main, sendCmd, pasivo */

#define _POSIX_C_SOURCE 200809L

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#include <sys/select.h>
#include <sys/time.h>


extern int  errno;

int  errexit(const char *format, ...);
int  connectTCP(const char *host, const char *service);
int  passiveTCP(const char *service, int qlen);

#define LINELEN 512
#define DATA_BUFSIZE 1024

/* ------------------ Globals for mget/process control ------------------ */
volatile sig_atomic_t children_count = 0;
int MAX_PROCS = 4; /* default, can be adjusted via FTP_PROCS env var */
/* restart offset para REST (aplicado en la siguiente RETR) */
long restart_offset = 0;


/* SIGCHLD handler: reap finished children and decrement counter */
void sigchld_handler(int signo) {
    (void)signo;
    int saved_errno = errno;
    while (1) {
        int status;
        pid_t pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0) break;
        if (children_count > 0) children_count--;
    }
    errno = saved_errno;
}

/* set up the SIGCHLD handler using sigaction */
void setup_sigchld(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) < 0) {
        perror("sigaction");
        /* not fatal, but warn */
    }
}

/* ------------------ I/O helpers ------------------ */
ssize_t send_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *p = buf;
    while (total < len) {
        ssize_t n = send(fd, p + total, len - total, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += n;
    }
    return (ssize_t)total;
}

ssize_t recv_line(int fd, char *buf, size_t max) {
    size_t idx = 0;
    char c;
    while (idx < max - 1) {
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) return n;
        buf[idx++] = c;
        if (c == '\n') break;
    }
    buf[idx] = '\0';
    return (ssize_t)idx;
}

int recv_response(int s, char *res, size_t rsz) {
    ssize_t n = recv_line(s, res, rsz);
    if (n <= 0) return -1;
    printf("%s", res);
    if (strlen(res) >= 3 && isdigit((unsigned char)res[0]) &&
        isdigit((unsigned char)res[1]) && isdigit((unsigned char)res[2])) {
        int code = (res[0]-'0')*100 + (res[1]-'0')*10 + (res[2]-'0');
        return code;
    }
    return -1;
}

/* safe send command: cmd_in no CRLF, res buffer captures response */
int sendCmd(int s, const char *cmd_in, char *res, size_t rsz) {
    char buf[1024];
    snprintf(buf, sizeof(buf)-3, "%s", cmd_in);
    strcat(buf, "\r\n");
    if (send_all(s, buf, strlen(buf)) < 0) {
        perror("send");
        return -1;
    }
    return recv_response(s, res, rsz);
}

/* ------------------ PASV ------------------ */
int pasivo(int s) {
    char res[LINELEN];
    if (sendCmd(s, "PASV", res, sizeof(res)) < 0) return -1;
    char *p = strchr(res, '(');
    if (!p) { fprintf(stderr, "PASV: respuesta malformada: %s\n", res); return -1; }
    int h1,h2,h3,h4,p1,p2;
    if (sscanf(p+1, "%d,%d,%d,%d,%d,%d", &h1,&h2,&h3,&h4,&p1,&p2) != 6) {
        fprintf(stderr, "PASV: sscanf fallo\n"); return -1;
    }
    char host[64];
    snprintf(host, sizeof(host), "%d.%d.%d.%d", h1,h2,h3,h4);
    int port = p1*256 + p2;
    char sport[32];
    snprintf(sport, sizeof(sport), "%d", port);
    int sdata = connectTCP(host, sport);
    if (sdata < 0) return -1;
    return sdata;
}

/* ------------------ pput (PORT - modo activo) robusto -------------------
 *
 * - No usa passiveTCP. Crea localmente un socket listening (bind port 0).
 * - Determina la IP local "real" usada para alcanzar al servidor usando
 *   un socket UDP conectado al peer del socket de control.
 * - Envía "PORT h1,h2,h3,h4,p1,p2", envía "STOR <file>", espera accept()
 *   con timeout, transmite el archivo y cierra todo correctamente.
 */
int pput(int s, const char *localfile) {
    char res[LINELEN], cmd[256];
    int s_listen = -1, sdata = -1;
    FILE *fp = NULL;
    struct sockaddr_in addr;
    socklen_t alen = sizeof(addr);

    /* 1) crear socket de escucha local y bindear a port 0 (ephemeral) */
    s_listen = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen < 0) { perror("socket"); return -1; }

    /* permitir quick reuse */
    {
        int opt = 1;
        setsockopt(s_listen, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(0); /* 0 => kernel elige puerto */

    if (bind(s_listen, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        perror("bind");
        close(s_listen);
        return -1;
    }
    if (listen(s_listen, 1) < 0) {
        perror("listen");
        close(s_listen);
        return -1;
    }

    /* 2) averiguar puerto asignado */
    if (getsockname(s_listen, (struct sockaddr*)&addr, &alen) < 0) {
        perror("getsockname");
        close(s_listen);
        return -1;
    }
    unsigned short port = ntohs(addr.sin_port);

    /* 3) determinar la IP local "correcta" usando un socket UDP conectado al peer */
    struct sockaddr_storage peer;
    socklen_t plen = sizeof(peer);
    if (getpeername(s, (struct sockaddr*)&peer, &plen) < 0) {
        perror("getpeername");
        close(s_listen);
        return -1;
    }

    int udp = socket(((struct sockaddr*)&peer)->sa_family, SOCK_DGRAM, 0);
    if (udp < 0) {
        perror("socket udp");
        close(s_listen);
        return -1;
    }
    /* conectar UDP al peer (no envía nada) para que el kernel seleccione interfaz */
    if (connect(udp, (struct sockaddr*)&peer, plen) < 0) {
        /* si falla, aún podemos probar con getsockname(control) como fallback */
        /* perror("connect udp (fallback)"); */
    }

    struct sockaddr_storage localss;
    socklen_t localss_len = sizeof(localss);
    if (getsockname(udp, (struct sockaddr*)&localss, &localss_len) < 0) {
        /* fallback: intenta getsockname() del socket de control */
        struct sockaddr_in localaddr;
        socklen_t localalen = sizeof(localaddr);
        if (getsockname(s, (struct sockaddr*)&localaddr, &localalen) < 0) {
            perror("getsockname(control) fallback");
            close(udp);
            close(s_listen);
            return -1;
        } else {
            memcpy(&localss, &localaddr, sizeof(localaddr));
            localss_len = sizeof(localaddr);
        }
    }
    close(udp);

    char local_ip_str[INET6_ADDRSTRLEN] = {0};
    if (localss.ss_family == AF_INET) {
        struct sockaddr_in *sinp = (struct sockaddr_in*)&localss;
        inet_ntop(AF_INET, &sinp->sin_addr, local_ip_str, sizeof(local_ip_str));
    } else {
        /* no soportamos IPv6 para PORT (IPv4) en esta versión */
        fprintf(stderr, "pput: IPv6 no soportado en PORT\n");
        close(s_listen);
        return -1;
    }

    /* 4) construir PORT h1,h2,h3,h4,p1,p2 */
    char ip_commas[64];
    snprintf(ip_commas, sizeof(ip_commas), "%s", local_ip_str);
    for (size_t i = 0; i < strlen(ip_commas); ++i) if (ip_commas[i]=='.') ip_commas[i]=',';

    int p1 = port / 256;
    int p2 = port % 256;
    snprintf(cmd, sizeof(cmd), "PORT %s,%d,%d", ip_commas, p1, p2);

    /* 5) enviar PORT y comprobar respuesta */
    int code = sendCmd(s, cmd, res, sizeof(res));
    if (code < 0) {
        fprintf(stderr, "Error enviando PORT\n");
        close(s_listen);
        return -1;
    }
    /* server puede devolver 200 o error (4xx/5xx) */
    if (code >= 400) {
        fprintf(stderr, "Server PORT error: %s\n", res);
        close(s_listen);
        return -1;
    }

    /* 6) enviar STOR */
    char storcmd[256];
    snprintf(storcmd, sizeof(storcmd), "STOR %s", localfile);
    code = sendCmd(s, storcmd, res, sizeof(res));
    if (code < 0) { fprintf(stderr, "Error enviando STOR\n"); close(s_listen); return -1; }
    if (code >= 400) { fprintf(stderr, "Server STOR error: %s\n", res); close(s_listen); return -1; }

    /* 7) esperar accept con timeout */
    fd_set rfds;
    struct timeval tv;
    FD_ZERO(&rfds);
    FD_SET(s_listen, &rfds);
    tv.tv_sec = 8;
    tv.tv_usec = 0;
    int sel = select(s_listen + 1, &rfds, NULL, NULL, &tv);
    if (sel <= 0) {
        if (sel == 0) fprintf(stderr, "pput: timeout esperando conexión de datos (server no conectó)\n");
        else perror("select");
        close(s_listen);
        return -1;
    }

    alen = sizeof(addr);
    sdata = accept(s_listen, (struct sockaddr *)&addr, &alen);
    if (sdata < 0) { perror("accept"); close(s_listen); return -1; }

    /* 8) enviar archivo */
    fp = fopen(localfile, "rb");
    if (!fp) { perror("fopen"); close(sdata); close(s_listen); return -1; }

    char buf[DATA_BUFSIZE];
    size_t nread;
    while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (send_all(sdata, buf, nread) < 0) {
            perror("send_all");
            fclose(fp);
            close(sdata);
            close(s_listen);
            return -1;
        }
    }
    fclose(fp);

    /* 9) cerrar sdata y s_listen */
    close(sdata);
    close(s_listen);

    /* 10) leer la respuesta final del control */
    if (recv_response(s, res, sizeof(res)) < 0) return -1;
    return 0;
}


/* ------------------ mget (procesos, usando fork) ------------------ */
/* Cada proceso hijo hace su propia conexión de control, autentica y RETR */
int do_mget_fork(const char *host, const char *service, const char *user, const char *pass, char *filename) {
    pid_t pid;
    /* Si estamos al limite, esperar (liberado por SIGCHLD handler) */
    while (children_count >= MAX_PROCS) {
        /* dormir corto y reintentar */
        struct timespec ts = {0, 200000000}; /* 200ms */
        nanosleep(&ts, NULL);
    }

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        /* CHILD */
        printf("[child %d] Empezando a descargar %s\n", getpid(), filename);
        sleep(10); // <---- SOLO PARA VERIFICAR

        int ctrl = connectTCP(host, service);
        if (ctrl < 0) {
            fprintf(stderr, "[child] connectTCP fallo\n");
            exit(1);
        }
        char res[LINELEN];

        if (recv_response(ctrl, res, sizeof(res)) < 0) { close(ctrl); exit(1); }

        char cmd[256];
        snprintf(cmd, sizeof(cmd), "USER %s", user);
        sendCmd(ctrl, cmd, res, sizeof(res));
        snprintf(cmd, sizeof(cmd), "PASS %s", pass);
        sendCmd(ctrl, cmd, res, sizeof(res));

        int sdata = pasivo(ctrl);
        if (sdata < 0) { close(ctrl); exit(1); }

        snprintf(cmd, sizeof(cmd), "RETR %s", filename);
        sendCmd(ctrl, cmd, res, sizeof(res));
        FILE *fp = fopen(filename, "wb");
        if (!fp) { perror("fopen child"); close(sdata); close(ctrl); exit(1); }
        ssize_t n;
        char databuf[DATA_BUFSIZE];
        while ((n = recv(sdata, databuf, sizeof(databuf), 0)) > 0) {
            fwrite(databuf, 1, n, fp);
        }
        fclose(fp);
        close(sdata);
        recv_response(ctrl, res, sizeof(res));
        close(ctrl);
        exit(0);
    } else {
        /* PARENT: incrementa contador y retorna */
        children_count++;
        return 0;
    }
}

/* ------------------ ayuda ------------------ */
void ayuda() {
    printf("Cliente FTP (modificado)\n");
    printf("Comandos disponibles:\n");
    printf("  dir                 - listar directorio remoto (LIST)\n");
    printf("  get <remoto>        - descargar archivo (RETR). Use REST antes para reanudar\n");
    printf("  put <local>         - subir archivo (PASV)\n");
    printf("  pput <local>        - subir archivo (PORT / activo)\n");
    printf("  mget <f1> <f2> ...  - descargar archivos en paralelo (forks)\n");
    printf("  mkd <dir>           - crea directorio remoto (MKD)\n");
    printf("  pwd                 - muestra directorio remoto (PWD)\n");
    printf("  dele <file>         - borra archivo remoto (DELE)\n");
    printf("  rest <offset>       - prepara REST para la siguiente descarga (RETR)\n");
    printf("  cd <dir>            - CWD (cambiar directorio remoto)\n");
    printf("  quit                - salir\n");
    printf("  help                - despliega este texto de ayuda\n\n");
}


/* ------------------ main ------------------ */
int main(int argc, char *argv[]) {
    char *host = "localhost";
    char *service = "21";
    if (argc >= 2) host = argv[1];
    if (argc >= 3) service = argv[2];

    /* configurar concurrencia si existe variable de entorno */
    char *env = getenv("FTP_PROCS");
    if (env) {
        int v = atoi(env);
        if (v > 0) MAX_PROCS = v;
    }

    /* instalar SIGCHLD handler */
    setup_sigchld();

    /* ignorar SIGPIPE para evitar termination on write to closed socket */
    struct sigaction sa2;
    memset(&sa2, 0, sizeof(sa2));
    sa2.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa2, NULL);

    /* Conectar control principal */
    int s = connectTCP(host, service);
    char res[LINELEN];
    if (recv_response(s, res, sizeof(res)) < 0) errexit("No banner\n");

    /* pedir user/pass una vez (los hijos los usarán) */
    char user[128], pass[128];
    printf("USER: ");
    if (!fgets(user, sizeof(user), stdin)) exit(0);
    user[strcspn(user, "\n")] = 0;
    printf("PASS: ");
    if (!fgets(pass, sizeof(pass), stdin)) exit(0);
    pass[strcspn(pass, "\n")] = 0;

    /* login en conexión principal (opcional) */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "USER %s", user);
    sendCmd(s, cmd, res, sizeof(res));
    snprintf(cmd, sizeof(cmd), "PASS %s", pass);
    sendCmd(s, cmd, res, sizeof(res));

    ayuda();
    char line[512];
    while (1) {
        printf("ftp> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = 0;
        char *tok = strtok(line, " ");
        if (!tok) continue;

        if (strcmp(tok, "help") == 0) { ayuda(); continue; }

        if (strcmp(tok, "dir") == 0) {
            int sdata = pasivo(s);
            if (sdata < 0) { fprintf(stderr, "pasivo fallo\n"); continue; }
            sendCmd(s, "LIST", res, sizeof(res));
            ssize_t n;
            char buf[DATA_BUFSIZE];
            while ((n = recv(sdata, buf, sizeof(buf), 0)) > 0) fwrite(buf,1,n,stdout);
            close(sdata);
            recv_response(s, res, sizeof(res));
            continue;
        }

        if (strcmp(tok, "get") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: get <remote>\n"); continue; }

            /* Si se definió restart_offset, volvemos a poner TYPE I y REST (por seguridad) */
            if (restart_offset > 0) {
                char restres[LINELEN];
                if (sendCmd(s, "TYPE I", restres, sizeof(restres)) < 0) {
                    fprintf(stderr, "Error estableciendo TYPE I\n");
                    /* no abortamos: intentamos continuar */
                }
                char cmdrest[256];
                snprintf(cmdrest, sizeof(cmdrest), "REST %ld", restart_offset);
                int code = sendCmd(s, cmdrest, restres, sizeof(restres));
                if (code >= 300 && code < 400) {
                    printf("REST %ld aceptado por servidor; reanudando.\n", restart_offset);
                } else {
                    printf("REST no aceptado por servidor: %s\n", restres);
                    /* limpiar para no intentar de nuevo */
                    restart_offset = 0;
                }
            }

            int sdata = pasivo(s);
            if (sdata < 0) { fprintf(stderr, "pasivo fallo\n"); continue; }

            snprintf(cmd, sizeof(cmd), "RETR %s", arg);
            sendCmd(s, cmd, res, sizeof(res));

            /* Abrir/crear el fichero local de forma que podamos reanudar (r+b o w+b) */
            FILE *fp = fopen(arg, "r+b");
            if (!fp) {
                /* no existe -> crear */
                fp = fopen(arg, "w+b");
                if (!fp) {
                    perror("fopen");
                    close(sdata);
                    /* limpiar restart_offset (no aplicado) */
                    restart_offset = 0;
                    continue;
                }
            }

            /* Si hay restart_offset, posicionar el puntero local */
            if (restart_offset > 0) {
                if (fseek(fp, restart_offset, SEEK_SET) != 0) {
                    perror("fseek");
                    /* seguimos de todas formas; la escritura empezará donde el SO permita */
                }
            } else {
                /* si no reanudamos, truncamos el archivo (por si existía) */
                if (ftruncate(fileno(fp), 0) != 0) {
                    /* no crítico; sólo aviso */
                    /* perror("ftruncate"); */
                }
            }

            ssize_t n;
            char buf[DATA_BUFSIZE];
            while ((n = recv(sdata, buf, sizeof(buf), 0)) > 0) {
                size_t wrote = fwrite(buf,1,n,fp);
                if (wrote != (size_t)n) {
                    perror("fwrite");
                    break;
                }
            }
            fclose(fp);
            close(sdata);
            recv_response(s, res, sizeof(res));

            /* limpiar restart_offset ya aplicado */
            restart_offset = 0;
            continue;
        }



        if (strcmp(tok, "put") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: put <file>\n"); continue; }
            int sdata = pasivo(s);
            if (sdata < 0) { fprintf(stderr, "pasivo fallo\n"); continue; }
            snprintf(cmd, sizeof(cmd), "STOR %s", arg);
            sendCmd(s, cmd, res, sizeof(res));
            FILE *fp = fopen(arg, "rb");
            if (!fp) { perror("fopen"); close(sdata); continue; }
            size_t r; char buf[DATA_BUFSIZE];
            while ((r = fread(buf,1,sizeof(buf),fp)) > 0) {
                if (send_all(sdata, buf, r) < 0) { perror("send data"); break; }
            }
            fclose(fp);
            close(sdata);
            recv_response(s, res, sizeof(res));
            continue;
        }

        if (strcmp(tok, "pput") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: pput <file>\n"); continue; }
            if (pput(s, arg) == 0) printf("pput OK\n"); else printf("pput fallo\n");
            continue;
        }

        if (strcmp(tok, "mget") == 0) {
            char *file;
            /* spawn child process for each file */
            while ((file = strtok(NULL, " ")) != NULL) {
                if (do_mget_fork(host, service, user, pass, file) < 0) {
                    fprintf(stderr, "No se pudo lanzar proceso para %s\n", file);
                } else {
                    printf("Lanzado proceso para %s (active children=%d)\n", file, (int)children_count);
                }
            }
            /* wait until all children finish */
            while (children_count > 0) {
                struct timespec ts = {0, 200000000}; /* 200ms */
                nanosleep(&ts, NULL);
            }
            printf("mget completo\n");
            continue;
        }

        /* PWD - mostrar directorio remoto */
        if (strcmp(tok, "pwd") == 0 || strcmp(tok, "PWD") == 0) {
            sendCmd(s, "PWD", res, sizeof(res));
            continue;
        }

        /* MKD - crear directorio remoto */
        if (strcmp(tok, "mkd") == 0 || strcmp(tok, "MKD") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: MKD <dir>\n"); continue; }
            char cmdmk[256];
            snprintf(cmdmk, sizeof(cmdmk), "MKD %s", arg);
            sendCmd(s, cmdmk, res, sizeof(res));
            continue;
        }

        /* DELE - borrar archivo remoto */
        if (strcmp(tok, "dele") == 0 || strcmp(tok, "DELE") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: DELE <file>\n"); continue; }
            char cmddel[256];
            snprintf(cmddel, sizeof(cmddel), "DELE %s", arg);
            sendCmd(s, cmddel, res, sizeof(res));
            continue;
        }

        /* REST - establecer offset para la siguiente RETR (en bytes) */
        if (strcmp(tok, "rest") == 0 || strcmp(tok, "REST") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: REST <offset>\n"); continue; }
            long off = atol(arg);
            if (off < 0) { printf("offset invalido\n"); continue; }

            /* Asegurar modo binario antes de REST (servidores suelen rechazar REST en ASCII) */
            char restres[LINELEN];
            if (sendCmd(s, "TYPE I", restres, sizeof(restres)) < 0) {
                fprintf(stderr, "Error estableciendo TYPE I\n");
                continue;
            }

            /* enviar REST al servidor para validar si lo acepta */
            char cmdrest[256];
            snprintf(cmdrest, sizeof(cmdrest), "REST %ld", off);
            int code = sendCmd(s, cmdrest, restres, sizeof(restres));
            if (code < 0) { fprintf(stderr, "REST: error de comunicación\n"); continue; }
            /* 3xx es OK (350 normalmente) */
            if (code >= 300 && code < 400) {
                restart_offset = off;
                printf("REST guardado: %ld (se aplicará al siguiente RETR)\n", restart_offset);
            } else {
                printf("REST no aceptado por servidor: %s\n", restres);
            }
            continue;
        }


        if (strcmp(tok, "cd") == 0) {
            char *arg = strtok(NULL, " ");
            if (!arg) { printf("Uso: cd <dir>\n"); continue; }
            snprintf(cmd, sizeof(cmd), "CWD %s", arg);
            sendCmd(s, cmd, res, sizeof(res));
            continue;
        }

        if (strcmp(tok, "quit") == 0) {
            sendCmd(s, "QUIT", res, sizeof(res));
            close(s);
            break;
        }

        printf("%s: comando no implementado\n", tok);
    }

    return 0;
}


