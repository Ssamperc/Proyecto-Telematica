/*
 * TWS - Telematics Web Server
 * HTTP/1.1 - Métodos: GET, HEAD, POST
 * Concurrencia: Thread-based
 * Uso: ./tws <PORT> <LogFile> <DocumentRootFolder>
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
#include <time.h>
#include <signal.h>

#define BUFFER_SIZE      8192
#define MAX_PATH         1024
#define MAX_HEADER       4096
#define BACKLOG          128

/* ───── Globals ───── */
static char  g_docroot[MAX_PATH];
static char  g_logfile[MAX_PATH];
static FILE *g_log_fp  = NULL;
static pthread_mutex_t g_log_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ───── Logging ───── */
static void log_write(const char *client_ip, const char *method,
                      const char *uri, int status, long bytes)
{
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char tsbuf[64];
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm_info);

    char line[512];
    snprintf(line, sizeof(line),
             "[%s] %s \"%s %s\" %d %ld\n",
             tsbuf, client_ip, method, uri, status, bytes);

    pthread_mutex_lock(&g_log_mutex);
    fputs(line, stdout);
    fflush(stdout);
    if (g_log_fp) {
        fputs(line, g_log_fp);
        fflush(g_log_fp);
    }
    pthread_mutex_unlock(&g_log_mutex);
}

/* ───── MIME types ───── */
static const char *mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (!strcasecmp(ext, ".html") || !strcasecmp(ext, ".htm")) return "text/html";
    if (!strcasecmp(ext, ".css"))  return "text/css";
    if (!strcasecmp(ext, ".js"))   return "application/javascript";
    if (!strcasecmp(ext, ".json")) return "application/json";
    if (!strcasecmp(ext, ".png"))  return "image/png";
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return "image/jpeg";
    if (!strcasecmp(ext, ".gif"))  return "image/gif";
    if (!strcasecmp(ext, ".ico"))  return "image/x-icon";
    if (!strcasecmp(ext, ".svg"))  return "image/svg+xml";
    if (!strcasecmp(ext, ".pdf"))  return "application/pdf";
    if (!strcasecmp(ext, ".txt"))  return "text/plain";
    return "application/octet-stream";
}

/* ───── HTTP date header ───── */
static void http_date(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm *gmt = gmtime(&now);
    strftime(buf, len, "%a, %d %b %Y %H:%M:%S GMT", gmt);
}

/* ───── Send error response ───── */
static void send_error(int fd, int code, const char *msg, const char *client_ip,
                       const char *method, const char *uri)
{
    char date[64];
    http_date(date, sizeof(date));

    char body[256];
    int  blen = snprintf(body, sizeof(body),
                         "<html><body><h1>%d %s</h1></body></html>", code, msg);

    char header[MAX_HEADER];
    int  hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Date: %s\r\n"
        "Server: TWS/1.0\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        code, msg, date, blen);

    write(fd, header, hlen);
    write(fd, body, blen);
    log_write(client_ip, method, uri, code, blen);
}

/* ───── Parse request line ───── */
typedef struct {
    char method[16];
    char uri[MAX_PATH];
    char version[16];
    char host[256];
    long content_length;
    char body[BUFFER_SIZE];
    int  body_len;
} HttpRequest;

static int parse_request(const char *raw, int raw_len, HttpRequest *req)
{
    (void)raw_len;
    memset(req, 0, sizeof(*req));

    /* Parse request line */
    if (sscanf(raw, "%15s %1023s %15s", req->method, req->uri, req->version) != 3)
        return -1;

    /* Parse headers */
    const char *p = strstr(raw, "\r\n");
    if (!p) return -1;
    p += 2;

    while (*p && !(p[0] == '\r' && p[1] == '\n')) {
        char hname[128] = {0}, hval[512] = {0};
        const char *colon = strchr(p, ':');
        const char *eol   = strstr(p, "\r\n");
        if (!colon || !eol || colon > eol) { p = eol + 2; continue; }

        int nlen = colon - p;
        if (nlen >= (int)sizeof(hname)) nlen = sizeof(hname)-1;
        strncpy(hname, p, nlen);

        const char *vstart = colon + 1;
        while (*vstart == ' ') vstart++;
        int vlen = eol - vstart;
        if (vlen >= (int)sizeof(hval)) vlen = sizeof(hval)-1;
        strncpy(hval, vstart, vlen);

        if (!strcasecmp(hname, "Host"))
            strncpy(req->host, hval, sizeof(req->host)-1);
        if (!strcasecmp(hname, "Content-Length"))
            req->content_length = atol(hval);

        p = eol + 2;
    }

    /* Body (POST) */
    if (p[0] == '\r' && p[1] == '\n') {
        p += 2;
        req->body_len = strlen(p);
        if (req->body_len > (int)sizeof(req->body)-1)
            req->body_len = sizeof(req->body)-1;
        memcpy(req->body, p, req->body_len);
    }

    return 0;
}

/* ───── Handle GET / HEAD ───── */
static void handle_get_head(int fd, HttpRequest *req, const char *client_ip,
                             int send_body)
{
    /* Build filesystem path */
    char fspath[MAX_PATH];
    const char *uri = req->uri;

    /* Strip query string */
    char uri_clean[MAX_PATH];
    strncpy(uri_clean, uri, sizeof(uri_clean)-1);
    char *qs = strchr(uri_clean, '?');
    if (qs) *qs = '\0';

    /* Default index */
    if (!strcmp(uri_clean, "/"))
        snprintf(fspath, sizeof(fspath), "%s/index.html", g_docroot);
    else
        snprintf(fspath, sizeof(fspath), "%s%s", g_docroot, uri_clean);

    /* Stat file */
    struct stat st;
    if (stat(fspath, &st) < 0 || S_ISDIR(st.st_mode)) {
        /* Try index.html inside directory */
        if (S_ISDIR(st.st_mode)) {
            char tmp[MAX_PATH];
            snprintf(tmp, sizeof(tmp), "%s/index.html", fspath);
            if (stat(tmp, &st) == 0) {
                strncpy(fspath, tmp, sizeof(fspath)-1);
            } else {
                send_error(fd, 404, "Not Found", client_ip, req->method, req->uri);
                return;
            }
        } else {
            send_error(fd, 404, "Not Found", client_ip, req->method, req->uri);
            return;
        }
    }

    int file_fd = open(fspath, O_RDONLY);
    if (file_fd < 0) {
        send_error(fd, 404, "Not Found", client_ip, req->method, req->uri);
        return;
    }

    char date[64], lmod[64];
    http_date(date, sizeof(date));
    struct tm *gmt = gmtime(&st.st_mtime);
    strftime(lmod, sizeof(lmod), "%a, %d %b %Y %H:%M:%S GMT", gmt);

    char header[MAX_HEADER];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Server: TWS/1.0\r\n"
        "Last-Modified: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        date, lmod, mime_type(fspath), (long)st.st_size);

    write(fd, header, hlen);

    if (send_body) {
        off_t offset = 0;
        sendfile(fd, file_fd, &offset, st.st_size);
    }

    close(file_fd);
    log_write(client_ip, req->method, req->uri, 200, send_body ? st.st_size : 0);
}

/* ───── Handle POST ───── */
static void handle_post(int fd, HttpRequest *req, const char *client_ip)
{
    /* Echo body back as JSON-like response for demonstration */
    char date[64];
    http_date(date, sizeof(date));

    char body[BUFFER_SIZE];
    int blen = snprintf(body, sizeof(body),
        "{\"status\":\"received\",\"uri\":\"%s\",\"bytes\":%d}",
        req->uri, req->body_len);

    char header[MAX_HEADER];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Date: %s\r\n"
        "Server: TWS/1.0\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        date, blen);

    write(fd, header, hlen);
    write(fd, body, blen);
    log_write(client_ip, req->method, req->uri, 200, blen);
}

/* ───── Thread handler ───── */
typedef struct {
    int    fd;
    char   client_ip[INET_ADDRSTRLEN];
} ClientCtx;

static void *handle_client(void *arg)
{
    ClientCtx *ctx = (ClientCtx *)arg;
    int fd = ctx->fd;
    char client_ip[INET_ADDRSTRLEN];
    strncpy(client_ip, ctx->client_ip, sizeof(client_ip)-1);
    free(ctx);

    pthread_detach(pthread_self());

    char buf[BUFFER_SIZE * 4];
    int  total = 0;
    int  n;

    /* Read full request (until \r\n\r\n) */
    while (total < (int)sizeof(buf) - 1) {
        n = read(fd, buf + total, sizeof(buf) - total - 1);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }

    if (total == 0) { close(fd); return NULL; }

    HttpRequest req;
    if (parse_request(buf, total, &req) < 0) {
        send_error(fd, 400, "Bad Request", client_ip, "?", "?");
        close(fd);
        return NULL;
    }

    if (!strcasecmp(req.method, "GET"))
        handle_get_head(fd, &req, client_ip, 1);
    else if (!strcasecmp(req.method, "HEAD"))
        handle_get_head(fd, &req, client_ip, 0);
    else if (!strcasecmp(req.method, "POST"))
        handle_post(fd, &req, client_ip);
    else
        send_error(fd, 400, "Bad Request", client_ip, req.method, req.uri);

    close(fd);
    return NULL;
}

/* ───── Main ───── */
int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Uso: %s <PORT> <LogFile> <DocumentRootFolder>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[1]);
    strncpy(g_logfile,  argv[2], sizeof(g_logfile)-1);
    strncpy(g_docroot,  argv[3], sizeof(g_docroot)-1);

    /* Remove trailing slash from docroot */
    int dlen = strlen(g_docroot);
    if (dlen > 1 && g_docroot[dlen-1] == '/') g_docroot[dlen-1] = '\0';

    g_log_fp = fopen(g_logfile, "a");
    if (!g_log_fp) {
        perror("fopen log");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(port)
    };

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(server_fd, BACKLOG) < 0) {
        perror("listen"); return 1;
    }

    printf("[TWS] Escuchando en puerto %d — docroot: %s\n", port, g_docroot);

    for (;;) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { perror("accept"); continue; }

        ClientCtx *ctx = malloc(sizeof(ClientCtx));
        if (!ctx) { close(client_fd); continue; }
        ctx->fd = client_fd;
        inet_ntop(AF_INET, &client_addr.sin_addr, ctx->client_ip, INET_ADDRSTRLEN);

        pthread_t tid;
        if (pthread_create(&tid, NULL, handle_client, ctx) != 0) {
            perror("pthread_create");
            free(ctx);
            close(client_fd);
        }
    }

    fclose(g_log_fp);
    return 0;
}
