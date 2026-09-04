#include <stdio.h>
#include <strings.h>
#include <pthread.h>
#include "csapp.h"

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

// 缓存
typedef struct cache_block {
    char *uri; // 缓存的键：完整 URI，如 http://host:port/path
    char *data;//完整响应（头部 + 正文）
    size_t size;// data size
    unsigned seq;//LRU
    struct cache_block *next;
} cache_block;

static cache_block *cache_head = NULL;
static size_t cache_total = 0;
static unsigned long cache_seq = 0;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

// 命中返回 data 的拷贝，未命中返回 NULL；*size 带回响应长度
char *cache_lookup(char *uri, size_t *size)
{
    cache_block *p;
    char *copy = NULL;

    pthread_mutex_lock(&cache_lock);
    for (p = cache_head; p != NULL; p = p->next) {
        if (strcmp(p->uri, uri) == 0) {
            p->seq = ++cache_seq;// update LRU
            copy = Malloc(p->size);
            memcpy(copy, p->data, p->size);
            *size = p->size;
            break;
        }
    }
    pthread_mutex_unlock(&cache_lock);
    return copy;
}

// 把整个响应插入缓存（内容被拷贝进缓存，data 仍归调用者所有）
void cache_insert(char *uri, char *data, size_t size)
{
    cache_block *p, *prev, *min, *minprev, *newblock;

    if (size == 0 || size > MAX_OBJECT_SIZE) return;

    pthread_mutex_lock(&cache_lock);

    while (cache_head != NULL && cache_total + size > MAX_CACHE_SIZE) {
        min = cache_head;
        minprev = NULL;
        prev = NULL;
        for (p = cache_head; p != NULL; p = p->next) {
            if (p->seq < min->seq) {
                min = p;
                minprev = prev;
            }
            prev = p;
        }
        if (minprev != NULL)
            minprev->next = min->next;
        else
            cache_head = min->next;
        cache_total -= min->size;
        Free(min->uri);
        Free(min->data);
        Free(min);
    }

    // 在头部插
    newblock = Malloc(sizeof(cache_block));
    newblock->uri = Malloc(strlen(uri) + 1);
    strcpy(newblock->uri, uri);
    newblock->data = Malloc(size);
    memcpy(newblock->data, data, size);
    newblock->size = size;
    newblock->seq = ++cache_seq;
    newblock->next = cache_head;
    cache_head = newblock;
    cache_total += size;

    pthread_mutex_unlock(&cache_lock);
}

// HTTP

//copy error output
void clienterror(int fd, char *errnum, char *shortmsg, char *longmsg)
{
    char body[MAXBUF], head[MAXBUF];

    sprintf(body, "<html><head><title>Proxy Error</title></head>\r\n"
                  "<body><h1>%s: %s</h1><p>%s</p></body></html>\r\n",
            errnum, shortmsg, longmsg);
    sprintf(head, "HTTP/1.0 %s %s\r\nContent-Type: text/html\r\n"
                  "Content-Length: %ld\r\n\r\n",
            errnum, shortmsg, (long)strlen(body));
    rio_writen(fd, head, strlen(head));
    rio_writen(fd, body, strlen(body));
}

// 把 http://host[:port]/path 拆成 host、port、path 三部分。
// path 含前导 '/'；URI 中没有路径时补成 "/"；没有端口时用 "80"。
int parse_uri(char *uri, char *host, char *port, char *path)
{
    char *rest, *slash, *colon;

    if (strncasecmp(uri, "http://", 7) != 0)
        return -1;

    rest = uri + 7;// 指向 host[:port]/path
    slash = strchr(rest, '/');
    if (slash == NULL) {// 没有路径部分
        strcpy(path, "/");
        slash = rest + strlen(rest);
    } else
        strcpy(path, slash);

    if (slash - rest >= MAXLINE) return -1;
    strncpy(host, rest, slash - rest);
    host[slash - rest] = '\0';

    colon = strchr(host, ':');// port num
    if (colon != NULL) {
        *colon = '\0';
        strcpy(port, colon + 1);
    } else
        strcpy(port, "80");
    return 0;
}

// 向服务器转发请求

int forward_request(rio_t *rp, char *method, char *version,char *host, char *port, char *path)
{
    char req[4 * MAXLINE], buf[MAXLINE], hosthdr[MAXLINE];
    int serverfd;

    serverfd = open_clientfd(host, port);// 连接源服务器
    if (serverfd < 0) return -1;

    if (strcmp(port, "80") == 0)
        sprintf(hosthdr, "Host: %s\r\n", host);
    else
        sprintf(hosthdr, "Host: %s:%s\r\n", host, port);

    sprintf(req, "%s %s %s\r\n", method, path, version);
    strcat(req, hosthdr);
    strcat(req, user_agent_hdr);
    strcat(req, "Connection: close\r\n");
    strcat(req, "Proxy-Connection: close\r\n");

    // 透传客户端剩余的头部，直到空行
    while (rio_readlineb(rp, buf, MAXLINE) > 0) {
        if (strcmp(buf, "\r\n") == 0) break;
        if (strncasecmp(buf, "Host:", 5) == 0 ||
            strncasecmp(buf, "User-Agent:", 11) == 0 ||
            strncasecmp(buf, "Connection:", 11) == 0 ||
            strncasecmp(buf, "Proxy-Connection:", 17) == 0) continue;
        strcat(req, buf);
    }
    strcat(req, "\r\n");

    rio_writen(serverfd, req, strlen(req));
    return serverfd;
}

//读取整个响应
char *read_response(int serverfd, size_t *len)
{
    rio_t rio;
    char *resp;
    size_t total = 0, cap = MAXLINE;
    ssize_t n;

    rio_readinitb(&rio, serverfd);
    resp = Malloc(cap);
    while ((n = rio_readnb(&rio, resp + total, cap - total)) > 0) {
        total += n;
        if (total == cap) {
            cap *= 2;
            resp = Realloc(resp, cap);
        }
    }
    *len = total;
    return resp;
}

//处理单个客户端连接（work）

void work(int fd)
{
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char host[MAXLINE], port[MAXLINE], path[MAXLINE];
    rio_t rio;
    char *cached, *resp;
    size_t len;
    int serverfd;

    rio_readinitb(&rio, fd);
    if (rio_readlineb(&rio, buf, MAXLINE) <= 0) return;

    method[0] = uri[0] = version[0] = '\0';
    sscanf(buf, "%s %s %s", method, uri, version);
    if (version[0] == '\0') strcpy(version, "HTTP/1.0");

    if (strcasecmp(method, "GET") != 0) {
        clienterror(fd, "501", "Not Implemented", "Proxy supports GET only");
        return;
    }

    // cache命中则直接回给客户端
    if ((cached = cache_lookup(uri, &len)) != NULL) {
        rio_writen(fd, cached, len);
        Free(cached);
        return;
    }

    // 解析 URI，拆出 host / port / path 
    if (parse_uri(uri, host, port, path) < 0) {
        clienterror(fd, "400", "Bad Request", "Cannot parse request URI");
        return;
    }

    // 连接源服务器并转发请求
    serverfd = forward_request(&rio, method, version, host, port, path);
    if (serverfd < 0) {
        clienterror(fd, "500", "Internal Server Error","Cannot connect to origin server");
        return;
    }

    // 读完整响应 -> 写入缓存 -> 转发给客户端
    resp = read_response(serverfd, &len);
    Close(serverfd);
    cache_insert(uri, resp, len);
    rio_writen(fd, resp, len);
    Free(resp);
}

//main
void *thread(void *vargp)
{
    int connfd = *(int *)vargp;
    Free(vargp);
    Pthread_detach(pthread_self());
    work(connfd);
    Close(connfd);
    return NULL;
}

int main(int argc, char **argv)
{
    int listenfd, connfd;
    struct sockaddr_storage clientaddr;
    socklen_t clientlen;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    Signal(SIGPIPE, SIG_IGN);

    listenfd = open_listenfd(argv[1]);
    if (listenfd < 0) {
        fprintf(stderr, "cannot listen on port %s\n", argv[1]);
        exit(1);
    }

    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = accept(listenfd, (SA *)&clientaddr, &clientlen);
        if (connfd < 0) {
            if (errno == EINTR)continue;
            fprintf(stderr, "accept error: %s\n", strerror(errno));
            continue;
        }
        int *connfdp = Malloc(sizeof(int));//把 fd 打包传给线程
        *connfdp = connfd;
        Pthread_create(&tid, NULL, thread, connfdp);
    }
}