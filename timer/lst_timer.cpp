#include "lst_timer.h"
#include "../http/http_conn.h"


/*------------------------sort_timer_lst----------------------------------*/
sort_timer_lst::sort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
//析构，负责销毁链表
sort_timer_lst::~ort_timer_lst()
{
    head = NULL;
    tail = NULL;
}
//添加定时器，内部调用私有成员add_timer
void sort_timer_lst::add_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if (!head)
    {
        head = tail = timer;
        return;
    }
    //若新的定时器超时时间小于当前头节点
    //则直接将新定时器节点作为头节点
    if (timer->expire < head->expire)
    {
        timer->next = head;
        head-prev = timer;
        head = timer;
        return;
    }
    //否则调用私有成员，将其插入到合适的中间位置
    add_timer(timer, head);
}
//调整定时器，任务发生变化时，调整定时器在链表中的位置，内部调用私有成员add_timer
void sort_timer_lst::adjust_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    util_timer *tmp = timer->next;

    //注意：这一部分是否可以通过虚拟头节点统一处理？

    //情况1：被调整定时器在链表尾部，或超时时间仍小于下一个定时器超时值，无需调整
    //在尾部无需调整，是因为调整定时器都是因为需要更新超时时间，会变得比原来更大
    if (!tmp || timer->expire < tmp->expire)
    {
        return;
    }
    //情况2：被调整定时器在链表头部，则将该定时器取出，并重新插入
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        timer->next = NULL;
        add_timer(timer, head);
    }
    //情况3：被调整定时器在内部，则将定时器取出，并重新插入
    else
    {
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}
//将到期的定时器从链表中删除
void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    //链表中只有一个定时器，需要删除该定时器
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    //被删除定时器为头节点
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    //被删除定时器为尾节点
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    //被删除定时器在链表内部（常规节点删除）
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}
//定时任务处理函数
//说明：SIGALRM信号每次被触发，主循环中调用一次定时任务处理函数，处理链表容器中到期的定时器
//1.遍历定时器升序链表容器，从头结点开始依次处理每个定时器，直到遇到尚未到期的定时器
//2.若当前时间小于定时器超时时间，跳出循环，即未找到到期的定时器
//3.若当前时间大于定时器超时时间，即找到了到期的定时器，执行回调函数，然后将它从链表中删除，然后继续遍历
void sort_timer_lst::tick()
{
    //若链表为空，则无需处理
    if (!head)
    {
        return;
    }
    //获取当前时间
    time_t cur = time(NULL);
    util_timer *tmp = head;
    //遍历定时器链表
    while (tmp != NULL)
    {
        //链表容器为升序链表
        //若当前时间小于某个定时器超时时间，则说明后面的定时器都没有到超时时间
        if (cur < tmp->expire)
        {
            break;
        }
        //若当前定时器到期，则调用回调函数，执行定时事件（清理非活动连接）
        tmp->cb_func(tmp->user_data);
        //将处理后的定时器从容器中删除，并重置头节点
        //不能直接调用del_timer，因为这里还需要head，之前的设计不能适配这里的需求，除非令del_timer的返回值为head
        head = tmp->next;
        if(head != NULL)
        {
            head->prev = NULL;
        }
        delete tmp;
        tmp = head;
    }
}
//私有add_timer函数，常规添加节点函数，用于调整链表内部（非head/tail）节点
void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head)
{
    util_timer *prev = lst_head;    //待插入位置的前一节点
    util_timer *tmp = prev->next;   //待插入位置的后一节点
    //遍历当前节点之后的链表，按照超时时间找到目标定时器对应位置，执行常规双向链表插入操作
    while (tmp != NULL)
    {
        if (timer->expire < tmp->expire)
        {
            prev->next = timer;
            timer->next = tmp;
            tmp->prev = timer;
            timer->prev = prev;
            break;
        }
        //移动节点
        prev = tmp;
        tmp = tmp->next;
    }
    //遍历完成后发现目标定时器需要放到尾部节点tail处
    if (!tmp)
    {
        prev->next = timer;
        timer->prev = prev;
        timer->next = NULL;
        tail = timer;
    }
}

/*-------------------------------------------------------------------*/

/*------------------------Utils----------------------------------*/

void Utils::init(int timeslot)
{
    m_TIMESLOT = timeslot;
}
//对文件描述符设置非阻塞
int Utils::setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}
//将内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void Utils::addfd(int epollfd, int fd, bool one_shot, int TRIGMode)
{
    epoll_event event;
    event.data.fd = fd;
    
    if (1 == TRIGMode)
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    else
        event.events = EPOLLIN | EPOLLRDHUP;

    if (one_shot)
    {
        event.events |= EPOLLONESHOT;
    }
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}
//信号处理函数
//信号处理函数中仅仅通过管道发送信号值，不处理信号对应的逻辑，缩短异步执行时间，减少对主程序的影响。
void Utils::sig_handler(int sig)
{
    //为保证函数的可重入性，保留原来的errno
    //可重入性表示中断后再次进入该函数，环境变量与之前相同，不会丢失数据
    int save_errno = errno;
    int msg = sig;
    //将信号值从管道写端写入，传输字符类型，而非整型
    send(u_pipefd[1], (char *)&msg, 1, 0);
    //将原来的errno赋值为当前的errno
    errno = save_errno;
}
//设置信号函数
void Utils::addsig(int sig, void(handler)(int), bool restart)
{
    //创建sigaction结构体变量
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    //handler是信号处理函数的函数句柄，信号处理函数中仅仅发送信号值，不做对应逻辑处理
    sa.sa_handler = handler;
    if (restart)
        sa.sa_flags |= SA_RESTART;
    //将所有信号添加到信号集中
    sigfillset(&sa.sa_mask);
    //执行sigaction函数
    assert(sigaction(sig, &sa, NULL) != -1);
}
//定时处理任务，重新定时以不断触发SIGALRM信号
void Utils::timer_handler()
{
    m_timer_lst.tick();
    //设置信号传送闹钟，即用来设置信号SIGALRM在经过参数m_TIMESLOT秒数后发送给目前的进程
    alarm(m_TIMESLOT);
}
//显示错误信息
void Utils::show_error(int connfd, const char *info)
{
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

//静态成员遍历初始化
int *Utils::u_pipefd = 0;
int Utils::u_epollfd = 0;

/*-------------------------------------------------------------------*/

/*--------------------------回调函数cb_func----------------------------*/
class Utils;
void cb_func(client_data *user_data)
{
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    //断言以检测指针user_data是否合法，防止后面使用指针时出现异常
    assert(user_data);      
    close(user_data->sockfd);
    http_conn::m_user_count--;
}

/*-------------------------------------------------------------------*/