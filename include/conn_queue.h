#ifndef CONN_QUEUE_H
#define CONN_QUEUE_H

#include <iostream>
#include <queue>
using namespace std;
#include "http_conn.h"


struct compare_conn_pointer_time //仿函数
{
	bool operator() (http_conn* a, http_conn* b)
	{
		return a->close_time > b->close_time; //小顶堆
	}
};

class conn_queue{
public:

    void push(http_conn* newconn){
        m_que.push(newconn);
    }

    void sort(){
        http_conn* tmp = m_que.top();
        m_que.pop();
        m_que.push(tmp);
    }

    void tick(){
        time_t cur = time(NULL);  // 获取当前系统时间
        while(!m_que.empty() && m_que.top()->close_time <= cur){
            m_que.top()->close_conn();
            m_que.pop();
        }
    }

private:
    priority_queue<http_conn*, vector<http_conn*>, compare_conn_pointer_time> m_que;
};


#endif