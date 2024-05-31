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
#include <sys/wait.h>
#include <sys/uio.h>
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn{
public:
    //设置读取文件的名称m_real_file大小
    static const int FILENAME_LEN = 200;
    //设置读缓冲区m_read_buf大小
    static const int READ_BUFFER_SIZE = 2048;
    //设置写缓冲区m_write_buf大小
    static const int WRITE_BUFFER_SIZE = 1024;
    //报文的请求方法，本项目只用到GET和POST
    enum METHOD{
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    //主状态机的状态
    enum CHECK_STATE{
        CHECK_STATE_REQUESTLINE = 0,    //解析请求行状态
        CHECK_STATE_HEADER,             //解析请求头部状态
        CHECK_STATE_CONTENT             //解析请求内容状态
    };
    //报文解析的结果
    enum HTTP_CODE{
        NO_REQUEST,         //表示请求不完整，还需要继续接收请求数据
        GET_REQUEST,        //获得了完整的HTTP请求
        BAD_REQUEST,        //HTTP请求报文有语法错误
        NO_RESOURCE,        //没有资源
        FORBIDDEN_REQUEST,  //拒绝访问
        FILE_REQUEST,       //表示请求文件存在，且可以访问
        INTERNAL_ERROR,     //服务器内部错误，该结果在主状态机逻辑switch的default下，一般不会触发
        CLOSED_CONNECTION   //关闭连接
    };
    //从状态机的状态
    //LINE_OK：读取到回车和换行字符
    //LINE_BAD：回车和换行字符单独出现在HTTP请求中
    //LINE_OPEN：未读取到完整请求，还需要继续读取
    enum LINE_STATUS{
        LINE_OK = 0,
        LINE_BAD,
        LINE_OPEN
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    //初始化套接字地址，函数内部会调用私有方法init
    void init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user, string passwd, string sqlname);
    //关闭http连接
    void close_conn(bool real_close = true);
    //http处理函数，内部调用process_read和prosess_write
    void process();
    //请求报文读取函数，一次性读取浏览器发来的全部数据
    bool read_once();
    //响应报文写入函数
    bool write();
    //获取套接字
    sockaddr_in *get_address(){
        return &m_address;
    }
    //同步线程初始化数据库读取表
    void initmysql_result(connection_pool *connPool);
    int timer_flag;     //超时标志
    int improv;         //


private:
    //初始化http请求/响应参数，由上面的init内部调用
    void init();
    //从m_read_buf读取，并处理请求报文
    HTTP_CODE process_read();
    //向m_write_buf写入响应报文数据
    bool process_write(HTTP_CODE ret);
    
    //请求报文解析
    //主状态机解析报文中的请求行数据
    HTTP_CODE parse_request_line(char *text);
    //主状态机解析报文中的请求头数据
    HTTP_CODE parse_headers(char *text);
    //主状态机解析报文中的请求内容
    HTTP_CODE parse_content(char *text);

    //生成响应报文
    HTTP_CODE do_request();

    //m_start_line是已经解析的字符
    //get_line用于将指针向后偏移，指向未处理的字符
    char *get_line(){
        return m_read_buf + m_start_line;
    }
    //从状态机读取一行，分析是请求报文的哪一部分
    LINE_STATUS parse_line();
    /*封装munmap函数
        mmap函数：用于申请一段内存空间。可以将这段内存作为进程间通信的共享内存，也可以将文件直接映射到其中。
        munmap函数：则释放由mmap创建的这段内存空间
    */
    void unmap();

    //根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;       //epoll I/O复用内核事件表的文件描述符（因为所有的用户请求事件都注册在同一张内核事件表上，因此为static）
    static int m_user_count;    //总用户量
    MYSQL *mysql;               //mysql对象，在其它头文件中include
    int m_state;                //0-读；1-写

private:
    int m_sockfd;               //用于通信的套接字，每个用户不同
    sockaddr_in m_address;

    //存储读取的请求报文数据，通过read_once读取
    char m_read_buf[READ_BUFFER_SIZE];
    //缓冲区中m_read_buf中数据的最后一个字节的下一个位置
    int m_read_idx;
    //m_read_buf读取的位置m_checked_idx
    int m_checked_idx;
    //m_read_buf中已经解析的字符个数
    int m_start_line;

    //存储发送的响应报文数据
    char m_write_buf[WRITE_BUFFER_SIZE];
    //指示buffer中的长度
    int m_write_idx;

    //主状态机的状态
    CHECK_STATE m_check_state;
    //请求方法
    METHOD m_method;

    //解析请求报文中对应的6个变量
    char m_real_file[FILENAME_LEN];     //客户请求的目标文件的完整路径，其内容等于doc_root+m_url，doc_root是网站根目录
    char *m_url;                        //客户请求的目标文件的文件名
    char *m_version;                    //http协议版本
    char *m_host;                       //域名
    int m_content_length;               //请求数据长度
    bool m_linger;                      //HTTP请求是否要求保持连接

    //
    char *m_file_address;               //客户请求的目标文件被mmap到内存逻辑地址的起始位置
    struct stat m_file_stat;            //目标文件的状态。通过它可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct iovec m_iv[2];               //io向量机制，使用writev执行写操作
    int m_iv_count;                     //被写内存块的数量
    int cgi;                            //是否启用POST，1-POST，0-非POST（用于登录校验功能）
    char *m_string;                     //存储请求数据（POST中为用户名和密码）
    int bytes_to_send;                  //剩余发送字节数
    int bytes_have_send;                //已发送字节数
    char *doc_root;                     //文件根目录

    map<string, string> m_users;        //{用户名，密码}
    int m_TRIGMode;                     //listenfd和connfd的模式组合。0-LT + LT；1-LT + ET；2-ET + LT；3-ET + ET
    int m_close_log;                    //http连接关闭标识

    char sql_user[100];
    char sql_passwd[100];
    char sql_name[100];
};

#endif