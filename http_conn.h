#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>

class util_timer;

class http_conn
{
public:
    static const int FILENAME_LEN = 200;        // 文件名的最大长度
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区的大小
    static const int WRITE_BUFFER_SIZE = 1024;  // 写缓冲区的大小

    // HTTP请求方法，这里只支持GET
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT };
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in& addr); // 初始化新接受的连接
    void close_conn();  // 关闭连接
    void process();    // 处理客户端请求
    bool read();      // 非阻塞读
    bool write();    // 非阻塞写

private:
    void init();                           // 初始化连接
    HTTP_CODE process_read();              // 解析HTTP请求
    bool process_write(HTTP_CODE ret);   // 填充HTTP应答

    // 下面这一组函数被process_read调用以分析HTTP请求
    HTTP_CODE parse_request_line(char* text);
    HTTP_CODE parse_headers(char* text);
    HTTP_CODE parse_content(char* text);
    HTTP_CODE do_request();
    char* get_line() { return m_read_buf + m_start_line; }
    LINE_STATUS parse_line();

    // 这一组函数被process_write调用以填充HTTP应答。
    void unmap();
    bool add_response(const char* format, ...);
    bool add_content(const char* content);
    bool add_content_type();
    bool add_status_line(int status, const char* title);
    bool add_headers(int content_length);
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       // 所有socket上的事件都被注册到同一个epoll内核事件中，所以设置成静态的
    static int m_user_count;    // 统计用户的数量
    util_timer* timer;          // 定时器

private:
    int m_sockfd;          // 该HTTP连接的socket
    sockaddr_in m_address; // 客户端的socket地址

    char m_read_buf[READ_BUFFER_SIZE];    // 读缓冲区
    int m_read_idx;                         // 标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置
    int m_checked_idx;                      // 当前正在分析的字符在读缓冲区中的位置
    int m_start_line;                       // 当前正在解析的行的起始位置

    CHECK_STATE m_check_state;              // 主状态机当前所处的状态
    METHOD m_method;                        // 请求方法

    char m_real_file[FILENAME_LEN];       // 客户请求的目标文件的完整路径，其内容等于 doc_root + m_url, doc_root是网站根目录
    char* m_url;                            // 客户请求的目标文件的文件名
    char* m_version;                        // HTTP协议版本号，我们仅支持HTTP1.1
    char* m_host;                           // 主机名
    int m_content_length;                   // HTTP请求的消息总长度
    bool m_linger;                          // HTTP请求是否要求保持连接

    char m_write_buf[WRITE_BUFFER_SIZE];  // 写缓冲区
    int m_write_idx;                        // 写缓冲区中待发送的字节数
    char* m_file_address;                   // 客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;                // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];                   // 我们将采用writev来执行写操作，所以定义下面两个成员，其中m_iv_count表示被写内存块的数量。
    int m_iv_count;

    int bytes_to_send;              // 将要发送的数据的字节数
    int bytes_have_send;            // 已经发送的字节数
};

// 定时器类
class util_timer {
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    time_t expire;   // 任务超时时间，这里使用绝对时间
    void (*cb_func)(http_conn*); // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
    http_conn* user_data;
    util_timer* prev;    // 指向前一个定时器
    util_timer* next;    // 指向后一个定时器
};

// 定时器链表，它是一个升序、双向链表，且带有头节点和尾节点。
class sort_timer_lst {
public:
    sort_timer_lst() : head(NULL), tail(NULL) {}
    // 链表被销毁时，删除其中所有的定时器
    ~sort_timer_lst() {
        util_timer* tmp = head;
        while (tmp) {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    // 将目标定时器timer添加到链表中
    void add_timer(util_timer* timer) {
        if (!timer) {
            return;
        }
        if (!head) {
            head = tail = timer;
            return;
        }
        /* 如果目标定时器的超时时间小于当前链表中所有定时器的超时时间，则把该定时器插入链表头部,作为链表新的头节点，
           否则就需要调用重载函数 add_timer(),把它插入链表中合适的位置，以保证链表的升序特性 */
        if (timer->expire < head->expire) {
            timer->next = head;
            head->prev = timer;
            head = timer;
            return;
        }
        add_timer(timer, head);
    }

    /* 当某个定时任务发生变化时，调整对应的定时器在链表中的位置。这个函数只考虑被调整的定时器的
    超时时间延长的情况，即该定时器需要往链表的尾部移动。*/
    void adjust_timer(util_timer* timer)
    {
        if (!timer) {
            return;
        }
        util_timer* tmp = timer->next;
        // 如果被调整的目标定时器处在链表的尾部，或者该定时器新的超时时间值仍然小于其下一个定时器的超时时间则不用调整
        if (!tmp || (timer->expire < tmp->expire)) {
            return;
        }
        // 如果目标定时器是链表的头节点，则将该定时器从链表中取出并重新插入链表
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            timer->next = NULL;
            add_timer(timer, head);
        }
        else {
            // 如果目标定时器不是链表的头节点，则将该定时器从链表中取出，然后插入其原来所在位置后的部分链表中
            timer->prev->next = timer->next;
            timer->next->prev = timer->prev;
            add_timer(timer, timer->next);
        }
    }
    // 将目标定时器 timer 从链表中删除
    void del_timer(util_timer* timer)
    {
        if (!timer) {
            return;
        }
        // 下面这个条件成立表示链表中只有一个定时器，即目标定时器
        if ((timer == head) && (timer == tail)) {
            delete timer;
            head = NULL;
            tail = NULL;
            return;
        }
        /* 如果链表中至少有两个定时器，且目标定时器是链表的头节点，
         则将链表的头节点重置为原头节点的下一个节点，然后删除目标定时器。 */
        if (timer == head) {
            head = head->next;
            head->prev = NULL;
            delete timer;
            return;
        }
        /* 如果链表中至少有两个定时器，且目标定时器是链表的尾节点，
        则将链表的尾节点重置为原尾节点的前一个节点，然后删除目标定时器。*/
        if (timer == tail) {
            tail = tail->prev;
            tail->next = NULL;
            delete timer;
            return;
        }
        // 如果目标定时器位于链表的中间，则把它前后的定时器串联起来，然后删除目标定时器
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        delete timer;
    }

    /* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/
    void tick() {
        if (!head) {
            return;
        }
        printf("timer tick\n");

        // printf("链表fd: ");//test------------
        // util_timer* test = head;
        // while(test){
        //     printf("%d",test->user_data->m_sockfd);
        //     test = test->next;
        // }
        // printf("\n");//test-----------------

        time_t cur = time(NULL);  // 获取当前系统时间
        util_timer* tmp = head;
        // 从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
        while (tmp) {
            /* 因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和系统当前时间，
            比较以判断定时器是否到期*/
            if (cur < tmp->expire) {
                break;
            }
            // 调用定时器的回调函数，以执行定时任务
            tmp->cb_func(tmp->user_data);
            // 执行完定时器中的定时任务之后，就将它从链表中删除，并重置链表头节点
            head = tmp->next;
            if (head) {
                head->prev = NULL;
            }
            delete tmp;
            tmp = head;
        }
    }

private:
    /* 一个重载的辅助函数，它被公有的 add_timer 函数和 adjust_timer 函数调用
    该函数表示将目标定时器 timer 添加到节点 lst_head 之后的部分链表中 */
    void add_timer(util_timer* timer, util_timer* lst_head) {
        util_timer* prev = lst_head;
        util_timer* tmp = prev->next;
        /* 遍历 list_head 节点之后的部分链表，直到找到一个超时时间大于目标定时器的超时时间节点
        并将目标定时器插入该节点之前 */
        while (tmp) {
            if (timer->expire < tmp->expire) {
                prev->next = timer;
                timer->next = tmp;
                tmp->prev = timer;
                timer->prev = prev;
                break;
            }
            prev = tmp;
            tmp = tmp->next;
        }
        /* 如果遍历完 lst_head 节点之后的部分链表，仍未找到超时时间大于目标定时器的超时时间的节点，
           则将目标定时器插入链表尾部，并把它设置为链表新的尾节点。*/
        if (!tmp) {
            prev->next = timer;
            timer->prev = prev;
            timer->next = NULL;
            tail = timer;
        }
    }

private:
    util_timer* head;   // 头结点
    util_timer* tail;   // 尾结点
};


#endif
