#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include <assert.h>
#include "lst_timer.h"


#define MAX_FD 65535            //最大客户端数量
#define MAX_EVENT_NUMBER 10000  //监听的最大的事件数量
#define TIMESLOT 5              //定时器周期

static int pipefd[2];           //管道文件描述符
/*
    函数功能：信号回调函数 向管道写数据
*/
void sig_to_pipe(int sig){
    int save_errno = errno;             //保存error
    int msg = sig;
    send(pipefd[1], (char*)&msg, 1, 0); //写入数据为msg的地址
    errno = save_errno;
}
/*
    函数功能：添加信号捕捉
*/
void addsig(int sig, void(handler)(int)){
    struct sigaction sa; //注册信号的参数
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

/*  函数声明 */
extern void addfd(int epollfd, int fd, bool one_shot, bool et);  //添加文件描述符到epoll中
extern void removefd(int epollfd, int fd);              //从epoll中删除文件描述符
extern void modfd(int epollfd, int fd, int ev);         //修改文件描述符
extern int setnonblocking(int fd);

/*
    函数功能：主函数体
*/
int main(int argc, char * argv[]){

    if(argc <= 1){
        printf("按照如下格式运行：%s port_number\n", basename(argv[0]));
        exit(-1);
    }

    int port = atoi(argv[1]);               //获取端口号 转换为整数

    addsig(SIGPIPE, SIG_IGN);               //对sigpipe信号进行忽略处理
    
    //创建线程池，初始化线程池
    threadpool<http_conn> * pool = NULL;    
    try{
        pool = new threadpool<http_conn>;
    }
    catch(...){
        exit(-1);
    }
    
    http_conn * users = new http_conn[MAX_FD];//创建数组保存所有客户端信息
    
    /****************网络部分代码*********************/
    //创建监听的套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert(listenfd >= 0);
    //设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    int ret = bind(listenfd, (struct sockaddr *)&address, sizeof(address));
    assert(ret != -1);

    //监听
    ret = listen(listenfd, 5);
    assert(ret != -1);

    //创建epoll对象，事件数组(IO多路复用，同时检测多个事件)
    epoll_event events[MAX_EVENT_NUMBER];
    int epollfd = epoll_create(5);
    assert(epollfd != -1);

    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false, false);

    http_conn::m_epollfd = epollfd; //静态成员 类共享

    //创建管道 sig_to_pipe()函数就是通过写入 pipefd[1] 来向管道发送信号数据
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert(ret != -1);
    setnonblocking(pipefd[1]);                  //写管道非阻塞
    addfd(epollfd, pipefd[0], false, false);    //epoll检测读管道

    //设置信号处理函数
    addsig(SIGALRM, sig_to_pipe);   //定时器关闭
    addsig(SIGTERM, sig_to_pipe);   //sigterm关闭服务器
    bool stop_server = false;       //关闭服务器标志位

    bool timeout = false;           //定时器周期到达
    alarm(TIMESLOT);                //定时产生sigalarm信号


    //主线程循环检测事件发生
    while(!stop_server){

        //检测到了num个事件
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num < 0) && (errno != EINTR)){
            printf("epoll failure...\n");
            break;
        }

        //循环遍历事件数组
        for(int i = 0; i < num; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd){
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);

                if(http_conn::m_user_count >= MAX_FD){
                    //目前用户连接数满了
                    //给客户端写一个信息，服务器正忙
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);
                
            }else if(sockfd == pipefd[0] && (events[i].events & EPOLLIN)){
                //判断当前事件的文件描述符为管道读段 后面判断时间为可读事件
                //读管道有数据时，SIGALARM or SIGTERM信号触发
                int sig;
                char signals[1024];
                ret = recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret == -1){
                    continue;
                }else if(ret == 0){
                    continue;
                }else{
                    for(int i = 0; i < ret; i++){
                        switch (signals[i])
                        {
                        case SIGALRM:
                            timeout = true;
                            break;
                        case SIGTERM:
                            stop_server = true;
                        }
                    }
                }
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开 或者 错误事件
                users[sockfd].close_conn();
                //移除用户对应的定时器
                http_conn::m_timer_lst.del_timer(users[sockfd].timer);

            }else if(events[i].events & EPOLLIN){
                //产生读事件
                if(users[sockfd].read()){
                    //一次性把所有数据都读完
                    pool->append(users + sockfd);
                }else{
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);
                }
            }else if(events[i].events & EPOLLOUT){
                if(!users[sockfd].write()){ //一次性写完所有数据
                    users[sockfd].close_conn();
                    http_conn::m_timer_lst.del_timer(users[sockfd].timer);
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。
        // 当然，这样做将导致定时任务不能精准的按照预定的时间执行
        if(timeout){
            //处理定时任务 调用tick()函数
            http_conn::m_timer_lst.tick();
            // 因为一次 alarm 调用只会引起一次SIGALARM 信号
            // 所以我们要重新定时，以不断触发 SIGALARM信号
            alarm(TIMESLOT);
            //重置
            timeout = false;
        }   

    }
    
    close(epollfd);
    close(listenfd);
    close(pipefd[0]);
    close(pipefd[1]);
    delete [] users;
    delete pool;

    return 0;
}