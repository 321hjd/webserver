#ifndef LST_TIMER
#define LST_TIMER

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
#include <sys/wait.h>
#include <sys/uio.h>

#include <time.h>
#include "../log/log.h"

//连接资源结构体成员需要用到定时器类
//需要前向声明
class util_timer;

//连接资源
struct client_data
{
    //客户端socket地址
    sockaddr_in address;
    //连接时所用的socket文件描述符
    int sockfd;
    //定时器
    util_timer *timer;
};

//定时回调函数
//定时事件/任务：用于当处理异常事件或定时器到期时，从内核事件表删除事件，关闭文件描述符，释放连接资源
void cb_func(client_data *user_data);

//定时器类
class util_timer
{
public:
    util_timer() : prev(NULL), next(NULL) {}

public:
    //超时时间，expire = 浏览器和服务器连接时刻(绝对时间) + 固定时间(TIMESLOT)
    time_t expire;
    //回调函数
    void (* cb_func)(client_data *);
    //连接资源
    client_data *user_data;
    //前向定时器
    util_timer *prev;
    //后继定时器
    util_timer *next;
};

//定时器容器类（带头尾节点的双向升序链表）
class sort_timer_lst
{
public:
    sort_timer_lst();
    //析构，负责销毁链表
    ~sort_timer_lst();

    //添加定时器，内部调用私有成员add_timer
    void add_timer(util_timer *timer);
    //调整定时器，任务发生变化时，调整定时器在链表中的位置，内部调用私有成员add_timer
    void adjust_timer(util_timer *timer);
    //将到期的定时器从链表中删除
    void del_timer(util_timer *timer);
    //定时任务处理函数
    void tick();

private:
    //私有成员，被公有成员add_timer和adjust_time调用（设置为私有成员的原因，方便复用代码，调整链表内部结点）
    void add_timer(util_timer *timer, util_timer *lst_head);

    util_timer *head;
    util_timer *tail;
};

class Utils
{
public:
    Utils() {}
    ~Utils() {}

    void init(int timeslot);

    //对文件描述符设置非阻塞
    /*问题：为什么要设置为非阻塞？
    send是将信息发送给套接字缓冲区，如果缓冲区满了，则会阻塞，这时候会进一步增加信号处理函数的执行时间，为此，将其修改为非阻塞。
    */
    int setnonblocking(int fd);

    //将内核事件表注册读事件，ET模式，并开启EPOLLONESHOT
    void addfd(int epollfd, int fd, bool one_shot, int TRIGMode);
    
    //信号处理函数
    static void sig_handler(int sig);

    //设置信号函数
    void addsig(int sig, void(handler)(int), bool restart = true);

    //定时处理任务，重新定时以不断触发SIGALRM信号
    void timer_handler();

    //显示错误信息
    void show_error(int connfd, const char *info);

public:
    static int *u_pipefd;    //信号处理函数到主线程的管道的文件描述符
    sort_timer_lst m_timer_lst;
    static int u_epollfd;    //统一事件源，用于监听信号事件的内核事件表文件描述符
    int m_TIMESLOT;
};

#endif