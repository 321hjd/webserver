#include "http_conn.h"

#include <mysql/mysql.h>
#include <fstream>

//定义http响应的一些状态信息
const char *ok_200_title = "OK";
const char *error_400_title = "Bad Request";
const char *error_400_form = "Your request has bad syntax or is inherently impossible to staisfy.\n";
const char *error_403_title = "Forbidden";
const char *error_403_form = "You do not have permission to get file form this server.\n";
const char *error_404_title = "Not Found";
const char *error_404_form = "The requested file was not found on this server.\n";
const char *error_500_title = "Internal Error";
const char *error_500_form = "There was an unusual problem serving the request file.\n";

locker m_lock;
map<string, string> users;      //？这不是类中定义了吗？

/*------------------------------数据库相关代码-----------------------------*/

//同步线程初始化数据库读取表
void http_conn::initmysql_result(connection_pool *connPool){
    //先从连接池取出一个连接
    MYSQL *mysql = NULL;
    connectionRAII mysqlcon(&mysql, connPool);

    //在数据库的user表中检索username，passwd数据（由浏览器端输入）
    if(mysql_query(mysql, "SELECT username,passwd FROM user")){
        LOG_ERROR("SELECT error:%s\n", mysql_error(mysql));
    }

    //从表中检索完整的结果集
    MYSQL_RES *result = mysql_store_result(mysql);

    //返回结果集中的列数
    int num_fields = mysql_num_fields(result);

    //返回所有字段结构的数据
    MYSQL_FIELD *fields = mysql_fetch_fields(result);

    //从结果集中获取下一行，将对应的用户名和密码，存入map中
    while(MYSQL_ROW row = mysql_fetch_row(result)){
        string temp1(row[0]);
        string temp2(row[2]);
        users[temp1] = temp2;
    }
}

/*-------------------------epoll相关代码--------------------------------------*/

//对文件描述符设置非阻塞
//fcntl函数提供对文件描述符的控制操作
int setnonblocking(int fd){
    //获取文件描述符旧的状态标志
    int old_option = fcntl(fd, F_GETFL);
    //设置非阻塞标志
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    //返回文件描述符旧的状态标志，以便日后恢复该状态标志
    return old_option;
}

//往内核事件表注册读事件，ET模式，选择开启EPOLLONESHOT
void addfd(int epollfd, int fd, bool one_shot, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if(1 == TRIGMode){
        event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    }
    else{
        event.events = EPOLLIN | EPOLLRDHUP;
    }

    if(one_shot){
        event.events = EPOLLONESHOT;
    }
    //epoll_ctl用于操作epoll的内核事件表
    //epollfd：内核事件表的文件描述符
    //EPOLL_CTL_ADD：往事件表中注册fd上的事件
    //fd：要操作的文件描述符
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);
    setnonblocking(fd);
}

//从内核事件表中删除事件
void removefd(int epollfd, int fd){
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//将事件重置为EPOLLONESHOT
//某工作线程处理完毕socket后，需要重置事件，否则当该socket再次就绪时，其它线程后续无法处理该socket
void modfd(int epollfd, int fd, int ev, int TRIGMode){
    epoll_event event;
    event.data.fd = fd;

    if(1 == TRIGMode){
        event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP; 
    }
    else{
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP; 
    }

    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}


/*------------------------------http_conn相关代码-------------------------*/

int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//关闭连接。关闭一个连接，客户总量-1
void http_conn::close_conn(bool real_close){
    if(real_close && (m_sockfd != -1)){
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化连接,外部调用初始化套接字地址（每个用户的连接就需要实例化一个对象）
void http_conn::init(int sockfd, const sockaddr_in &addr, char *root, int TRIGMode, int close_log, string user, string passwd, string sqlname){
    m_sockfd = sockfd;
    m_address = addr;
    
    //注册事件
    //？注册时使用默认的m_TRIGMode=0？
    addfd(m_epollfd, sockfd, true, m_TRIGMode);
    m_user_count++;

    //当浏览器出现连接重置时，可能是网站根目录出错或http响应格式出错或者访问的文件中内容完全为空
    doc_root = root;
    m_TRIGMode = TRIGMode;
    m_close_log = close_log;

    strcpy(sql_user, user.c_str());
    strcpy(sql_passwd, passwd.c_str());
    strcpy(sql_name, sqlname.c_str());

    init();
}

//初始化新接受的连接
//check_state默认为分析请求行状态
void http_conn::init()
{
    mysql = NULL;
    bytes_to_send = 0;
    bytes_have_send = 0;
    m_check_state = CHECK_STATE_REQUESTLINE;
    m_linger = false;
    m_method = GET;
    m_url = 0;
    m_version = 0;
    m_content_length = 0;
    m_host = 0;
    m_start_line = 0;
    m_checked_idx = 0;
    m_read_idx = 0;
    m_write_idx = 0;
    cgi = 0;
    m_state = 0;
    timer_flag = 0;
    improv = 0;

    memset(m_read_buf, '\0', READ_BUFFER_SIZE);
    memset(m_write_buf, '\0', WRITE_BUFFER_SIZE);
    memset(m_real_file, '\0', FILENAME_LEN);
}

//循环读取客户数据，直到无数据可读或对方关闭连接
//非阻塞ET工作模式下，需要一次性将数据读完
bool http_conn::read_once(){
    if(m_read_idx >= READ_BUFFER_SIZE){
        return false;
    }
    int bytes_read = 0;

    //LT读取数据
    if(0 == m_TRIGMode){
        //系统调用recv从通信socket中读取数据，返回读取的字节数
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;
        //读取失败
        if(bytes_read <= 0){
            return false;
        }
        return true;
    }
    //ET读取数据
    else{
        while(true){
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if(-1 == bytes_read){
                if(errno == EAGAIN || errno == EWOULDBLOCK){
                    break;
                }
                return false;
            }
            else if(0 == bytes_read){
                return false;
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

/*------------------------从状态机，用于分析出一行内容----------------------*/
/*返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN*/

//m_read_idx指向缓冲区m_read_buf的数据末尾的下一个字节
//m_checked_idx指向从状态机当前正在分析的字节
http_conn::LINE_STATUS http_conn::parse_line(){
    char temp;
    for(; m_checked_idx < m_read_idx; ++m_checked_idx){
        //temp为将要分析的字节
        temp = m_read_buf[m_checked_idx];
        //如果当前是\r字符，则有可能会读取到完整行
        if('\r' == temp){
            //到达末尾但没读到\n，说明不是完整行，需要继续读
            if((m_checked_idx + 1) == m_read_idx){
                return LINE_OPEN;
            }
            //下一个字符是\n，将\r\n改为\0\0
            else if('\n' == m_read_buf[m_checked_idx + 1]){
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            //如果都不符合，则返回语法错误
            return LINE_BAD;
        }
        //如果当前字符是\n，也有可能读取到完整行
        //一般是上次读取到\r就到buffer末尾了，没有接收完整，再次接收时会出现这种情况
        else if('\n' == temp)
        {
            //前一个字符是\r，则接收完整
            if(m_checked_idx > 1 && '\r' == m_read_buf[m_checked_idx - 1])
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            return LINE_BAD;
        }
    }
    //并没有找到\r\n，请求不完整，需要继续接收
    return LINE_OPEN;
}
/*--------------------------------------------------------------------*/


/*------------------------主状态机----------------------*/
//主状态机初始状态是CHECK_STATE_REQUESTLINE，通过调用从状态机来驱动主状态机，在主状态机进行解析前，从状态机已经将每一行的末尾\r\n符号改为\0\0，以便于主状态机直接取出对应字符串进行处理。

//解析http请求行，获得请求方法，目标url及http版本号
http_conn::HTTP_CODE http_conn::parse_request_line(char *text){
    //在HTTP报文中，请求行用来说明请求类型,要访问的资源以及所使用的HTTP版本，其中各个部分之间通过\t或空格分隔。
    //请求行中最先含有空格和\t任一字符的位置并返回
    //strpbrk搜索text中第一次出现的任何指定字符集中的字符（请求url和请求方法、版本号以空格或\t分割）
    //若搜索到则返回对应字符，否则返回NULL
    m_url = strpbrk(text, " \t");
    if(!m_url){
        return BAD_REQUEST;
    }
    //将该位置改为\0，用于将前面数据取出
    *m_url++ = '\0';
    //将该位置改为\0，用于将前面数据取出
    char *method = text;
    if(strcasecmp(method, "GET") == 0){
        m_method = GET;
    }
    else if(strcasecmp(method, "POST") == 0){
        m_method = POST:
        cgi = 1;
    }
    else{
        return BAD_REQUEST;
    }

    //m_url此时跳过了第一个空格或\t字符，但不知道之后是否还有
    //将m_url向后偏移，通过查找，继续跳过空格和\t字符，指向请求资源的第一个字符
    m_url += strspn(m_url, " \t");
    //使用与判断请求方式的相同逻辑，判断HTTP版本号
    m_version = strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    //仅支持HTTP/1.1
    if (strcasecmp(m_version, "HTTP/1.1") != 0)
        return BAD_REQUEST;
    //对请求资源前7个字符进行判断
    //这里主要是有些报文的请求资源中会带有http://，这里需要对这种情况进行单独处理
    if (strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr(m_url, '/');
    }
    //https情况
    if (strncasecmp(m_url, "https://", 8) == 0)
    {
        m_url += 8;
        m_url = strchr(m_url, '/');
    }   

    //一般的不会带有上述两种符号，直接是单独的/或/后面带访问资源
    if(!m_url || m_url[0] != '/'){
        return BAD_REQUEST;
    }
    //当url为/时，显示欢迎界面
    if(strlen(m_url) == 1){
        strcat(m_url, "judge.html");
    }
    //请求行解析完毕，主状态机从CHECK_STATE_REQUESTLINE切换到CHECK_STATE_HEADER状态
    m_check_state = CHECK_STATE_HEADER;
    return NO_REQUEST;
}

//解析http请求的一个头部信息
http_conn::HTTP_CODE http_conn::parse_headers(char *text){
    //遇到空行，则标识头部字段解析完毕
    if(text[0] == '\0'){
        
        //如果HTTP请求有消息体，则说明是POST请求
        //需要读取m_content_length字节的消息体，状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length != 0){
            //POST需要跳转到消息体处理状态
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        //否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    //解析请求头部连接字段
    else if(strncasecmp(text, "Connection:", 11) == 0){
        text += 11;
        //跳过空格和\t字符
        text += strspn(text, " \t");
        if(strcasecmp(text, "keep-alive") == 0){
            //如果是长连接，则将linger标志设置为true
            m_linger = true;
        }
    }
    //解析请求头部内容长度字段
    else if (strncasecmp(text, "Content-length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);
    }
    //解析请求头部HOST字段
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        LOG_INFO("oop!unknow header: %s", text);
    }
    return NO_REQUEST;
}

//判断http请求是否被完整读入
http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    //判断buffer中是否读取了消息体
    if (m_read_idx >= (m_content_length + m_checked_idx))
    {
        text[m_content_length] = '\0';
        //POST请求中最后为输入的用户名和密码
        m_string = text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
/*--------------------------------------------------------------------*/


//process_read从m_read_buf读取，并处理请求报文
http_conn::HTTP_CODE http_conn::process_read(){

    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char *text = 0;

    //通过while循环，将主从状态机进行封装，对报文的每一行进行循环处理
    //判断条件：
    //1.主状态机转移到CHECK_STATE_CONTENT，即请求行和头部都解析完毕
    //2.从状态机转移到LINE_OK，该条件涉及解析请求行和请求头部
    while((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) || ((line_status == parse_line()) == LINE_OK)){
        //循环体工作流程
        //1.从状态机循环读取数据(while判断条件中)
        //2.调用get_line函数，通过m_start_line将从状态机读取数据间接赋给text
        //3.主状态机解析text
        text = get_line();
        m_start_line = m_checked_idx;
        LOG_INFO("%s", text);
        //主状态机解析text
        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:{
                //解析请求行
                ret = parse_request_line(text);
                if(ret == BAD_REQUEST)
                    return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:{
                //解析请求头
                ret = parse_headers(text);
                if (ret == BAD_REQUEST)
                    return BAD_REQUEST;
                //完整解析GET请求后，跳转到报文响应函数
                else if (ret == GET_REQUEST)
                    return do_request();
                break;
            }
            case CHECK_STATE_CONTENT:{
                //解析消息体
                ret = parse_content(text);
                //完整解析POST请求后，跳转到报文响应函数
                if (ret == GET_REQUEST)
                    return do_request();
                //解析完消息体即完成报文解析，避免再次进入循环，更新line_status
                //问题：为什么CHECK_STATE_HEADER状态下完成解析后不用更新line_status？
                line_status = LINE_OPEN;
                break;
            }
            default:
                return INTERNAL_ERROR;
        }
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::do_request(){
    //将初始化的m_real_file赋值为网站根目录
    strcpy(m_real_file, doc_root);
    int len = strlen(doc_root);

    //找到m_url中/的位置
    const char *p = strrchr(m_url, '/'); 

    //实现登录和注册校验（基于CGI）
    if(cgi == 1 && (*(p + 1) == '2' || *(p + 1) == '3'))
    {
        //根据标志判断是登录检测还是注册检测
        char flag = m_url[1];       //2-登录校验；3-注册校验

        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url + 2);
        strncpy(m_real_file + len, m_url_real, FILENAME_LEN - len - 1);

        free(m_url_real);

        //提取用户名和密码
        //格式：user=123&password=123
        char name[100], password[100];
        int i;
        for(i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j;
        for(j = 0, i = i + 10; m_string[i] != '\0'; ++i, ++j){
            password[j] = m_string[i];
        }
        password[j] = '\0';

        //注册校验
        if(*(p + 1) == '3'){
            //注册，需检测数据库中是否有重名用户，若没有重名，则添加数据
            char *sql_insert = (char *)malloc(sizeof(char) * 200);
            strcpy(sql_insert, "INSERT INTO user(username, passwd) VALUES(");
            strcat(sql_insert, "'");
            strcat(sql_insert, name);
            strcat(sql_insert, "', '");
            strcat(sql_insert, password);
            strcat(sql_insert, "')");

            //users是一个map，存储用户名和密码
            if(users.find(name) == users.end()){
                m_lock.lock();
                int res = mysql_query(mysql, sql_insert);
                user.insert(pair<string, string>(name, password));
                m_lock.unlock();

                if(!res)
                    strcpy(m_url, "/log.html");
                else
                    strcpy(m_url, "/registerError.html");
            }
            else
                strcpy(m_url, "/registerError.html");
        }

        //登录校验
        else if(*(p + 1) == '2'){
            if(users.find(name) != users.end() && users[name] == password)
                strcpy(m_url, "/welcome.html"); 
            else
                strcpy(m_url, "/logError.html");
        }
    }

    /*-----------------------------页面跳转设置（实际应该用常量或者专用用一个初始化文件来处理这些页面，否则修改会比较麻烦？）------------------------*/
    //如果请求资源为/0，表示跳转注册界面
    if(*(p+1) == '0'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");

        //将网站目录和/register.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果请求资源为/1，表示跳转登录界面
    else if(*(p+1) == '1'){
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");

        //将网站目录和/log.html进行拼接，更新到m_real_file中
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果以上均不符合，即不是登录和注册，直接将url与网站目录拼接
    //这里的情况是welcome界面，请求服务器上的一个图片
    else
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //通过stat(系统调用)获取请求资源文件信息，成功则将信息更新到m_file_stat结构体
    //失败返回NO_RESOURCE状态，表示资源不存在
    if(stat(m_real_file, &m_file_stat) < 0)
        return NO_RESOURCE;

    //判断文件的权限，是否可读，不可读则返回FORBIDDEN_REQUEST状态
    if(!(m_file_stat.st_mode&S_IROTH))
        return FORBIDDEN_REQUEST;
    //判断文件类型，如果是目录，则返回BAD_REQUEST，表示请求报文有误
    if(S_ISDIR(m_file_stat.st_mode))
        return BAD_REQUEST;

    //以只读方式获取文件描述符，通过mmap将该文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    //为了提高访问速度，通过mmap进行映射，将普通文件映射到内存逻辑地址
    m_file_address = (char*)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);

    //避免文件描述符的浪费和占用
    close(fd);

    //表示请求文件存在，且可以访问
    return FILE_REQUEST;
}  

//释放由mmap创建的文件到内存逻辑地址的内存空间
void http_conn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

//响应报文写入函数，写线程从请求队列中取出m_write_buf，并写入通信socket
bool http_conn::write(){
    int temp = 0;

    //若需要发送的数据字节数为0，则立刻重置连接
    //表示响应报文为空，一般不会出现这种情况
    if (bytes_to_send == 0)
    {
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        init();
        return true;
    }
    //写入socket
    while (1)
    {
        //通过writev函数循环发送响应报文数据给浏览器端
        //返回发送的字节数
        temp = writev(m_sockfd, m_iv, m_iv_count);

        //若writev单次发送不成功，判断是否是写缓冲区满了
        if (temp < 0)
        {
            //若eagain则说明写缓冲区满
            //更新iovec结构体的指针和长度，并注册写事件，等待下一次写事件触发
            if (errno == EAGAIN)
            {
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
                return true;
            }
            unmap();
            return false;
        }

        //更新已发送字节和待发送字节
        bytes_have_send += temp;
        bytes_to_send -= temp;
        //第一个iovec头部信息的数据已发送完，需要准备好发送第二个iovec数据，需偏移文件指针
        if (bytes_have_send >= m_iv[0].iov_len)
        {
            //不再继续发送头部信息
            m_iv[0].iov_len = 0;
            //偏移文件iovec的指针
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        //继续发送第一个iovec头部信息的数据
        else
        {
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }

        //判断响应报文整体是否发送成功
        if (bytes_to_send <= 0)
        {
            unmap();
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
            //长连接
            if (m_linger)
            {
                //重置http类实例，注册读事件
                init();
                //返回true表示不关闭连接
                return true;
            }
            //短连接
            else
            {
                //返回false表示关闭连接
                return false;
            }
        }
    }
}

//根据响应报文格式，生成对应8个部分，以下函数均由do_request调用
//下面的响应报文写入函数均通过add_response更新m_write_idx指针和缓冲区m_write_buf中的内容
bool http_conn::add_response(const char *format, ...){
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE)
        return false;
    //定义可变参数列表
    va_list arg_list;
    //将变量arg_list初始化为传入参数
    va_start(arg_list, format);
    //将数据format从可变参数列表写入缓冲区，返回写入数据的长度
    int len = vsnprintf(m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - 1 - m_write_idx, format, arg_list);
    //若写入数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - 1 - m_write_idx))
    {
        va_end(arg_list);
        return false;
    }
    //更新写缓冲区已写内容指针的位置
    m_write_idx += len;
    //情况可变参数列表
    va_end(arg_list);
    //写日志
    LOG_INFO("request:%s", m_write_buf);

    return true;  
}
//添加状态行
bool http_conn::add_status_line(int status, const char *title){
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}
//添加消息报头，具体地：添加响应报文长度、连接状态和空行
bool http_conn::add_headers(int content_len){
    add_content_length(content_len);
    add_linger();
    add_blank_line();
}
//添加响应报文长度
bool http_conn::add_content_length(int content_len){
    return add_response("Content-Length:%d\r\n", content_len);
}
//添加文本类型
bool http_conn::add_content_type(){
    return add_response("Content-Type:%s\r\n", "text/html");
}
//添加连接状态，通知浏览器是保持连接还是关闭
bool http_conn::add_linger(){
    return add_response("Connection:%s\r\n", (m_linger == true) ? "keep-alive" : "close");
}
//添加空行
bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}
//添加响应正文
bool http_conn::add_content(const char *content){
    return add_response("%s", content);
}

//向m_write_buf中写入响应报文
bool http_conn::process_write(HTTP_CODE ret){
    switch (ret)
    {
    case INTERNAL_ERROR:
    {
        add_status_line(500, error_500_title);
        add_headers(strlen(error_500_form));
        if (!add_content(error_500_form))
            return false;
        break;
    }
    case BAD_REQUEST:
    {
        add_status_line(404, error_404_title);
        add_headers(strlen(error_404_form));
        if (!add_content(error_404_form))
            return false;
        break;
    }
    case FORBIDDEN_REQUEST:
    {
        add_status_line(403, error_403_title);
        add_headers(strlen(error_403_form));
        if (!add_content(error_403_form))
            return false;
        break;
    }
    case FILE_REQUEST:
    {
        //注意：有两个iovec
        //m_iv[0]指向m_write_buf，是响应报文头部的内容（状态行/消息报头等）
        //m_iv[1]指向文件地址m_file_address，是http请求资源(文件)的数据
        //iovec指针成员有两个：
        //iov_base指向一个缓冲区，这个缓冲区是存放的是writev将要发送的数据
        //iov_len表示实际写入的长度
        add_status_line(200, ok_200_title);
        if(m_file_stat.st_size != 0){
            add_headers(m_file_stat.st_size);
            m_iv[0].iov_base = m_write_buf;         
            m_iv[0].iov_len = m_write_idx;          
            m_iv[1].iov_base = m_file_address;
            m_iv[1].iov_len = m_file_stat.st_size;
            m_iv_count = 2;
            bytes_to_send = m_write_idx + m_file_stat.st_size;
            return true;
        }
        else
        {
            const char *ok_string = "<html><body></body></html>";
            add_headers(strlen(ok_string));
            if (!add_content(ok_string))
                return false;
        }
    }
    default:
        return false;
    }
    //执行非FILE_REQUEST情况下的写入操作
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;
}

//http处理函数，内部调用process_read和prosess_write
void http_conn::process(){
    //如果成功解析请求报文，read_ret是do_request返回的状态值
    HTTP_CODE read_ret = process_read();    
    //NO_REQUEST表示还没有完成请求报文解析，需要继续接收请求数据
    if(read_ret == NO_REQUEST){
        //将读事件重置为EPOLLONESHOT，否则后续无法再次触发
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode);
        return;
    }
    //调用process_write完成报文响应
    bool write_ret = process_write(read_ret);
    if(!write_ret){
        close_conn();
    }
    //注册并监听写事件，将写事件重置为EPOLLONESHOT，否则后续无法再次触发
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode);
}