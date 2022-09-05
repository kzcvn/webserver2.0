#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5
static int pipefd[2];
static sort_timer_lst timer_lst;

// 添加、删除文件描述符（定义在http_conn.cpp中）
extern int setnonblocking(int fd);
extern void addfd(int epollfd, int fd, bool one_shot);
extern void removefd(int epollfd, int fd);

//添加信号处理
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);// ?
    assert(sigaction(sig, &sa, NULL) != -1);
}
//信号处理函数（将信号写入管道）
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}
// 定时处理任务，实际上就是调用tick()函数
void timer_handler()
{
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}
// 定时器回调函数，它删除非活动连接socket上的注册事件，并关闭之。
void cb_func(http_conn* user_data)
{
    user_data->close_conn();
}


int main(int argc, char* argv[]) {

    if (argc <= 1) {
        printf("usage: %s port_number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi(argv[1]);
    addsig(SIGPIPE, SIG_IGN);//忽略SIGPIPE信号(为了防止客户端进程终止，而导致服务器进程被SIGPIPE信号终止，因此服务器程序要处理SIGPIPE信号。)

    threadpool< http_conn >* pool = NULL;//创建线程池
    try {
        pool = new threadpool<http_conn>;
    }
    catch (...) {
        return 1;
    }

    http_conn* users = new http_conn[MAX_FD];//创建用于存储客户端连接的数组


    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    printf("listen fd: %d\n", listenfd);

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    // 端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    ret = bind(listenfd, (struct sockaddr*)&address, sizeof(address));
    ret = listen(listenfd, 5);

    // 创建epoll对象，和事件数组
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    http_conn::m_epollfd = epollfd;
    printf("epollfd: %d\n", epollfd);

    // 将监听文件描述符（listenfd）添加到epoll对象中
    addfd(epollfd, listenfd, false);

    // 创建管道,将读端加入epollfd
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    printf("pipefd: %d、%d\n", pipefd[0], pipefd[1]);

    // 设置信号处理函数
    addsig(SIGALRM, sig_handler);
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号
    bool timeout = false;

    while (true) {

        int number = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if ((number < 0) && (errno != EINTR)) { //如果错误为EINTR表示在读/写的时候出现了中断错误
            printf("epoll failure\n");
            break;
        }

        for (int i = 0; i < number; i++) {

            int sockfd = events[i].data.fd;

            if (sockfd == listenfd) {    //监听描述符            
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlength);
                printf("新建客户端连接fd: %d\n", connfd);
                if (connfd < 0) {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if (http_conn::m_user_count >= MAX_FD) {
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_address);   //添加客户端连接
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                timer->cb_func = cb_func;
                time_t cur = time(NULL);
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer(timer);
            }

            else if (sockfd == pipefd[0] && (events[i].events & EPOLLIN)) { //读管道
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if (ret == -1) {
                    continue;
                }
                else if (ret == 0) {
                    continue;
                }
                else {
                    timeout = true;
                }
            }

            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {  //断开连接，并移除对应的定时器。
                util_timer* timer = users[sockfd].timer;
                users[sockfd].close_conn();
                timer_lst.del_timer(timer);
            }

            else if (events[i].events & EPOLLIN) {   //读数据，添加到工作队列，调整该连接对应的定时器，以延迟该连接被关闭的时间。
                util_timer* timer = users[sockfd].timer;
                if (users[sockfd].read()) {
                    pool->append(users + sockfd);
                    time_t cur = time(NULL);
                    timer->expire = cur + 3 * TIMESLOT;
                    timer_lst.adjust_timer(timer);
                    //printf( "adjust timer once\n" );
                }
                else {
                    users[sockfd].close_conn();
                    timer_lst.del_timer(timer);
                }
            }

            else if (events[i].events & EPOLLOUT) {  //写数据
                util_timer* timer = users[sockfd].timer;
                if (!users[sockfd].write()) {
                    users[sockfd].close_conn();
                    timer_lst.del_timer(timer);
                }
            }

            // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
            if (timeout) {
                timer_handler();
                timeout = false;
            }
            //printf("user fd: %d%d%d%d\n",users[0].m_sockfd, users[7].m_sockfd, users[8].m_sockfd, users[9].m_sockfd);
        }                                          //test----------------

    }

    close(pipefd[0]);
    close(pipefd[1]);
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;
    return 0;
}