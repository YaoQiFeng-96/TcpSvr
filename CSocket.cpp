#include "CSocket.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <thread>
#include <chrono>

CMemory *CMemory::m_instance = nullptr;

CSocket::CSocket():
    m_ListenSocket(nullptr),m_epollhandle(-1),m_worker_connections(1024),
    m_total_connection_n(0),m_free_connection_n(0)
{
    //std::cout<<__func__<<std::endl;
    CMemory::GetInstance();
}

CSocket::~CSocket()
{
    //std::cout<<__func__<<std::endl;

    close_listening_socket();
    if(m_ListenSocket)
    {
        delete m_ListenSocket;
    }
    clearconnection();
}

bool CSocket::Initialize(int port, callback_handler_pt rhandle, int work_connections)
{
    //std::cout<<__func__<<std::endl;
    m_worker_connections = work_connections;
    cb_rhandle = rhandle;
    bool res =  open_listening_socket(port);
    return res;
}

void CSocket::SetReadCallback(callback_handler_pt cb)
{
    cb_rhandle = cb;
}

ssize_t CSocket::SendMsg(lpconnection_t c,const char* buff,ssize_t len)
{

}

bool CSocket::open_listening_socket(int port)
{
    //std::cout<<__func__<<std::endl;

    int                isock;
    struct sockaddr_in serv_addr;
    int                iport;

    memset(&serv_addr,0,sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    isock = socket(AF_INET,SOCK_STREAM,0);
    if(isock == -1)
    {
        return false;
    }

    int reuseaddr = 1;
    if(setsockopt(isock,SOL_SOCKET, SO_REUSEADDR,(const void *) &reuseaddr, sizeof(reuseaddr)) == -1)
    {
        close(isock);                                           
        return false;
    }

    if(setnonblocking(isock) == false)
    {
        close(isock);
        return false;
    }

    iport = port;
    serv_addr.sin_port = htons((in_port_t)iport);

    if(bind(isock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) == -1)
    {
        close(isock);
        return false;
    }
        
    if(listen(isock,NGX_LISTEN_BACKLOG) == -1)
    {
        close(isock);
        return false;
    }

    m_ListenSocket = new listening_t;
    memset(m_ListenSocket,0,sizeof(listening_t));
    m_ListenSocket->port = iport;
    m_ListenSocket->fd   = isock;
    std::cout<<"start listen "<<iport<<std::endl;

    return true;
}

void CSocket::close_listening_socket()
{
    std::cout<<__func__<<std::endl;

    close(m_ListenSocket->fd);
}

bool CSocket::setnonblocking(int sockfd)
{
    //std::cout<<__func__<<std::endl;

    int nb=1; //0：清除，1：设置  
    if(ioctl(sockfd, FIONBIO, &nb) == -1) //FIONBIO：设置/清除非阻塞I/O标记：0：清除，1：设置
    {
        return false;
    }
    return true;
}

void CSocket::event_accept(lpconnection_t oldc)
{
    //std::cout<<__func__<<std::endl;

    //监听套接字为LT模式  此处可以一次accept一个

    struct sockaddr     mysockaddr;
    socklen_t           socklen;
    int                 err;
    int                 s = -1;
    static int          use_accept4 = 1;
    lpconnection_t      newc;

    socklen = sizeof(mysockaddr);
    do
    {
        if(use_accept4)
        {
            s = accept4(oldc->fd,&mysockaddr,&socklen,SOCK_NONBLOCK);
        }
        else
        {
            s = accept(oldc->fd,&mysockaddr,&socklen);
        }
        
        if(s == -1)
        {
            err = errno;
            if(err == EAGAIN)
            {
                //ignore
                return;
            }
           
           if(err == ECONNABORTED)
            {
                //close by client?   server:ignore
                return;
            }
            else if(err == EMFILE || err == ENFILE)
            {
                //EMFILE:进程的fd已用尽【已达到系统所允许单一进程所能打开的文件/套接字总数】。可参考：https://blog.csdn.net/sdn_prc/article/details/28661661   以及 https://bbs.csdn.net/topics/390592927
                //ulimit -n ,看看文件描述符限制,如果是1024的话，需要改大;  打开的文件句柄数过多 ,把系统的fd软限制和硬限制都抬高.
                //ENFILE这个errno的存在，表明一定存在system-wide的resource limits，而不仅仅有process-specific的resource limits。按照常识，process-specific的resource limits，一定受限于system-wide的resource limits。
                std::cout<<"fd resource limits"<<std::endl;
                return;
            }
            
            if(err == ENOSYS && use_accept4)
            {
                //系统未实现 accetp4
                use_accept4 = 0;
                continue;
            }

            if(err == ECONNABORTED)
            {
                //do nothing
            }

            if(err == EMFILE || err == ENFILE)
            {
                //do nothing
            }
            return;
        }
        
        newc = get_connection(s);
        if(newc == nullptr)
        {
            if(close(s) == -1)
            {
                std::cout<<"event_accept()中close("<<s<<")失败!"<<std::endl;
            }
            return;
        }

        memcpy(&newc->s_sockaddr,&mysockaddr,socklen);

        if(!use_accept4)
        {
            if(setnonblocking(s) == false)
            {
                close_connection(newc);
                return;
            }
        }

        newc->listening = oldc->listening;
        newc->r_ready = 1;

        newc->rhandler = &CSocket::read_request_handler;
        newc->whandler = &CSocket::write_request_handler;

        if(ngx_epoll_oper_event(
                                s,
                                EPOLL_CTL_ADD,
                                EPOLLIN|EPOLLRDHUP,
                                0,
                                newc
                                ) == -1)
        {
            close_connection(newc);
            return;
        }
        break;
    } while (1);
    return;
}

void CSocket::close_connection(lpconnection_t c)
{
    std::cout<<__func__<<std::endl;

    if(close(c->fd) == -1)
    {
        std::cout<<"close_connection()中close("<<c->fd<<")失败."<<std::endl;
    }
    c->fd = -1;
    free_connection(c);
    return;
}

void CSocket::read_request_handler(lpconnection_t c)
{
    //std::cout<<__func__<<std::endl;

    if(c->th_worker != nullptr)
    {
        if(c->th_worker->joinable())
        {
            c->th_worker->join();
            c->th_worker = nullptr;
        }
    }

    
    auto e = c->events;
    e &= ~EPOLLONESHOT;
    ngx_epoll_oper_event(c->fd,EPOLL_CTL_MOD,EPOLLONESHOT,2,c);
    char buff[4096];
    ssize_t iread = this->recvproc(c,buff,4096);
    if(iread <= 0)
        return;
    std::vector<uint8_t> v{buff,buff+iread};
    c->v_readbuff.insert(c->v_readbuff.end(),v.begin(),v.end());
    
    c->th_worker = new std::thread([=](){
        try
        {
            if(cb_rhandle != nullptr)
            {
                cb_rhandle(c);
            }
        }
        catch(const std::exception& e)
        {
            //std::cerr << e.what() << '\n';
        }
        
        ngx_epoll_oper_event(c->fd,EPOLL_CTL_MOD,e,2,c);
    });
    //ngx_epoll_oper_event(c->fd,EPOLL_CTL_MOD,EPOLLOUT,0,c);
}

void CSocket::write_request_handler(lpconnection_t c)
{
    std::cout<<__func__<<std::endl;

    ngx_epoll_oper_event(c->fd,EPOLL_CTL_MOD,EPOLLOUT,1,c);
}

ssize_t CSocket::recvproc(lpconnection_t c,char *buff,ssize_t buflen)
{
    ssize_t n;
    n = recv(c->fd,buff,buflen,0);
    if(n == 0)
    {
        //客户端关闭
        zdCloseSocketConn(c);
        return -1;
    }
    if(n < 0)
    {
        int err = errno;
        //不算error
        if(err == EAGAIN || err == EWOULDBLOCK)
        {
            //LT模式不应该出现
            return -1;
        }
        if(err == EINTR)
        {
            //LT模式不应该出现
            return -1;
        }

        //异常
        if(err == ECONNRESET)
        {
            //客户端未正常关闭链接
        }
        else
        {
            if(err == EBADE)
            {
                //多线程时偶尔关闭socket？do nothing
            }
        }
        zdCloseSocketConn(c);
        return -1;
    }
    return n;
}

ssize_t CSocket::sendproc(lpconnection_t c,const char* buff,ssize_t size)
{
    ssize_t n;
    for(;;)
    {
        n = send(c->fd,buff,size,0);
        if(n > 0)
        {
            return n;
        }
        if(n == 0)
        {
            return 0;
        }
        if(errno == EAGAIN)
        {
            //内核缓冲区满
            return -1;
        }
        if(errno == EINTR)
        {
            //cause by sig?
        }
        else
        {
            return -2;
        }
    }
}

void CSocket::zdCloseSocketConn(lpconnection_t c)
{
    if(c->fd != -1)
    {
        close(c->fd);
        c->fd = -1;
    }
    free_connection(c);
}

int CSocket::ngx_epoll_init()
{
    //std::cout<<__func__<<std::endl;

    m_epollhandle = epoll_create(m_worker_connections);
    if(m_epollhandle == -1)
    {
        std::cout<<"create epollhandler err."<<std::endl;
        exit(2);
    }

    initconnection();

    lpconnection_t c = get_connection(m_ListenSocket->fd);
    if(c == nullptr)
    {
        std::cout<<"ngx_epoll_init()中ngx_get_connection()失败."<<std::endl;
        exit(2);
    }
    c->listening = m_ListenSocket;
    m_ListenSocket->connection = c;

    c->rhandler = &CSocket::event_accept;

    if(ngx_epoll_oper_event(
                            c->fd,
                            EPOLL_CTL_ADD,
                            EPOLLIN|EPOLLRDHUP,
                            0,
                            c
                            ) == -1)
    {
        exit(2);
    }
    return 1;
}

int CSocket::ngx_epoll_add_event(int fd,int readevent,int writeevent,uint32_t otherflag,uint32_t eventtype,lpconnection_t c)
{
    std::cout<<__func__<<std::endl;

    struct epoll_event ev;
    memset(&ev,0,sizeof(ev));

    if(readevent == 1)
    {
        ev.events = EPOLLIN|EPOLLRDHUP;
    }
    else
    {
        //later
    }

    if(otherflag != 0)
    {
        ev.events |= otherflag;
    }

    ev.data.ptr = (void *)((uintptr_t)c | c->instance);

    if(epoll_ctl(m_epollhandle,eventtype,fd,&ev) == -1)
    {
        std::cout<<"ngx_epoll_add_event()中epoll_ctl()失败"<<std::endl;
        return -1;
    }
    return 1;
}

int CSocket::ngx_epoll_oper_event(
                        int fd,                 //socket fd
                        uint32_t eventtype,     //EPOLL_CTL_ADD，EPOLL_CTL_MOD，EPOLL_CTL_DEL
                        uint32_t flag,          //标志，具体含义取决于eventtype
                        int baction,            //补充动作，用于补充flag标记的不足  :  0：增加   1：去掉 2：完全覆盖 ,eventtype是EPOLL_CTL_MOD时这个参数就有用
                        lpconnection_t c        //链接
                        )
{
    //std::cout<<__func__<<std::endl;

    struct epoll_event ev;
    memset(&ev,0,sizeof(ev));

    if(eventtype == EPOLL_CTL_ADD)
    {
        ev.events = flag;
        c->events = flag;
    }
    else if(eventtype == EPOLL_CTL_MOD)
    {
        ev.events = c->events;
        if(baction == 0)
        {
            ev.events |= flag;
        }
        else if(baction == 1)
        {
            ev.events &= ~flag;
        }
        else if(baction == 2)
        {
            ev.events = flag;
        }
        c->events = ev.events;
    }
    else if(eventtype == EPOLL_CTL_DEL)
    {
        //链接会直接移除 暂不实现
        return 1;
    }

    //std::cout<<c->events<<std::endl;

    ev.data.ptr = (void *)((uintptr_t)c | c->instance);

    if(epoll_ctl(m_epollhandle,eventtype,fd,&ev) == -1)
    {
        std::cout<<"ngx_epoll_oper_event()中epoll_ctl()失败."<<std::endl;
        return -1;
    }
    return 1;
}

int CSocket::ngx_epoll_process_events(int timer)
{
    //std::cout<<__func__<<std::endl;

    int err;
    int events = epoll_wait(m_epollhandle,m_events,NGX_MAX_EVENTS,timer);

    if(events == -1)
    {
        err == errno;
        if(err == EINTR)
        {
            //信号导致
            return 1;
        }
        else
        {
            //something err
            return 0;
        }
    }

    if(events == 0)
    {
        if(timer != -1)
        {
            //阻塞时间到
            return 1;
        }
        //非阻塞 something err
        return 0;
    }

    //有事件到
    lpconnection_t c;
    uintptr_t instance;
    uint32_t revents;
    for(int i = 0; i < events; ++i)
    {
        c = (lpconnection_t)(m_events[i].data.ptr);
        instance = (uintptr_t)c&1;
        c = (lpconnection_t)((uintptr_t)c&(uintptr_t)~1);

        if(c->fd == -1)
        {
            //过期事件
            continue;
        }
        if(c->instance != instance)
        {
            //过期事件
            continue;
        }

        revents = m_events[i].events;
        // if(revents & (EPOLLERR|EPOLLHUP))
        // {
        //     //if the error events were returned, add EPOLLIN and EPOLLOUT，to handle the events at least in one active handler
        //     revents |= EPOLLIN|EPOLLOUT;
        // }

        if(revents & EPOLLIN)
        {
            //ngx_epoll_oper_event(c->fd,EPOLL_CTL_MOD,EPOLLONESHOT,0,c);
            (this->*(c->rhandler))(c);
        }

        
        if(revents & EPOLLOUT)
        {
            if(revents & (EPOLLERR | EPOLLHUP | EPOLLRDHUP))
            {
                //close by client
            }
            else
            {
                (this->*(c->whandler))(c);
            }
        }
        
    }
    return 1;
}

void CSocket::epoll_process_and_timer(int timer)
{
    ngx_epoll_process_events(timer);
}

void CSocket::initconnection()
{
    lpconnection_t pConn;
    CMemory *pMemory = CMemory::GetInstance();
    int ilenconnpool = sizeof(connection_t);
    for(int i = 0; i < m_worker_connections; ++i)
    {
        pConn = (lpconnection_t)pMemory->AllocMemory(ilenconnpool,true);
        pConn = new(pConn)connection_t();
        pConn->GetOneToUse();
        m_connectionList.push_back(pConn);
        m_freeConnectionList.push_back(pConn);
    }
    m_free_connection_n = m_total_connection_n = m_connectionList.size();
    return;
}

void CSocket::clearconnection()
{
    lpconnection_t pConn;
    CMemory *pMemory = CMemory::GetInstance();
    while(!m_connectionList.empty())
    {
        pConn = m_connectionList.front();
        m_connectionList.pop_front();
        pConn->~connection_t();
        pMemory->FreeMemory(pConn);
    }
}

lpconnection_t CSocket::get_connection(int isock)
{
    //std::cout<<__func__<<std::endl;

    CLock lock(&m_connectionMutex);
    if(!m_freeConnectionList.empty())
    {
        // std::cout<<"!m_freeConnectionList.empty()"<<std::endl;
        // std::cout<<"m_connectionList.size() = " <<m_connectionList.size()<<std::endl;
        // std::cout<<"m_freeConnectionList.size() = " <<m_freeConnectionList.size()<<std::endl;
        // std::cout<<"m_total_connection_n = " <<m_total_connection_n<<std::endl;
        // std::cout<<"m_free_connection_n = " <<m_free_connection_n<<std::endl;

        lpconnection_t c = m_freeConnectionList.front();
        m_freeConnectionList.pop_front();
        c->GetOneToUse();
        --m_free_connection_n;
        c->fd = isock;

        // std::cout<<"m_connectionList.size() = " <<m_connectionList.size()<<std::endl;
        // std::cout<<"m_freeConnectionList.size() = " <<m_freeConnectionList.size()<<std::endl;
        // std::cout<<"m_total_connection_n = " <<m_total_connection_n<<std::endl;
        // std::cout<<"m_free_connection_n = " <<m_free_connection_n<<std::endl;
        // std::cout<<"-------------------------------------------------------"<<std::endl;

        return c;
    }

    //freeConnectionList is empty
    CMemory *pMemory = CMemory::GetInstance();
    lpconnection_t c = (lpconnection_t)pMemory->AllocMemory(sizeof(connection_t),true);
    c = new(c)connection_t();
    c->GetOneToUse();
    m_connectionList.push_back(c);
    ++m_total_connection_n;
    c->fd = isock;

    // std::cout<<"m_connectionList.size() = " <<m_connectionList.size()<<std::endl;
    // std::cout<<"m_freeConnectionList.size() = " <<m_freeConnectionList.size()<<std::endl;
    // std::cout<<"m_total_connection_n = " <<m_total_connection_n<<std::endl;
    // std::cout<<"m_free_connection_n = " <<m_free_connection_n<<std::endl;
    // std::cout<<"-------------------------------------------------------"<<std::endl;

    return c;
}

void CSocket::free_connection(lpconnection_t c)
{
    //std::cout<<__func__<<std::endl;

    CLock lock(&m_connectionMutex);
    c->PutOneToFree();
    m_freeConnectionList.push_back(c);
    ++m_free_connection_n;

    // std::cout<<"m_connectionList.size() = " <<m_connectionList.size()<<std::endl;
    // std::cout<<"m_freeConnectionList.size() = " <<m_freeConnectionList.size()<<std::endl;
    // std::cout<<"m_total_connection_n = " <<m_total_connection_n<<std::endl;
    // std::cout<<"m_free_connection_n = " <<m_free_connection_n<<std::endl;
    // std::cout<<"-------------------------------------------------------"<<std::endl;

    return;
}