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

#include "threadpool.h"
#include "http_conn.h"
#include "fd_operate.h"
#include "conn_queue.h"

#define MAX_FD 65536   // 最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000  // 监听的最大的事件数量
#define TIMESLOT 5
static int pipefd[2];
static conn_queue que;


//添加信号处理
void addsig(int sig, void( handler )(int)){
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = handler;
    sigfillset( &sa.sa_mask );// ?
    assert( sigaction( sig, &sa, NULL ) != -1 );
}
//信号处理函数（将信号写入管道）
void sig_handler(int sig) {
    int save_errno = errno;
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0);
    errno = save_errno;
}


int main( int argc, char* argv[] ) {

    if( argc <= 1 ) {
        printf( "usage: %s port_number\n", basename(argv[0]));
        return 1;
    }
    int port = atoi( argv[1] );
    addsig( SIGPIPE, SIG_IGN );//忽略SIGPIPE信号(为了防止客户端进程终止，而导致服务器进程被SIGPIPE信号终止，因此服务器程序要处理SIGPIPE信号。)

    threadpool< http_conn >* pool = NULL;//创建线程池
    try {
        pool = new threadpool<http_conn>;
    } catch( ... ) {
        return 1;
    }

    http_conn* users = new http_conn[ MAX_FD ];//创建用于存储客户端连接的数组

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    printf("listen fd: %d\n",listenfd);

    int ret = 0;
    struct sockaddr_in address;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_family = AF_INET;
    address.sin_port = htons( port );

    int reuse = 1; // 端口复用
    setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof( reuse ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    ret = listen( listenfd, 5 );
   
    epoll_event events[ MAX_EVENT_NUMBER ]; // 创建epoll对象，和事件数组
    int epollfd = epoll_create( 5 );
    http_conn::m_epollfd = epollfd;
    printf("epollfd: %d\n",epollfd);

    addfd( epollfd, listenfd, false ); // 将监听文件描述符（listenfd）添加到epoll对象中
      
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd); // 创建管道,将读端加入epollfd
    setnonblocking(pipefd[1]);
    addfd(epollfd, pipefd[0], false);
    printf("pipefd: %d、%d\n",pipefd[0],pipefd[1]);
 
    addsig(SIGALRM,sig_handler); // 设置信号处理函数
    alarm(TIMESLOT); // 定时,5秒后产生SIGALARM信号
    bool timeout = false;


    while(true) {
        
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );    
        if ( ( number < 0 ) && ( errno != EINTR ) ) { //如果错误为EINTR表示在读/写的时候出现了中断错误
            printf( "epoll failure\n" );
            break;
        }

        for ( int i = 0; i < number; i++ ) {
            
            int sockfd = events[i].data.fd;
            
            if( sockfd == listenfd ) {    //1、监听描述符            
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                int connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength ); 
                printf("新建客户端连接fd: %d\n",connfd);              
                if ( connfd < 0 ) {
                    printf( "errno is: %d\n", errno );
                    continue;
                } 
                if( http_conn::m_user_count >= MAX_FD ) {
                    close(connfd);
                    continue;
                }
                users[connfd].init(connfd, client_address);   //初始化客户端连接
                // 设置超时时间，然后将客户端连接添加到队列que中            
                time_t cur = time(NULL);
                users[connfd].close_time = cur + 3 * TIMESLOT;
                que.push(users + connfd);
            } 

            else if (sockfd == pipefd[0] && (events[i].events & EPOLLIN)) { //2、读管道
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

            else if( events[i].events & ( EPOLLRDHUP | EPOLLHUP | EPOLLERR ) ) {  //3、断开连接
                users[sockfd].close_conn();
            }

            else if(events[i].events & EPOLLIN) {   //4、读数据，添加到工作队列，调整该连接对应的定时器，以延迟该连接被关闭的时间。
                if(users[sockfd].httpread()) {
                    pool->append(users + sockfd);
                    time_t cur = time(NULL);
                    users[sockfd].close_time = cur + 3 * TIMESLOT;
                    que.sort();
                } else {
                    users[sockfd].close_conn();
                }
            } 

            else if( events[i].events & EPOLLOUT ) {  //5、写数据
                if( !users[sockfd].httpwrite() ) {
                    users[sockfd].close_conn();
                }
            }

            // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
            if (timeout) { 
                printf("tick\n");
                que.tick();     
                alarm(TIMESLOT);// 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
                timeout = false;
            }           
        }                                        
    }
    
    close( pipefd[0] );
    close( pipefd[1] );
    close( epollfd );
    close( listenfd );
    delete [] users;
    delete pool;
    return 0;
}