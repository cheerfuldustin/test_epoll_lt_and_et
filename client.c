#include <stdlib.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>

#define MAX_INPUT_CHAR 256
#define MAX_READ 256
#define WRITE_NUM 8

struct WRITE_BUFFER {
    struct WRITE_BUFFER* next;
    int size;
    char* ptr;
    char* buf;
};

struct WB_LIST {
    struct WRITE_BUFFER* head;
    struct WRITE_BUFFER* tail;
};

static struct WB_LIST wb_list;

static void* new_buffer(int size) {
    struct WRITE_BUFFER* wb = (struct WRITE_BUFFER*)malloc(sizeof(struct WRITE_BUFFER));
    wb->size = size;
    wb->buf = (char*)malloc(sizeof(char) * size);
    wb->ptr = wb->buf;
    wb->next = NULL;
}

static void free_buffer(struct WRITE_BUFFER* wb) {
    free(wb->buf);
    free(wb);
}

static int try_connect(const char* host, int port) {
	int sockfd, n;
	struct sockaddr_in serv_addr;
	struct hostent *server;

	sockfd = socket(AF_INET, SOCK_STREAM, 0);
	
	server = gethostbyname(host);
	
	bzero((char *) &serv_addr, sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	bcopy((char *)server->h_addr, (char *)&serv_addr.sin_addr.s_addr, server->h_length);
	serv_addr.sin_port = htons(port);

	if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
		printf("connect to server fail \n");
		exit(1);
	}

    printf("connect to server success \n");
    return sockfd;
}

static void input(int epfd, int fd, int event) {
    printf("################################ try to input, end with \'\\n\'\n");
    char buf[MAX_INPUT_CHAR];
    int idx = 0;
    for (;;) {
        int c = getchar();
        if (c == '\n'|| idx >= MAX_INPUT_CHAR - 1) {
            break;
        }

        // reset epollout
        if (c == '~') {
            struct epoll_event e;
            e.events = event & (~EPOLLET);
            e.data.fd = fd;
            epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
            
            e.events = event | EPOLLOUT;
            epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &e);
            printf("epollout reset\n");
            continue;
        }

        buf[idx] = c;
        idx++;
    }
    buf[idx] = '\0';

    if (idx > 0) {
       struct WRITE_BUFFER* wb = new_buffer(idx); 
       if (wb_list.tail == NULL) {
            wb_list.head = wb_list.tail = wb;

            struct epoll_event ee;
            ee.events = event; 
            ee.data.fd = fd;
            epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ee);
       }
       else {
           if (wb_list.head == wb_list.tail) {
               wb_list.head->next = wb; 
           }
            wb_list.tail->next = wb;
            wb_list.tail = wb;
       }
       memcpy(wb->buf, buf, sizeof(char) * idx);
    }
    printf("you have input: %s\n", buf);
}

static void output(int epfd, int fd, int event) {
    printf("begin to output\n");
    struct WRITE_BUFFER* wb = wb_list.head;
    if (wb) {
        int wsize = wb->size - (wb->ptr - wb->buf);
        if (wsize > WRITE_NUM) {
            wsize = WRITE_NUM;
        }
        int n = write(fd, wb->ptr, wsize);
        if (n <= 0) {
            abort();
        }

      
        printf("<<<<");
        char* ptr = (char*)wb->ptr;
		int i = 0;
        for (i = 0; i < n; i ++) {
            printf("%c", ptr[i]);
        }
		printf(", wsize:%d n:%d\n", wsize, n);
        printf("\n");
		

        wb->ptr += wsize;
        if (wb->ptr >= wb->buf + wb->size) {
            if (wb_list.head == wb_list.tail) {
                wb_list.head = wb_list.tail = NULL;
                free_buffer(wb);

                struct epoll_event ee;
                ee.events = event;
                ee.data.fd = fd;
                epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ee);
            }
            else {
                wb_list.head = wb_list.head->next;
                free_buffer(wb);
            }
        }
    }
    printf("end to output\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("please input mode, lt(1) or et(2)\n");
        return -1;
    }

    int ep_event = 0;
    if (strcmp(argv[1], "lt") == 0) {
        printf("client epoll set lt\n");
        ep_event = 0; 
    }
    else if (strcmp(argv[1], "et") == 0) {
        printf("client epoll set et\n");
        ep_event = EPOLLET;
    }
    else {
        printf("unknow mode %s please input lt or et\n", argv[1]);
        return -1;
    }

    int epfd = epoll_create(1024);
    if (epfd == -1) {
        printf("can not create epoll error:%d\n", errno);
        return -1;
    }

    int fd = try_connect("localhost", 8001);
    if (fd < 0) {
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);

    int socket_event = EPOLLIN | ep_event;
    struct epoll_event e;
    e.events = socket_event; 
    e.data.fd = fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &e);

    wb_list.head = wb_list.tail = NULL;

    for (;;) {
        input(epfd, fd, socket_event | EPOLLOUT);

        /* interested in stdout, it will return gt 0 anytime */
        struct epoll_event ev[1];
        int n = epoll_wait(epfd, ev, 1, -1);
        if (n == -1) {
            printf("epoll_wait error %d", errno);
            break;
        }
		
        int i = 0;
        for (i = 0; i < n; i ++) {
            struct epoll_event* e = &ev[i];
            int flag = e->events;
            int w = (flag & EPOLLOUT) != 0;
            if (w) {
                output(epfd, fd, socket_event);
            }
        }
    }

_EXIT:
    close(fd);
    close(epfd);

    return 0;
}
