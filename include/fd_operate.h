#ifndef FD_OPERATE
#define FD_OPERATE

#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>

//设置fd非阻塞
int setnonblocking( int fd );

// 向epoll中添加需要监听的文件描述符
void addfd( int epollfd, int fd, bool one_shot );

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd );
    
// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev);
    
#endif