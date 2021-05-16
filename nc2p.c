#include <stdlib.h>
#include <stdio.h>
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
};

size_t curl_read_cb(char* buffer, size_t size, size_t nitems, void* user)
{
    struct client_connection* cc = (struct client_connection*)user;
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
    size_t realsize = size * nitems;
    if (realsize == 0)
    {
        return 0;
    }
    void* new = realloc(cc->resp.data, cc->resp.size + realsize + 1);
    if (new == NULL)
    {
        fprintf(stderr, "realloc() failed: %s\n", strerror(errno));
        cc->err = errno;
        return -1;
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

    CURL* curl = curl_easy_init();
    curl_mime* mime = curl_mime_init(curl);
    curl_mimepart* part = curl_mime_addpart(mime);
    curl_mime_name(part, cc->form_field);
    curl_mime_filename(part, cc->filename);
    curl_mime_data_cb(part, -1, curl_read_cb, NULL, NULL, cc); // -1 -> chunked
    curl_easy_setopt(curl, CURLOPT_URL, cc->upstream);
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, cc);
    CURLcode curl_res = curl_easy_perform(curl);
    curl_mime_free(mime);
    curl_easy_cleanup(curl);

    int sent;
    if (curl_res != CURLE_OK)
    {
        const char* curl_err = "Error POSTing to upstream server";
        if (cc->err == EWOULDBLOCK && !ALLOW_TIMEOUT)
        {
            curl_err = "Error: Receive Timout. Consider using `netcat -N` or something equivalent.";
        }
        sent = send(cc->socket, curl_err, strlen(curl_err)+1, 0);
    }
    else
    {
        sent = send(cc->socket, cc->resp.data, cc->resp.size, 0);
    }
    if (sent == -1)
    {
        printf("Error send()ing reply: %s\n", strerror(errno));
    }

    close(cc->socket);
    free(cc->resp.data);
    free(cc);
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
            case 'p': arg_port = atoi(optarg); break;
            case 't': arg_timeout = atoi(optarg); break;
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

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(arg_listen_ip);
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

    while (1)
    {
        int client_sock = accept(server_socket, NULL, NULL);
        if (client_sock == -1)
        {
            fprintf(stderr, "accept() failed: %s\n", strerror(errno));
            continue;
        }
        if (arg_timeout != 0)
        {
            if (setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == -1
                || setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == -1)
            {
                fprintf(stderr, "Setting SO_RCVTIMEO or SO_SNDTIMEO using setsockopt() failed: %s\n", strerror(errno));
                close(client_sock);
                continue;
            }
        }
        struct client_connection* cc = malloc(sizeof(*cc));
        memset(cc, 0, sizeof(*cc));
        cc->socket = client_sock;
        cc->upstream = arg_upstream_url;
        cc->form_field = arg_form_field;
        cc->filename = arg_filename;

        pthread_t t;
        if (pthread_create(&t, NULL, thread, cc) != 0)
        {
            fprintf(stderr, "pthread_create() failed\n");
            close(client_sock);
            continue;
        }
        pthread_detach(t);
    }
    close(server_socket);

    return 0;
}
