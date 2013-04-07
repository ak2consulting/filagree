// node.c - socket client and server

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cyassl/ssl.h>
#include <pthread.h>
#include <dirent.h>
#include "vm.h"
#include "serial.h"
#include "sys.h"


#define MAXLINE 1024
#define STR_BUF_SIZE 1024


struct thread_argument {
    find_c_var *find;
    struct variable *listener;
    CYASSL_CTX *cya;
    CYASSL* ssl;
    int fd;
};

struct listen_arguments {
    struct variable *listener;
    int serverport;
    find_c_var *find;
};

static struct map *server_listeners = NULL; // maps port to listener
static struct map *socket_listeners = NULL; // maps fd to listener

void *incoming_connection(void *arg)
{
	char readline[MAXLINE];
    struct thread_argument *ta = (struct thread_argument *)arg;
    struct context *context = context_new(true);
    context->find = ta->find;

    for (;;)
    {
        bzero(readline, sizeof(readline));
        int n;
        if (((n = CyaSSL_read(ta->ssl, readline, MAXLINE)) <= 0))
        {
            fprintf(stderr, "client closed connection\n");
            goto free_ssl;
        }

        fprintf(stderr, "%d bytes received: %s\n", n, readline);
        struct byte_array *raw_message = byte_array_new_size(n);
        raw_message->data = (uint8_t*)readline;
        int32_t raw_message_length = serial_decode_int(raw_message);
        assert_message(raw_message_length < MAXLINE, "todo: handle long messages");
        struct variable *message = variable_deserialize(context, raw_message);

        struct variable *listener = (struct variable *)map_get(server_listeners, (void*)(VOID_INT)ta->fd);
        vm_call(context, listener, message);
    }

free_ssl:
	CyaSSL_free(ta->ssl); // Free CYASSL object
    free(ta);
    context_del(context);
	return NULL;
}

static bool int_compare(const void *a, const void *b, void *context)
{
    int32_t i = (VOID_INT)a;
    int32_t j = (VOID_INT)b;
    return i == j;
}

static int32_t int_hash(const void* x, void *context) {
    return (int32_t)(VOID_INT)x;
}

void *int_copy(const void *x, void *context) { return (void*)x; }
void int_del(const void *x, void *context) {}

void node_init()
{
    static bool done = false;
    if (!done)
        CyaSSL_Init(); // Initialize CyaSSL
    done = true;
}

// listens for incoming connections
void *sys_listen2(void *arg)
{
    struct listen_arguments *la = (struct listen_arguments*)arg;
    struct variable *listener = la->listener;
    int serverport = la->serverport;
    
    if (server_listeners == NULL)
        server_listeners = map_new_ex(NULL, &int_compare, &int_hash, &int_copy, &int_del);

    map_insert(server_listeners, (void*)(VOID_INT)serverport, listener);

    node_init();

	// Create and initialize CYASSL_CTX structure
	CYASSL_CTX* ctx;
	if ( (ctx = CyaSSL_CTX_new(CyaTLSv1_server_method())) == NULL)
	{
		fprintf(stderr, "CyaSSL_CTX_new error.\n");
		return NULL;
	}
    
	// Load CA certificates into CYASSL_CTX
	if (CyaSSL_CTX_load_verify_locations(ctx, "./conf/ca-cert.pem", 0) != SSL_SUCCESS)
	{
		fprintf(stderr, "Error loading ca-cert.pem, please check the file.\n");
		return NULL;
	}
    
	// Load server certificate into CYASSL_CTX
	if (CyaSSL_CTX_use_certificate_file(ctx, "conf/server-cert.pem", SSL_FILETYPE_PEM) != SSL_SUCCESS)
	{
		fprintf(stderr, "Error loading server-cert.pem, please check the file.\n");
		return NULL;
	}
    
	// Load server key into CYASSL_CTX
	if (CyaSSL_CTX_use_PrivateKey_file(ctx, "conf/server-key.pem", SSL_FILETYPE_PEM) != SSL_SUCCESS)
	{
		fprintf(stderr, "Error loading server-key.pem, please check the file.\n");
		return NULL;
	}

	// open the server socket over specified port 8080 to accept client connections
	int listenfd = socket(AF_INET, SOCK_STREAM, 0);

	// setsockopt: Eliminates "ERROR on binding: Address already in use" error.
	int optval = 1;
	setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval, sizeof(int));

	struct sockaddr_in servaddr;
	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family      = AF_INET;
	servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	servaddr.sin_port        = htons(serverport);

	bind(listenfd, (struct sockaddr *) &servaddr, sizeof(servaddr));
	listen(listenfd, 5);

	// create thread for processing each client request
	struct sockaddr_in client_addr;
	socklen_t sin_size = sizeof (struct sockaddr_in);

    for(;;)
	{
		int connfd = accept(listenfd, (struct sockaddr *) &client_addr, &sin_size );
		DEBUGPRINT("\n Got a connection from (%s , %d)\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

		// Create CYASSL Object
		CYASSL* ssl;
		if ((ssl = CyaSSL_new(ctx)) == NULL) {
            fprintf(stderr, "CyaSSL_new error");
            return NULL;
		}

		CyaSSL_set_fd(ssl, connfd);
        struct thread_argument *ta = (struct thread_argument *)malloc(sizeof(struct thread_argument));
        ta->find = la->find;
        ta->listener = listener;
        ta->ssl = ssl;
        ta->fd = connfd;
        ta->cya = ctx;

        DEBUGPRINT("listenting on %d - %p\n", connfd, ta->ssl);
        pthread_t child;
        pthread_create(&child, NULL, incoming_connection, &ta);
	}
    return NULL;
}

struct variable *sys_listen(struct context *context)
{
    struct variable *arguments = (struct variable*)stack_pop(context->operand_stack);
    struct listen_arguments *la = (struct listen_arguments*)malloc(sizeof(struct listen_arguments));
    la->serverport = param_int(arguments, 1);
    la->listener = variable_copy(context, (struct variable*)array_get(arguments->list, 2));
    
    pthread_t child;
    pthread_create(&child, NULL, sys_listen2, la);
    return NULL;
}

struct variable *sys_connect(struct context *context)
{
    struct variable *arguments = (struct variable*)stack_pop(context->operand_stack);
    const char *serveraddr = param_str(arguments, 1);
    const int32_t serverport = param_int(arguments, 2);
    struct variable *listener = ((struct variable*)array_get(arguments->list, 2));

	int sockfd;
	struct sockaddr_in servaddr;
	CYASSL_CTX* ctx;
	CYASSL* ssl;

	node_init();

	// Create and initialize CYASSL_CTX structure
	if ( (ctx = CyaSSL_CTX_new(CyaTLSv1_client_method())) == NULL)
	{
        context->vm_exception = variable_new_str(context, byte_array_from_string("SSL_CTX_new error"));
        CyaSSL_Cleanup();
        return NULL;
	}

	// Load CA certificates into CYASSL_CTX
	if (CyaSSL_CTX_load_verify_locations(ctx, "./conf/ca-cert.pem", 0) != SSL_SUCCESS)
	{
        context->vm_exception = variable_new_str(context, byte_array_from_string("Error loading ca-cert.pem, please check the file.\n"));
        CyaSSL_CTX_free(ctx);
        CyaSSL_Cleanup();
        return NULL;
	}

	// Create Socket file descriptor
	sockfd = socket(AF_INET, SOCK_STREAM, 0);

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_port = htons(serverport);
	inet_pton(AF_INET, serveraddr, &servaddr.sin_addr);

	// Blocking Connect to socket file descriptor
	connect(sockfd, (struct sockaddr *) &servaddr, sizeof(servaddr));

	// Create CYASSL object
	if ((ssl = CyaSSL_new(ctx)) == NULL)
	{
        context->vm_exception = variable_new_str(context, byte_array_from_string("CyaSSL_new error"));
        CyaSSL_CTX_free(ctx);
        CyaSSL_Cleanup();
        return NULL;
	}

	CyaSSL_set_fd(ssl, sockfd);
	fprintf(stderr, "Connected on %d -- %p\n", sockfd, ssl);

    struct thread_argument *ta = (struct thread_argument *)malloc(sizeof(struct thread_argument));
    ta->find = context->find;
    ta->listener = listener;
    ta->ssl = ssl;
    ta->fd = sockfd;
    ta->cya = ctx;

    if (socket_listeners == NULL)
        socket_listeners = map_new_ex(NULL, &int_compare, &int_hash, &int_copy, &int_del);
    map_insert(socket_listeners, (void*)(VOID_INT)sockfd, (void*)(VOID_INT)ta);

    return variable_new_int(context, sockfd);
}

struct variable *sys_send(struct context *context)
{
    struct variable *arguments = (struct variable*)stack_pop(context->operand_stack);
    struct variable *sender = (struct variable*)array_get(arguments->list, 1);
    const char *message = param_str(arguments, 2);

    assert_message(sender->type == VAR_INT, "non int fd");
    int32_t fd = sender->integer;
    struct thread_argument *ta = (struct thread_argument*)map_get(socket_listeners, (void*)(VOID_INT)fd);

    printf("send on ssl=%p\n", ta->ssl);
    if (CyaSSL_write(ta->ssl, message, strlen(message)) != strlen(message))
        context->vm_exception = variable_new_str(context, byte_array_from_string("CyaSSL_write error"));

    return NULL;
}

struct variable *sys_disconnect(struct context *context)
{
    struct variable *arguments = (struct variable*)stack_pop(context->operand_stack);
    if (arguments->list->length < 2)
    {
        struct variable *sockets = (struct variable *)array_get(arguments->list, 1);
        assert_message(sockets->type == VAR_LST, "non list of sockets");
        for (int i=0; i<sockets->list->length; i++)
        {
            struct thread_argument *tc = (struct thread_argument *)array_get(sockets->list, i);
            close(tc->fd);
            CyaSSL_free(tc->ssl);
        }
    }
    else
    {
        const int32_t fd = param_int(arguments, 1);
        close(fd);
        map_remove(socket_listeners, (void*)(VOID_INT)fd);
    }
    return NULL;
}