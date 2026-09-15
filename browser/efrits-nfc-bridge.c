#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#define close_socket closesocket
#define popen_cmd _popen
#define pclose_cmd _pclose
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#define close_socket close
#define popen_cmd popen
#define pclose_cmd pclose
#endif

#ifdef _WIN32
typedef SOCKET socket_handle;
#define INVALID_SOCKET_HANDLE INVALID_SOCKET
#else
typedef int socket_handle;
#define INVALID_SOCKET_HANDLE (-1)
#endif

#define BRIDGE_PORT 38421
#define MAX_REQUEST 16384
#define MAX_OUTPUT 16384
#define NFC_SIZE 32
#define ALLOWED_ORIGIN "https://intra.efrits.fr"
#define BRIDGE_VERSION 5
#define PROCESS_RETRIES 2
#define PROCESS_RETRY_MS 350

typedef struct s_http_request
{
    char method[12];
    char path[256];
    char origin[512];
    char host[512];
    size_t content_length;
    char *body;
} http_request;

static int is_allowed_origin(const char *origin)
{
    return origin && strcmp(origin, ALLOWED_ORIGIN) == 0;
}

static int is_allowed_host(const char *host)
{
    if (!host)
        return 0;
    return strcmp(host, "127.0.0.1:38421") == 0
        || strcmp(host, "localhost:38421") == 0;
}

static char *json_result(int ok, int exit_code, const char *output)
{
    static const char hexdigits[] = "0123456789abcdef";
    const unsigned char *p = (const unsigned char *)(output ? output : "");
    size_t input_len = strlen((const char *)p);
    size_t capacity;
    char *buffer;
    char *out;
    int n;

    /*
     * Do not use tmpfile() here.  The bridge is normally launched from the
     * Windows user startup environment; a temporary C stream is not guaranteed
     * to be available there.  The old implementation returned NULL when
     * tmpfile() failed, which made the HTTP layer fall back to {"ok":false} and
     * discarded both the real exit code and efrits-nfc's output.
     *
     * Six bytes per input byte is the JSON worst case (\\u00XX), with ample
     * room for the fixed object syntax and integer fields.
     */
    if (input_len > (((size_t)-1) - 256) / 6)
        return NULL;
    capacity = input_len * 6 + 256;
    buffer = (char *)malloc(capacity);
    if (!buffer)
        return NULL;

    n = snprintf(buffer, capacity,
        "{\"ok\":%s,\"bridge_version\":%d,\"exit_code\":%d,\"output\":",
        ok ? "true" : "false", BRIDGE_VERSION, exit_code);
    if (n < 0 || (size_t)n >= capacity)
    {
        free(buffer);
        return NULL;
    }
    out = buffer + (size_t)n;
    *out++ = '"';
    while (*p)
    {
        switch (*p)
        {
            case '\\': *out++ = '\\'; *out++ = '\\'; break;
            case '"':  *out++ = '\\'; *out++ = '"';  break;
            case '\b': *out++ = '\\'; *out++ = 'b';  break;
            case '\f': *out++ = '\\'; *out++ = 'f';  break;
            case '\n': *out++ = '\\'; *out++ = 'n';  break;
            case '\r': *out++ = '\\'; *out++ = 'r';  break;
            case '\t': *out++ = '\\'; *out++ = 't';  break;
            default:
                if (*p < 0x20)
                {
                    *out++ = '\\';
                    *out++ = 'u';
                    *out++ = '0';
                    *out++ = '0';
                    *out++ = hexdigits[(*p >> 4) & 0x0f];
                    *out++ = hexdigits[*p & 0x0f];
                }
                else
                    *out++ = (char)*p;
        }
        ++p;
    }
    *out++ = '"';
    *out++ = '}';
    *out = 0;
    return buffer;
}

static char *json_error(const char *message)
{
    return json_result(0, -1, message);
}

static int send_all(socket_handle fd, const char *buf, size_t len)
{
    while (len > 0)
    {
#ifdef _WIN32
        int n = send(fd, buf, (int)len, 0);
#else
        ssize_t n = send(fd, buf, len, 0);
#endif
        if (n <= 0)
            return 0;
        buf += n;
        len -= (size_t)n;
    }
    return 1;
}

static void send_response(socket_handle fd, int status, const char *status_text,
                          const char *origin, const char *content_type,
                          const char *body)
{
    char header[2048];
    size_t body_len = body ? strlen(body) : 0;
    int n;

    n = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: %s\r\n"
        "Vary: Origin\r\n"
        "Access-Control-Allow-Private-Network: true\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "\r\n",
        status, status_text, content_type ? content_type : "application/json; charset=utf-8",
        body_len, origin ? origin : "null");
    if (n <= 0 || (size_t)n >= sizeof(header))
        return;
    send_all(fd, header, (size_t)n);
    if (body_len)
        send_all(fd, body, body_len);
}

static char *trim_left(char *s)
{
    while (*s && isspace((unsigned char)*s))
        ++s;
    return s;
}

static void trim_right(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
}

static int ascii_equal_ci_n(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; ++i)
    {
        if (!a[i] || !b[i])
            return a[i] == b[i];
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    }
    return 1;
}

static int ascii_equal_ci(const char *a, const char *b)
{
    size_t na = strlen(a);
    size_t nb = strlen(b);
    return na == nb && ascii_equal_ci_n(a, b, na);
}

static size_t request_content_length(const char *buffer, const char *headers_end)
{
    const char *p = strstr(buffer, "\r\n");

    if (!p || p >= headers_end)
        return 0;
    p += 2;
    while (p < headers_end)
    {
        const char *line_end = strstr(p, "\r\n");
        const char *colon;
        size_t name_len;
        const char *value;

        if (!line_end || line_end > headers_end)
            line_end = headers_end;
        colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon)
        {
            name_len = (size_t)(colon - p);
            if (name_len == strlen("Content-Length")
                && ascii_equal_ci_n(p, "Content-Length", name_len))
            {
                value = colon + 1;
                while (value < line_end && isspace((unsigned char)*value))
                    ++value;
                return (size_t)strtoul(value, NULL, 10);
            }
        }
        if (line_end == headers_end)
            break;
        p = line_end + 2;
    }
    return 0;
}

static int parse_request(char *buffer, size_t len, http_request *req)
{
    char *headers_end;
    char *line_end;
    char *p;

    memset(req, 0, sizeof(*req));
    headers_end = strstr(buffer, "\r\n\r\n");
    if (!headers_end)
        return 0;
    req->body = headers_end + 4;

    line_end = strstr(buffer, "\r\n");
    if (!line_end)
        return 0;
    *line_end = 0;
    if (sscanf(buffer, "%11s %255s", req->method, req->path) != 2)
        return 0;
    *line_end = '\r';

    p = line_end + 2;
    while (p < headers_end)
    {
        char *next = strstr(p, "\r\n");
        char *colon;
        size_t line_len;
        size_t name_len;
        char *value;

        if (!next || next > headers_end)
            next = headers_end;
        line_len = (size_t)(next - p);
        colon = memchr(p, ':', line_len);
        if (colon)
        {
            char saved_colon = *colon;
            char saved_end = *next;
            *colon = 0;
            *next = 0;
            trim_right(p);
            value = trim_left(colon + 1);
            trim_right(value);
            name_len = strlen(p);
            if (ascii_equal_ci(p, "Origin"))
                snprintf(req->origin, sizeof(req->origin), "%s", value);
            else if (ascii_equal_ci(p, "Host"))
                snprintf(req->host, sizeof(req->host), "%s", value);
            else if (ascii_equal_ci(p, "Content-Length"))
                req->content_length = (size_t)strtoul(value, NULL, 10);
            (void)name_len;
            *colon = saved_colon;
            *next = saved_end;
        }
        if (next == headers_end)
            break;
        p = next + 2;
    }

    if (req->content_length > MAX_REQUEST)
        return 0;
    if ((size_t)(buffer + len - req->body) < req->content_length)
        return 0;
    req->body[req->content_length] = 0;
    return 1;
}

static int base64_value(int c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static int base64_decode(const char *src, unsigned char *dst, size_t cap, size_t *out_len)
{
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;

    while (*src)
    {
        int v;
        if (*src == '=')
            break;
        v = base64_value((unsigned char)*src++);
        if (v < 0)
            return 0;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            if (n >= cap)
                return 0;
            dst[n++] = (unsigned char)((acc >> bits) & 0xff);
        }
    }
    *out_len = n;
    return 1;
}

static int extract_json_payload(const char *body, char *payload, size_t payload_size)
{
    const char *key = strstr(body ? body : "", "\"payload\"");
    const char *p;
    const char *end;
    size_t n;

    if (!key)
        return 0;
    p = strchr(key + 9, ':');
    if (!p)
        return 0;
    ++p;
    while (*p && isspace((unsigned char)*p))
        ++p;
    if (*p != '"')
        return 0;
    ++p;
    end = strchr(p, '"');
    if (!end)
        return 0;
    n = (size_t)(end - p);
    if (n == 0 || n >= payload_size)
        return 0;
    memcpy(payload, p, n);
    payload[n] = 0;
    return 1;
}

static int file_exists(const char *path)
{
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
    return access(path, X_OK) == 0;
#endif
}

static int path_with_filename(char *out, size_t out_size, const char *dir, const char *name)
{
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    if (dir_len + name_len + 1 > out_size)
        return 0;
    memcpy(out, dir, dir_len);
    memcpy(out + dir_len, name, name_len + 1);
    return 1;
}

static void find_cli(char *out, size_t out_size)
{
    const char *env = getenv("EFRITS_NFC_BIN");
    if (env && *env)
    {
        snprintf(out, out_size, "%s", env);
        return;
    }
#ifdef _WIN32
    {
        char self[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, self, sizeof(self));
        if (n > 0 && n < sizeof(self))
        {
            char *slash = strrchr(self, '\\');
            if (slash)
            {
                *(slash + 1) = 0;
                if (path_with_filename(out, out_size, self, "efrits-nfc.exe") && file_exists(out))
                    return;
            }
        }
    }
    snprintf(out, out_size, "efrits-nfc.exe");
#else
    {
        char self[4096];
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0)
        {
            char *slash;
            self[n] = 0;
            slash = strrchr(self, '/');
            if (slash)
            {
                *(slash + 1) = 0;
                if (path_with_filename(out, out_size, self, "efrits-nfc") && file_exists(out))
                    return;
            }
        }
    }
    snprintf(out, out_size, "efrits-nfc");
#endif
}

static int make_temp_nfc(const unsigned char *data, size_t len, char *path, size_t path_size)
{
#ifdef _WIN32
    char dir[MAX_PATH];
    unsigned long pid = GetCurrentProcessId();
    unsigned long tick = GetTickCount();
    int i;

    if (!GetTempPathA(sizeof(dir), dir))
        return 0;
    for (i = 0; i < 100; ++i)
    {
        HANDLE h;
        DWORD written = 0;
        snprintf(path, path_size, "%sefrits-nfc-%lu-%lu-%d.nfc", dir, pid, tick, i);
        h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (!WriteFile(h, data, (DWORD)len, &written, NULL) || written != (DWORD)len)
        {
            CloseHandle(h);
            DeleteFileA(path);
            return 0;
        }
        CloseHandle(h);
        return 1;
    }
    return 0;
#else
    int fd;
    ssize_t written;
    char templ[] = "/tmp/efrits-nfc-XXXXXX";
    char final_path[4096];

    fd = mkstemp(templ);
    if (fd < 0)
        return 0;
    if (snprintf(final_path, sizeof(final_path), "%s.nfc", templ) >= (int)sizeof(final_path))
    {
        close(fd);
        unlink(templ);
        return 0;
    }
    written = write(fd, data, len);
    close(fd);
    if (written != (ssize_t)len || rename(templ, final_path) != 0)
    {
        unlink(templ);
        unlink(final_path);
        return 0;
    }
    snprintf(path, path_size, "%s", final_path);
    return 1;
#endif
}

static void remove_temp(const char *path)
{
#ifdef _WIN32
    DeleteFileA(path);
#else
    unlink(path);
#endif
}

#ifdef _WIN32
static int run_cli_windows(const char *cli, const char *nfc_path, char *output, size_t output_size)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE pipe_read = NULL;
    HANDLE pipe_write = NULL;
    HANDLE nul_input = INVALID_HANDLE_VALUE;
    char command[9000];
    DWORD exit_code = 1;
    size_t used = 0;
    BOOL created;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&pipe_read, &pipe_write, &sa, 0))
    {
        snprintf(output, output_size, "Impossible de creer le canal de sortie pour efrits-nfc (Win32=%lu).",
                 (unsigned long)GetLastError());
        return -1;
    }
    if (!SetHandleInformation(pipe_read, HANDLE_FLAG_INHERIT, 0))
    {
        snprintf(output, output_size, "Impossible de configurer le canal de sortie pour efrits-nfc (Win32=%lu).",
                 (unsigned long)GetLastError());
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return -1;
    }

    nul_input = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (nul_input == INVALID_HANDLE_VALUE)
    {
        snprintf(output, output_size, "Impossible d'ouvrir NUL pour efrits-nfc (Win32=%lu).",
                 (unsigned long)GetLastError());
        CloseHandle(pipe_read);
        CloseHandle(pipe_write);
        return -1;
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nul_input;
    si.hStdOutput = pipe_write;
    si.hStdError = pipe_write;

    if (nfc_path)
        snprintf(command, sizeof(command), "\"%s\" \"%s\"", cli, nfc_path);
    else
        snprintf(command, sizeof(command), "\"%s\"", cli);

    created = CreateProcessA(
        cli,
        command,
        NULL,
        NULL,
        TRUE,
        CREATE_NO_WINDOW,
        NULL,
        NULL,
        &si,
        &pi
    );

    CloseHandle(pipe_write);
    pipe_write = NULL;
    CloseHandle(nul_input);
    nul_input = INVALID_HANDLE_VALUE;

    if (!created)
    {
        snprintf(output, output_size, "Impossible de lancer %s (Win32=%lu).",
                 cli, (unsigned long)GetLastError());
        CloseHandle(pipe_read);
        return -1;
    }

    while (used + 1 < output_size)
    {
        DWORD got = 0;
        DWORD room = (DWORD)(output_size - used - 1);

        if (!ReadFile(pipe_read, output + used, room, &got, NULL) || got == 0)
            break;
        used += (size_t)got;
    }
    output[used] = 0;
    CloseHandle(pipe_read);

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code))
        exit_code = 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0)
    {
        size_t i;
        int only_space = 1;

        for (i = 0; i < used; ++i)
            if (!isspace((unsigned char)output[i]))
            {
                only_space = 0;
                break;
            }
        if (used == 0 || only_space)
            snprintf(output, output_size,
                     "efrits-nfc a quitte avec le code %lu sans produire de diagnostic. Binaire: %s",
                     (unsigned long)exit_code, cli);
    }
    return (int)exit_code;
}
#endif

static int run_cli(const char *nfc_path, char *output, size_t output_size)
{
    char cli[4096];

    find_cli(cli, sizeof(cli));
#ifdef _WIN32
    return run_cli_windows(cli, nfc_path, output, output_size);
#else
    char command[9000];
    FILE *pipe;
    size_t used = 0;
    int status;

    if (nfc_path)
        snprintf(command, sizeof(command), "\"%s\" \"%s\" 2>&1", cli, nfc_path);
    else
        snprintf(command, sizeof(command), "\"%s\" 2>&1", cli);

    pipe = popen_cmd(command, "r");
    if (!pipe)
    {
        snprintf(output, output_size, "Impossible de lancer efrits-nfc.");
        return -1;
    }
    while (used + 1 < output_size)
    {
        size_t n = fread(output + used, 1, output_size - used - 1, pipe);
        used += n;
        if (n == 0)
            break;
    }
    output[used] = 0;
    status = pclose_cmd(pipe);
    if (status == -1)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return 1;
#endif
}

static void bridge_sleep_ms(unsigned long milliseconds)
{
#ifdef _WIN32
    Sleep((DWORD)milliseconds);
#else
    struct timespec req;
    struct timespec rem;

    req.tv_sec = (time_t)(milliseconds / 1000UL);
    req.tv_nsec = (long)((milliseconds % 1000UL) * 1000000UL);
    while (nanosleep(&req, &rem) != 0 && errno == EINTR)
        req = rem;
#endif
}

static void append_diagnostic(char *output, size_t output_size, size_t *used,
                              int attempt, int code, const char *attempt_output)
{
    int n;
    const char *text = attempt_output && *attempt_output
        ? attempt_output : "(aucune sortie produite par efrits-nfc)\n";

    if (*used >= output_size - 1)
        return;
    n = snprintf(output + *used, output_size - *used,
                 "%sTentative processus %d/%d — code de sortie %d\n%s",
                 *used ? "\n" : "", attempt, PROCESS_RETRIES, code, text);
    if (n < 0)
        return;
    if ((size_t)n >= output_size - *used)
        *used = output_size - 1;
    else
        *used += (size_t)n;
}

/*
 * A fresh child process is intentional here.  The ACR1552U can leave the
 * PC/SC context of one efrits-nfc invocation in a state where the write has
 * actually reached the tag but the verification fails.  Re-running inside
 * the same process/context was not sufficient on the Windows station; a
 * subsequent standalone CLI invocation was.  The bridge therefore reproduces
 * that exact recovery: terminate the first CLI, wait briefly, then spawn a
 * completely fresh efrits-nfc process.
 */
static int run_cli_with_process_retry(const char *nfc_path,
                                      char *output, size_t output_size)
{
    char attempt_output[MAX_OUTPUT];
    size_t used = 0;
    int attempt;
    int code = 1;

    output[0] = 0;
    for (attempt = 1; attempt <= PROCESS_RETRIES; ++attempt)
    {
        attempt_output[0] = 0;
        code = run_cli(nfc_path, attempt_output, sizeof(attempt_output));
        append_diagnostic(output, output_size, &used, attempt, code, attempt_output);
        output[used < output_size ? used : output_size - 1] = 0;
        if (code == 0)
            return 0;
        if (attempt < PROCESS_RETRIES)
            bridge_sleep_ms(PROCESS_RETRY_MS);
    }
    return code;
}

static char *handle_read(void)
{
    char output[MAX_OUTPUT];
    int code = run_cli_with_process_retry(NULL, output, sizeof(output));
    return json_result(code == 0, code, output);
}

static char *handle_write(const char *body)
{
    char payload_b64[256];
    unsigned char payload[NFC_SIZE + 1];
    size_t payload_len = 0;
    char temp_path[4096];
    char output[MAX_OUTPUT];
    int code;

    if (!extract_json_payload(body, payload_b64, sizeof(payload_b64)))
        return json_error("Payload NFC manquant.");
    if (!base64_decode(payload_b64, payload, sizeof(payload), &payload_len) || payload_len != NFC_SIZE)
        return json_error("Le payload NFC doit faire exactement 32 octets.");
    if (!make_temp_nfc(payload, payload_len, temp_path, sizeof(temp_path)))
        return json_error("Impossible de créer le fichier NFC temporaire.");

    code = run_cli_with_process_retry(temp_path, output, sizeof(output));
    remove_temp(temp_path);
    return json_result(code == 0, code, output);
}

static void handle_client(socket_handle fd)
{
    char buffer[MAX_REQUEST + 4096 + 1];
    size_t used = 0;
    http_request req;
    char *response;

    while (used < sizeof(buffer) - 1)
    {
#ifdef _WIN32
        int n = recv(fd, buffer + used, (int)(sizeof(buffer) - 1 - used), 0);
#else
        ssize_t n = recv(fd, buffer + used, sizeof(buffer) - 1 - used, 0);
#endif
        if (n <= 0)
            break;
        used += (size_t)n;
        buffer[used] = 0;
        if (strstr(buffer, "\r\n\r\n"))
        {
            char *head_end = strstr(buffer, "\r\n\r\n");
            size_t body_have = used - (size_t)((head_end + 4) - buffer);
            size_t wanted = request_content_length(buffer, head_end);
            if (body_have >= wanted)
                break;
        }
    }
    buffer[used] = 0;

    if (!parse_request(buffer, used, &req))
    {
        send_response(fd, 400, "Bad Request", "null", "application/json; charset=utf-8", "{\"ok\":false,\"bridge_version\":5,\"exit_code\":-1,\"output\":\"Requête invalide.\"}");
        return;
    }
    if (!is_allowed_host(req.host))
    {
        send_response(fd, 403, "Forbidden", "null", "application/json; charset=utf-8", "{\"ok\":false,\"bridge_version\":5,\"exit_code\":-1,\"output\":\"Host refusé.\"}");
        return;
    }
    if (!is_allowed_origin(req.origin))
    {
        send_response(fd, 403, "Forbidden", "null", "application/json; charset=utf-8", "{\"ok\":false,\"bridge_version\":5,\"exit_code\":-1,\"output\":\"Origine refusée.\"}");
        return;
    }

    if (strcmp(req.method, "OPTIONS") == 0)
    {
        send_response(fd, 204, "No Content", req.origin, "text/plain", "");
        return;
    }
    if (strcmp(req.path, "/v1/ping") == 0 && strcmp(req.method, "GET") == 0)
    {
        send_response(fd, 200, "OK", req.origin, "application/json; charset=utf-8",
                      "{\"ok\":true,\"version\":5,\"tool\":\"efrits-nfc\"}");
        return;
    }
    if (strcmp(req.path, "/v1/read") == 0 && strcmp(req.method, "POST") == 0)
        response = handle_read();
    else if (strcmp(req.path, "/v1/write") == 0 && strcmp(req.method, "POST") == 0)
        response = handle_write(req.body);
    else
    {
        send_response(fd, 404, "Not Found", req.origin, "application/json; charset=utf-8", "{\"ok\":false,\"bridge_version\":5,\"exit_code\":-1,\"output\":\"Action inconnue.\"}");
        return;
    }

    if (!response)
        response = json_error("Erreur interne du pont NFC.");
    send_response(fd, 200, "OK", req.origin, "application/json; charset=utf-8", response ? response : "{\"ok\":false,\"bridge_version\":5,\"exit_code\":-1,\"output\":\"Erreur interne du pont NFC.\"}");
    free(response);
}

int main(void)
{
#ifdef _WIN32
    WSADATA wsa;
#endif
    socket_handle server;
    struct sockaddr_in addr;
    int yes = 1;

#ifdef _WIN32
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        return 1;
#endif
    server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET_HANDLE)
    {
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(BRIDGE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(server, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 4) != 0)
    {
        close_socket(server);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    for (;;)
    {
        socket_handle client = accept(server, NULL, NULL);
        if (client == INVALID_SOCKET_HANDLE)
            continue;
        handle_client(client);
        close_socket(client);
    }
}
