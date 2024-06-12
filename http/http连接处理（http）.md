# http连接处理（http）

## 一、基础知识

### 1.1 epoll（I/O复用机制）

> 具体可查阅《游双的Linux高性能服务器编程》 第9章 I/O复用

* 头文件：`<sys/epoll.h>`

* `int epoll_create(int size)`：创建一个指示epoll内核事件表的文件描述符，该描述符将用作其他epoll系统调用的第一个参数，size不起作用。

* `int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)`：用于操作内核事件表监控的文件描述符上的事件：注册、修改、删除

  * `epfd`：为`epoll_create`的句柄

  * `op`：表示动作，用3个宏来表示：

  * - `EPOLL_CTL_ADD` (注册新的`fd`到`epfd`)，
    - `EPOLL_CTL_MOD` (修改已经注册的`fd`的监听事件)，
    - `EPOLL_CTL_DEL` (从`epfd`删除一个`fd`)；

  * `event`：告诉内核需要监听的事件，是结构体指针，定义如下

    ```c++
    struct epoll_event {
        __uint32_t events; /* Epoll events */
        epoll_data_t data; /* User data variable */
    };
    ```

  * 其中events描述事件类型，epoll事件类型包含以下几种：

    * `EPOLLIN`：表示对应的文件描述符**可读**（包括对端`socket`正常关闭）
    * `EPOLLOUT`：表示对应的文件描述符**可写**
    * `EPOLLPRI`：表示对应的文件描述符有紧急的数据可读（这里应该表示有带外数据到来）
    * `EPOLLERR`：表示对应的文件描述符发生错误
    * `EPOLLHUP`：表示对应的文件描述符被挂断；
    * `EPOLLET`：将`epoll`设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)而言的
    * `EPOLLONESHOT`：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个`socket`的话，需要再次把这个`socket`加入到`epoll`队列里

* `int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)`：用于等待所监控文件描述符上有事件的产生，返回就绪的文件描述符个数

  * `events`：用来存内核得到事件的集合，

  * `maxevents`：告之内核这个`events`有多大，这个`maxevents`的值不能大于创建`epoll_create()`时的`size`，

  * `timeout`：是超时时间

  * - `-1`：阻塞
    - `0`：立即返回，非阻塞
    - `>0`：指定毫秒

  * 返回值：成功返回有多少文件描述符就绪，时间到时返回`0`，出错返回`-1`

### 1.2 select/poll/epoll

- 调用函数

- - select和poll都是一个函数，epoll是一组函数

- 文件描述符数量

- - select通过线性表描述文件描述符集合，文件描述符有上限，一般是1024，但可以修改源码，重新编译内核，不推荐
  - poll是链表描述，突破了文件描述符上限，最大可以打开文件的数目（65535）
  - epoll通过**红黑树**描述，最大可以打开文件的数目（65535），可以通过命令ulimit -n number修改，仅对当前终端有效

- 将文件描述符从用户传给内核

- - select和poll通过将所有文件描述符拷贝到内核态，**每次调用都需要拷贝**
  - **epoll通过epoll_create建立一棵红黑树，通过epoll_ctl将要监听的文件描述符注册到红黑树上**

- 内核判断就绪的文件描述符

- - select和poll通过遍历文件描述符集合，判断哪个文件描述符上有事件发生
  - epoll_create时，内核除了帮我们在epoll文件系统里建了个红黑树用于存储以后epoll_ctl传来的fd外，还会再建立一个list链表，用于存储准备就绪的事件，当epoll_wait调用时，仅仅观察这个list链表里有没有数据即可。
  - epoll是根据每个fd上面的回调函数(中断函数)判断，只有发生了事件的socket才会主动的去调用 callback函数，其他空闲状态socket则不会，若是就绪事件，插入list

- 应用程序索引就绪文件描述符

- - select/poll只返回发生了事件的文件描述符的个数，若知道是哪个发生了事件，同样需要遍历
  - epoll返回的发生了事件的个数和结构体数组，结构体包含socket的信息，因此直接处理返回的数组即可

- 工作模式

- - select和poll都只能工作在相对`低效的LT模式`下
  - epoll则可以工作在`ET高效模式`，并且epoll还支持`EPOLLONESHOT`事件，该事件能进一步减少可读、可写和异常事件被触发的次数。 

- 应用场景

  - 当所有的fd都是活跃连接，使用epoll，需要建立文件系统，红黑书和链表对于此来说，效率反而不高，不如selece和poll
  - 当监测的fd数目较小，且各个fd都比较活跃，建议使用select或者poll
  - 当监测的fd数目非常大，成千上万，且单位时间只有其中的一部分fd处于就绪状态，这个时候使用epoll能够明显提升性能

### 1.3 HTTP报文格式

* 说明：HTTP报文分为**请求报文**和**响应报文**两种，每种报文必须按照特有格式生成，才能被浏览器端识别。其中，浏览器端向服务器发送的为请求报文，服务器处理后返回给浏览器端的为响应报文。

* 请求报文

  * 组成：请求行（request line）、请求头部（header）、空行（用于区分头部和数据）、请求数据

  * 最常用的两种请求类型：GET、POST

    * <font color='grenn'>GET</font>：请求从服务器检索特定资源。GET请求应该只用于获取数据，并且不应该产生副作用（即不会修改服务器状态，也不会对服务器上的资源产生任何修改和影响）。

      ```http
      GET /562f25980001b1b106000338.jpg HTTP/1.1
      Host:img.mukewang.com
      User-Agent:Mozilla/5.0 (Windows NT 10.0; WOW64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/51.0.2704.106 Safari/537.36
      Accept:image/webp,image/*,*/*;q=0.8
      Referer:http://www.imooc.com/
      Accept-Encoding:gzip, deflate, sdch
      Accept-Language:zh-CN,zh;q=0.8
      空行
      请求数据为空
      ```

    * <font color='grenn'>POST</font>：用于向服务器提交数据进行处理，例如提交表单或上传文件。POST请求可能会导致新资源的创建或现有资源的更新。

      ```http
      POST / HTTP1.1
      Host:www.wrox.com
      User-Agent:Mozilla/4.0 (compatible; MSIE 6.0; Windows NT 5.1; SV1; .NET CLR 2.0.50727; .NET CLR 3.0.04506.648; .NET CLR 3.5.21022)
      Content-Type:application/x-www-form-urlencoded
      Content-Length:40
      Connection: Keep-Alive
      空行
      name=Professional%20Ajax&publisher=Wiley
      ```

    * 说明

      > - **请求行**，用来说明请求类型,要访问的资源以及所使用的HTTP版本。
      >   GET说明请求类型为GET，/562f25980001b1b106000338.jpg(URL)为要访问的资源，该行的最后一部分说明使用的是HTTP1.1版本。
      >- **请求头部**，紧接着请求行（即第一行）之后的部分，用来说明服务器要使用的附加信息。
      > - - HOST，给出请求资源所在服务器的域名。
      >  - User-Agent，HTTP客户端程序的信息，该信息由你发出请求使用的浏览器来定义,并且在每个请求中自动发送等。
      >   - Accept，说明用户代理可处理的媒体类型。
      >   - Accept-Encoding，说明用户代理支持的内容编码。
      >   - Accept-Language，说明用户代理能够处理的自然语言集。
      >   - Content-Type，说明实现主体的媒体类型。
      >   - Content-Length，说明实现主体的大小。
      >   - Connection，连接管理，可以是Keep-Alive或close。
      > - **空行**，请求头部后面的空行是必须的即使第四部分的请求数据为空，也必须有空行。
      > - **请求数据**也叫主体，可以添加任意的其他数据。
  
    * GET和POST请求下的页面跳转流程
  
      ![图片](http连接处理（http）.assets/640.webp)

* 响应报文

  * 组成：状态行、消息报头、空行、响应正文

    ```http
    HTTP/1.1 200 OK
    Date: Fri, 22 May 2009 06:07:21 GMT
    Content-Type: text/html; charset=UTF-8
    空行
    <html>
    	<head></head>
    	<body>
    		<!--body goes here-->
    	</body>
    </html>
    ```

    > - 状态行，由HTTP协议版本号， **状态码**， **状态消息** 三部分组成。
    >   第一行为状态行，（HTTP/1.1）表明HTTP版本为1.1版本，状态码为200，状态消息为OK。
    > - 消息报头，用来说明客户端要使用的一些附加信息。
    >   第二行和第三行为消息报头，Date:生成响应的日期和时间；Content-Type:指定了MIME类型的HTML(text/html),编码类型是UTF-8。
    > - 空行，消息报头后面的空行是必须的。
    > - 响应正文，服务器返回给客户端的文本信息。空行后面的html部分为响应正文。

* HTTP的状态码

  * `1xx`：**指示**信息--表示请求已接收，继续处理。

  * `2xx`：**成功**--表示请求正常处理完毕。
    * `200 OK`：客户端请求被正常处理。
    * `206 Partial content`：客户端进行了范围请求。
  * `3xx`：**重定向**--要完成请求必须进行更进一步的操作。
    * `301 Moved Permanently`：永久重定向，该资源已被永久移动到新位置，将来任何对该资源的访问都要使用本响应返回的若干个URI之一。
    * `302 Found`：临时重定向，请求的资源现在临时从不同的URI中获得。
  * `4xx`：**客户端错误**--请求有语法错误，服务器无法处理请求。
    * `400 Bad Request`：请求报文存在语法错误。
    * `403 Forbidden`：请求被服务器拒绝。
    * `404 Not Found`：请求不存在，服务器上找不到请求的资源。
  * `5xx`：**服务器端错误**--服务器处理请求出错。
    * `500 Internal Server Error`：服务器在执行请求时出现错误。

### 1.4 有限状态机

* 有限状态机，是一种抽象的理论模型，它能够把有限个变量描述的状态变化过程，以可构造可验证的方式呈现出来。**是一种逻辑单元内部的一种高效编程方法**，在服务器编程中，服务器可以根据不同状态或者消息类型进行相应的处理逻辑，使得程序逻辑清晰易懂

* 实现：通过`if-else`，`switch-case`和`函数指针`来实现，主要是为了封装逻辑

* 示例：

  ```C++
  STATE_MACHINE(){
      State cur_State = type_A;
      while(cur_State != type_C){
          Package _pack = getNewPackage();
          switch(){
              case type_A:
                  process_pkg_state_A(_pack);
                  cur_State = type_B;
                  break;
              case type_B:
                  process_pkg_state_B(_pack);
                  cur_State = type_C;
                  break;
          }
      }
  }
  ```

## 二、HTTP处理流程

> 1. 浏览器端发出`http`连接请求，主线程创建`http`对象接收请求并将所有数据读入对应`buffer`，将该对象插入任务队列，工作线程从任务队列中取出一个任务进行处理。
> 2. 工作线程取出任务后，调用`process_read`函数，通过主、从状态机对请求报文进行解析。
> 3. 解析完之后，跳转`do_request`函数生成响应报文，通过`process_write`写入`buffer`，返回给浏览器端

### 2.1 HTTP类定义和服务器读取数据



### 2.2 解析请求报文

![图片](https://mmbiz.qpic.cn/mmbiz_jpg/6OkibcrXVmBH2ZO50WrURwTiaNKTH7tCia3AR4WeKu2EEzSgKibXzG4oa4WaPfGutwBqCJtemia3rc5V1wupvOLFjzQ/640?wx_fmt=jpeg&tp=webp&wxfrom=5&wx_lazy=1&wx_co=1)

### 2.3 生成响应报文

![图片](http连接处理（http）.assets/640-17169098803302.webp)
