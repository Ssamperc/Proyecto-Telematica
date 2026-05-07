/*
 * PIBL - Proxy Inverso + Balanceador de Carga
 * - Round Robin
 * - Caché en disco con TTL
 * - Log a stdout y archivo
 * - Concurrencia: Thread-based
 * - HTTP/1.1
 *
 * Uso: ./pibl <config_file> [cache_ttl_seconds]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <signal.h>

#define BUFFER_SIZE   65536
#define MAX_PATH      1024
#define MAX_BACKENDS  32
#define MAX_LINE      512
#define BACKLOG       256
#define CACHE_DIR     "./cache"

/* ─── Estructuras ─── */
typedef struct {
    char host[256];
    int  port;
} Backend;

typedef struct {
    int     listen_port;
    Backend backends[MAX_BACKENDS];
    int     backend_count;
    long    cache_ttl;
    char    log_file[MAX_PATH];
} Config;

typedef struct {
    int  client_fd;
    char client_ip[INET_ADDRSTRLEN];
} ProxyCtx;

typedef struct {
    char method[16];
    char uri[MAX_PATH];
    char version[16];
} RequestLine;

/* ─── Globales ─── */
static Config          g_cfg;
static int             g_rr_index = 0;
static pthread_mutex_t g_rr_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE           *g_log_fp    = NULL;

/* ═══════════════════════════════════
 * LOGGING
 * ═══════════════════════════════════ */
static void log_entry(const char *client_ip, const char *method,
                      const char *uri,        const char *backend,
                      int status, long bytes,  const char *source)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", t);

    char line[1024];
    snprintf(line, sizeof(line),
        "[%s] %s \"%s %s\" -> %s | HTTP %d | %ld bytes | [%s]\n",
        ts, client_ip, method, uri,
        backend ? backend : "-", status, bytes, source);

    pthread_mutex_lock(&g_log_mutex);
    fputs(line, stdout); fflush(stdout);
    if (g_log_fp) { fputs(line, g_log_fp); fflush(g_log_fp); }
    pthread_mutex_unlock(&g_log_mutex);
}

/* ═══════════════════════════════════
 * CONFIG PARSER
 * ═══════════════════════════════════ */
static int parse_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror("fopen config"); return -1; }

    g_cfg.listen_port   = 8080;
    g_cfg.backend_count = 0;
    g_cfg.cache_ttl     = 300;
    snprintf(g_cfg.log_file, sizeof(g_cfg.log_file), "pibl.log");

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char *cr = strchr(line, '\r'); if (cr) *cr = '\0';
        char *cm = strchr(line, '#');  if (cm) *cm = '\0';
        if (!line[0]) continue;

        char key[128], val[MAX_LINE];
        if (sscanf(line, " %127[^= ] = %511s", key, val) != 2) continue;

        if (!strcmp(key, "port")) {
            g_cfg.listen_port = atoi(val);
        } else if (!strcmp(key, "log_file")) {
            strncpy(g_cfg.log_file, val, sizeof(g_cfg.log_file)-1);
        } else if (!strcmp(key, "backend")) {
            if (g_cfg.backend_count < MAX_BACKENDS) {
                char host[256]; int port;
                if (sscanf(val, "%255[^:]:%d", host, &port) == 2) {
                    strncpy(g_cfg.backends[g_cfg.backend_count].host, host, 255);
                    g_cfg.backends[g_cfg.backend_count].port = port;
                    g_cfg.backend_count++;
                }
            }
        }
    }
    fclose(f);

    if (g_cfg.backend_count == 0) {
        fprintf(stderr, "Error: no hay backends en la config.\n");
        return -1;
    }
    return 0;
}

/* ═══════════════════════════════════
 * ROUND ROBIN
 * ═══════════════════════════════════ */
static Backend *next_backend(void)
{
    pthread_mutex_lock(&g_rr_mutex);
    int idx = g_rr_index % g_cfg.backend_count;
    g_rr_index++;
    pthread_mutex_unlock(&g_rr_mutex);
    return &g_cfg.backends[idx];
}

/* ═══════════════════════════════════
 * CACHÉ
 * ═══════════════════════════════════ */
static void uri_to_cache_path(const char *uri, char *out, size_t out_size)
{
    char safe[MAX_PATH];
    int j = 0;
    for (int i = 0; uri[i] && j < (int)sizeof(safe)-1; i++) {
        char c = uri[i];
        safe[j++] = (c=='/'||c=='?'||c=='&'||c=='='||c==':'||c==' ') ? '_' : c;
    }
    safe[j] = '\0';
    if (j == 0 || !strcmp(safe, "_")) snprintf(safe, sizeof(safe), "root_index");
    snprintf(out, out_size, "%s/%s", CACHE_DIR, safe);
}

static int cache_valid(const char *cache_path)
{
    struct stat st;
    if (stat(cache_path, &st) < 0) return 0;
    return (time(NULL) - st.st_mtime) < g_cfg.cache_ttl;
}

static long serve_from_cache(int client_fd, const char *cache_path)
{
    int fd = open(cache_path, O_RDONLY);
    if (fd < 0) return -1;
    struct stat st;
    fstat(fd, &st);
    off_t offset = 0;
    long  sent   = 0;
    ssize_t n;
    while (sent < (long)st.st_size) {
        n = sendfile(client_fd, fd, &offset, st.st_size - sent);
        if (n <= 0) break;
        sent += n;
    }
    close(fd);
    return sent;
}

/* ═══════════════════════════════════
 * CONEXIÓN AL BACKEND
 * ═══════════════════════════════════ */
static int connect_to_backend(const Backend *b)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", b->port);

    if (getaddrinfo(b->host, port_str, &hints, &res) != 0) return -1;

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) { freeaddrinfo(res); return -1; }

    struct timeval tv = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
        close(sock); freeaddrinfo(res); return -1;
    }
    freeaddrinfo(res);
    return sock;
}

/* ═══════════════════════════════════
 * PARSE REQUEST LINE
 * ═══════════════════════════════════ */
static int parse_request_line(const char *raw, RequestLine *rl)
{
    return (sscanf(raw, "%15s %1023s %15s",
                   rl->method, rl->uri, rl->version) == 3) ? 0 : -1;
}

/* ═══════════════════════════════════
 * RESPUESTAS DE ERROR
 * ═══════════════════════════════════ */
static void send_502(int fd)
{
    const char *r =
        "HTTP/1.1 502 Bad Gateway\r\n"
        "Server: PIBL/1.0\r\nContent-Type: text/html\r\n"
        "Content-Length: 38\r\nConnection: close\r\n\r\n"
        "<html><body>502 Bad Gateway</body></html>";
    write(fd, r, strlen(r));
}

static void send_400(int fd)
{
    const char *r =
        "HTTP/1.1 400 Bad Request\r\n"
        "Server: PIBL/1.0\r\nContent-Type: text/html\r\n"
        "Content-Length: 39\r\nConnection: close\r\n\r\n"
        "<html><body>400 Bad Request</body></html>";
    write(fd, r, strlen(r));
}

/* ═══════════════════════════════════
 * PARSE HTTP STATUS de la respuesta
 * ═══════════════════════════════════ */
static int parse_response_status(const char *buf)
{
    int status = 0;
    sscanf(buf, "HTTP/%*s %d", &status);
    return status;
}

/* ═══════════════════════════════════
 * HANDLER POR THREAD
 * ═══════════════════════════════════ */
static void *proxy_handler(void *arg)
{
    ProxyCtx *ctx = (ProxyCtx *)arg;
    int  client_fd = ctx->client_fd;
    char client_ip[INET_ADDRSTRLEN];
    strncpy(client_ip, ctx->client_ip, sizeof(client_ip)-1);
    free(ctx);

    pthread_detach(pthread_self());

    /* 1. Leer petición del cliente */
    char *req_buf = malloc(BUFFER_SIZE);
    if (!req_buf) { close(client_fd); return NULL; }

    int total = 0;
    ssize_t n;
    while (total < BUFFER_SIZE - 1) {
        n = read(client_fd, req_buf + total, BUFFER_SIZE - total - 1);
        if (n <= 0) break;
        total += n;
        req_buf[total] = '\0';
        if (strstr(req_buf, "\r\n\r\n")) break;
    }

    if (total == 0) { free(req_buf); close(client_fd); return NULL; }

    req_buf[total] = '\0';

    /* 2. Parsear línea de petición */
    RequestLine rl;
    memset(&rl, 0, sizeof(rl));
    if (parse_request_line(req_buf, &rl) < 0) {
        send_400(client_fd);
        free(req_buf); close(client_fd); return NULL;
    }

    /* 3. Solo cachear GET */
    int cacheable = !strcasecmp(rl.method, "GET");
    char cache_path[MAX_PATH] = {0};
    if (cacheable) {
        uri_to_cache_path(rl.uri, cache_path, sizeof(cache_path));
        if (cache_valid(cache_path)) {
            long bytes = serve_from_cache(client_fd, cache_path);
            if (bytes > 0) {
                log_entry(client_ip, rl.method, rl.uri, "CACHE", 200, bytes, "CACHE-HIT");
                free(req_buf); close(client_fd); return NULL;
            }
        }
    }

    /* 4. Seleccionar backend (Round Robin) */
    Backend *b = next_backend();
    char backend_label[300];
    snprintf(backend_label, sizeof(backend_label), "%s:%d", b->host, b->port);

    /* 5. Conectar al backend */
    int backend_fd = connect_to_backend(b);
    if (backend_fd < 0) {
        log_entry(client_ip, rl.method, rl.uri, backend_label, 502, 0, "BACKEND-ERR");
        send_502(client_fd);
        free(req_buf); close(client_fd); return NULL;
    }

    /* 6. Reescribir cabecera Host y reenviar */
    /* Construir nueva petición con Host correcto */
    char new_req[BUFFER_SIZE];
    int  new_len = 0;

    /* Reemplazar o añadir Host */
    const char *header_end = strstr(req_buf, "\r\n\r\n");
    if (!header_end) {
        send_502(client_fd);
        free(req_buf); close(backend_fd); close(client_fd); return NULL;
    }

    /* Reconstruir: copiar primera línea, luego cabeceras filtrando Host */
    const char *p = req_buf;
    const char *eol = strstr(p, "\r\n");

    /* Primera línea */
    new_len += snprintf(new_req + new_len, sizeof(new_req) - new_len,
                        "%.*s\r\n", (int)(eol - p), p);
    /* Host correcto */
    new_len += snprintf(new_req + new_len, sizeof(new_req) - new_len,
                        "Host: %s:%d\r\n", b->host, b->port);

    p = eol + 2;
    while (p < header_end) {
        const char *next = strstr(p, "\r\n");
        if (!next) break;
        /* Saltar cabecera Host original */
        if (strncasecmp(p, "Host:", 5) != 0) {
            new_len += snprintf(new_req + new_len, sizeof(new_req) - new_len,
                                "%.*s\r\n", (int)(next - p), p);
        }
        p = next + 2;
    }
    new_len += snprintf(new_req + new_len, sizeof(new_req) - new_len, "\r\n");

    /* Body (si hay) */
    const char *body_start = header_end + 4;
    int body_len = total - (int)(body_start - req_buf);
    if (body_len > 0 && new_len + body_len < BUFFER_SIZE) {
        memcpy(new_req + new_len, body_start, body_len);
        new_len += body_len;
    }

    /* Enviar al backend */
    int sent_to_backend = 0;
    while (sent_to_backend < new_len) {
        n = write(backend_fd, new_req + sent_to_backend, new_len - sent_to_backend);
        if (n <= 0) break;
        sent_to_backend += n;
    }

    /* 7. Leer respuesta del backend y reenviar al cliente + guardar caché */
    FILE *cache_fp = NULL;
    if (cacheable && cache_path[0]) {
        /* Asegurar directorio caché */
        mkdir(CACHE_DIR, 0755);
        cache_fp = fopen(cache_path, "wb");
    }

    char *resp_buf = malloc(BUFFER_SIZE);
    if (!resp_buf) {
        if (cache_fp) fclose(cache_fp);
        free(req_buf); close(backend_fd); close(client_fd); return NULL;
    }

    long total_resp = 0;
    int  status_code = 0;
    int  first_chunk = 1;

    while ((n = read(backend_fd, resp_buf, BUFFER_SIZE)) > 0) {
        if (first_chunk) {
            resp_buf[n < BUFFER_SIZE ? n : BUFFER_SIZE-1] = '\0';
            status_code = parse_response_status(resp_buf);
            first_chunk = 0;
        }
        /* Enviar al cliente */
        int w = 0;
        while (w < n) {
            ssize_t wn = write(client_fd, resp_buf + w, n - w);
            if (wn <= 0) goto done_reading;
            w += wn;
        }
        /* Guardar en caché */
        if (cache_fp) fwrite(resp_buf, 1, n, cache_fp);
        total_resp += n;
    }

done_reading:
    if (cache_fp) fclose(cache_fp);

    /* Si no fue 200, borrar caché parcial */
    if (cacheable && status_code != 200 && cache_path[0])
        unlink(cache_path);

    log_entry(client_ip, rl.method, rl.uri, backend_label,
              status_code ? status_code : 502, total_resp, "BACKEND");

    free(resp_buf);
    free(req_buf);
    close(backend_fd);
    close(client_fd);
    return NULL;
}

/* ═══════════════════════════════════
 * MAIN
 * ═══════════════════════════════════ */
int main(int argc, char *argv[])
{
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Uso: %s <config_file> [cache_ttl_seconds]\n", argv[0]);
        return 1;
    }

    if (parse_config(argv[1]) < 0) return 1;

    if (argc == 3) g_cfg.cache_ttl = atol(argv[2]);

    g_log_fp = fopen(g_cfg.log_file, "a");
    if (!g_log_fp) perror("Advertencia: no se pudo abrir log file");

    signal(SIGPIPE, SIG_IGN);
    mkdir(CACHE_DIR, 0755);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(g_cfg.listen_port)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    printf("[PIBL] Escuchando en puerto %d | Backends: %d | TTL caché: %lds\n",
           g_cfg.listen_port, g_cfg.backend_count, g_cfg.cache_ttl);
    for (int i = 0; i < g_cfg.backend_count; i++)
        printf("  [Backend %d] %s:%d\n", i+1,
               g_cfg.backends[i].host, g_cfg.backends[i].port);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        ProxyCtx *ctx = malloc(sizeof(ProxyCtx));
        if (!ctx) { close(client_fd); continue; }
        ctx->client_fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, ctx->client_ip, INET_ADDRSTRLEN);

        pthread_t tid;
        if (pthread_create(&tid, NULL, proxy_handler, ctx) != 0) {
            perror("pthread_create"); free(ctx); close(client_fd);
        }
    }

    if (g_log_fp) fclose(g_log_fp);
    return 0;
}
