#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <pthread.h>
#include <curl/curl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

//set this to 1 to behave like termbin.com, which treats receive timeout
//like a completed upload
#define ALLOW_TIMEOUT 0

#define DEFAULT_PORT 9999
#define DEFAULT_LISTEN_IP "0.0.0.0"
#define DEFAULT_TIMEOUT 10
#define DEFAULT_FORM_FIELD "file"
#define DEFAULT_FILENAME "file"
#define MAX_RESPONSE_SIZE (100 * 1024 * 1024) // 100 MB
#define MAX_CONNECTIONS 1024

static volatile sig_atomic_t running = 1;
static void handle_signal(int sig) {
    (void)sig;
    running = 0;
}

static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;
static int active_connections = 0;

static void send_all(int sock, const void* buf, size_t len) {
    size_t total = 0;
    while (total < len) {
        ssize_t n = send(sock, (const char*)buf + total, len - total, 0);
        if (n <= 0) {
            if (n == -1) {
                printf("Error send()ing reply: %s\n", strerror(errno));
            }
            return;
        }
        total += n;
    }
}

struct curl_response
{
    char* data;
    size_t size;
};

struct client_connection
{
    const char* upstream;
    const char* form_field;
    const char* filename;
    int socket;
    struct curl_response resp;
    int err;
    struct sockaddr_in client_addr;
};

size_t curl_read_cb(char* buffer, size_t size, size_t nitems, void* user)
{
    struct client_connection* cc = (struct client_connection*)user;
    if (size > 0 && nitems > SIZE_MAX / size) {
        cc->err = E2BIG;
        return CURL_READFUNC_ABORT;
    }
    size_t realsize = size * nitems;
    int ret = recv(cc->socket, buffer, realsize, 0);
    if (ret == -1)
    {
        if (errno == EWOULDBLOCK && ALLOW_TIMEOUT)
        {
            return 0;
        }
        printf("Error recv()ing from client: %s\n", strerror(errno));
        cc->err = errno;
        return CURL_READFUNC_ABORT;
    }
    return ret;
}

size_t curl_write_cb(void* data, size_t size, size_t nitems, void* user)
{
    struct client_connection* cc = (struct client_connection*)user;
    if (size > 0 && nitems > SIZE_MAX / size) {
        cc->err = E2BIG;
        return 0;
    }
    size_t realsize = size * nitems;
    if (realsize == 0)
    {
        return 0;
    }
    if (realsize > SIZE_MAX - cc->resp.size - 1) {
        cc->err = E2BIG;
        return 0;
    }
    if (cc->resp.size + realsize > MAX_RESPONSE_SIZE) {
        cc->err = E2BIG;
        return 0;
    }
    void* new = realloc(cc->resp.data, cc->resp.size + realsize + 1);
    if (new == NULL)
    {
        fprintf(stderr, "realloc() failed: %s\n", strerror(errno));
        cc->err = errno;
        return 0;
    }
    cc->resp.data = new;
    memcpy(cc->resp.data + cc->resp.size, data, realsize);
    cc->resp.size += realsize;
    cc->resp.data[cc->resp.size] = 0;
    return realsize;
}

void* thread(void* arg)
{
    struct client_connection* cc = (struct client_connection*) arg;
    char xff[100];
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &cc->client_addr.sin_addr, addr_str, INET_ADDRSTRLEN);
    snprintf(xff, sizeof(xff), "X-Forwarded-For: %s", addr_str);

    CURL* curl = curl_easy_init();
    if (!curl) {
        cc->err = ENOMEM;
        goto send_error;
    }
    struct curl_slist* headers = curl_slist_append(NULL, xff);
    if (!headers) {
        cc->err = ENOMEM;
        curl_easy_cleanup(curl);
        goto send_error;
    }
    curl_mime* mime = curl_mime_init(curl);
    if (!mime) {
        cc->err = ENOMEM;
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        goto send_error;
    }
    curl_mimepart* part = curl_mime_addpart(mime);
    if (!part) {
        cc->err = ENOMEM;
        curl_mime_free(mime);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        goto send_error;
    }
    curl_mime_name(part, cc->form_field);
    curl_mime_filename(part, cc->filename);
    curl_mime_data_cb(part, -1, curl_read_cb, NULL, NULL, cc); // -1 -> chunked
    curl_easy_setopt(curl, CURLOPT_URL, cc->upstream);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, cc);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    CURLcode curl_res = curl_easy_perform(curl);
    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (curl_res != CURLE_OK) {
        goto send_error;
    }

    send_all(cc->socket, cc->resp.data, cc->resp.size);
    goto cleanup;

send_error:
    {
        const char* curl_err = "Error POSTing to upstream server";
        if (cc->err == EWOULDBLOCK && !ALLOW_TIMEOUT)
        {
            curl_err = "Error: Receive Timout. Consider using `netcat -N` or something equivalent.";
        }
        send_all(cc->socket, curl_err, strlen(curl_err));
    }

cleanup:
    close(cc->socket);
    free(cc->resp.data);
    free(cc);
    pthread_mutex_lock(&conn_mutex);
    active_connections--;
    pthread_mutex_unlock(&conn_mutex);
    return 0;
}

void print_usage(const char* arg0)
{
    printf("Usage: %s [-l listen_ip] [-p listen_port] [-t connection_timeout] [-f form_field] [-n filename] upstream_url\n", arg0);
}

int main(int argc, char* argv[])
{
    const char* arg_listen_ip = DEFAULT_LISTEN_IP;
    int arg_port = DEFAULT_PORT;
    int arg_timeout = DEFAULT_TIMEOUT;
    const char* arg_upstream_url;
    const char* arg_form_field = DEFAULT_FORM_FIELD;
    const char* arg_filename = DEFAULT_FILENAME;
    int opt;
    while ((opt = getopt(argc, argv, "l:p:t:f:n:")) != -1)
    {
        switch (opt)
        {
            case 'l': arg_listen_ip = optarg; break;
            case 'p': {
                char* end;
                long port = strtol(optarg, &end, 10);
                if (*end != '\0' || port < 1 || port > 65535) {
                    fprintf(stderr, "Invalid port: %s\n", optarg);
                    return 1;
                }
                arg_port = (int)port;
                break;
            }
            case 't': {
                char* end;
                long timeout = strtol(optarg, &end, 10);
                if (*end != '\0' || timeout < 0) {
                    fprintf(stderr, "Invalid timeout: %s\n", optarg);
                    return 1;
                }
                arg_timeout = (int)timeout;
                break;
            }
            case 'f': arg_form_field = optarg; break;
            case 'n': arg_filename = optarg; break;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    if (optind >= argc)
    {
        print_usage(argv[0]);
        return 1;
    }
    arg_upstream_url = argv[optind];

    // Validate upstream URL scheme (only allow http/https)
    if (strncmp(arg_upstream_url, "http://", 7) != 0 &&
        strncmp(arg_upstream_url, "https://", 8) != 0) {
        fprintf(stderr, "Upstream URL must use http:// or https:// scheme\n");
        return 1;
    }

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    if (inet_pton(AF_INET, arg_listen_ip, &server_addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid listen IP: %s\n", arg_listen_ip);
        return 1;
    }
    server_addr.sin_port = htons(arg_port);
    const struct timeval timeout = {arg_timeout, 0};

    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1)
    {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return 1;
    }

    int reuse = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
    {
        fprintf(stderr, "Setting SO_REUSEADDR using setsockopt() failed: %s\n", strerror(errno));
        close(server_socket);
        return 1;
    }

    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1)
    {
        fprintf(stderr, "bind() failed: %s\n", strerror(errno));
        close(server_socket);
        return 1;
    }
    if (listen(server_socket, 100) == -1)
    {
        fprintf(stderr, "listen() failed: %s\n", strerror(errno));
        close(server_socket);
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);
    atexit(curl_global_cleanup);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    while (running)
    {
        struct client_connection* cc = calloc(1, sizeof(*cc));
        if (!cc) {
            perror("calloc");
            continue;
        }
        cc->upstream = arg_upstream_url;
        cc->form_field = arg_form_field;
        cc->filename = arg_filename;

        socklen_t addrlen = sizeof(cc->client_addr);
        cc->socket = accept(server_socket, (struct sockaddr*)&cc->client_addr, &addrlen);
        if (cc->socket == -1)
        {
            fprintf(stderr, "accept() failed: %s\n", strerror(errno));
            free(cc);
            continue;
        }
        if (arg_timeout != 0)
        {
            if (setsockopt(cc->socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1
                || setsockopt(cc->socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1)
            {
                fprintf(stderr, "Setting SO_RCVTIMEO or SO_SNDTIMEO using setsockopt() failed: %s\n", strerror(errno));
                close(cc->socket);
                free(cc);
                continue;
            }
        }

        pthread_t t;
        pthread_mutex_lock(&conn_mutex);
        if (active_connections >= MAX_CONNECTIONS) {
            pthread_mutex_unlock(&conn_mutex);
            fprintf(stderr, "Too many connections (max %d)\n", MAX_CONNECTIONS);
            close(cc->socket);
            free(cc);
            continue;
        }
        active_connections++;
        pthread_mutex_unlock(&conn_mutex);

        if (pthread_create(&t, NULL, thread, cc) != 0)
        {
            pthread_mutex_lock(&conn_mutex);
            active_connections--;
            pthread_mutex_unlock(&conn_mutex);
            fprintf(stderr, "pthread_create() failed: %s\n", strerror(errno));
            close(cc->socket);
            free(cc);
            continue;
        }
        pthread_detach(t);
    }
    close(server_socket);

    return 0;
}
