#ifndef __CSocket_H__
#define __CSocket_H__

#include <iostream>
#include <vector>
#include <deque>
#include <list>
#include <atomic>
#include <stddef.h>
#include <memory>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <sys/epoll.h>

#define NGX_LISTEN_BACKLOG  511		//已完成连接队列
#define NGX_MAX_EVENTS		512		//epoll_wait一次最多接受事件数 nginx中缺省512

typedef class	CSocket			CSocket;
typedef struct 	listening_s		listening_t, *lplistening_t;
typedef struct 	connection_s	connection_t,*lpconnection_t;

typedef void (CSocket::*event_handler_pt)(lpconnection_t c); //定义成员函数指针
typedef void (*callback_handler_pt)(lpconnection_t c);

void* work_thread(void* ptr);

class CMemory 
{
private:
	CMemory() {}  //构造函数，因为要做成单例类，所以是私有的构造函数

public:
	~CMemory(){};

private:
	static CMemory *m_instance;

public:	
	static CMemory* GetInstance() //单例
	{			
		if(m_instance == NULL)
		{
			//锁
			if(m_instance == NULL)
			{				
				m_instance = new CMemory();
				static CGarhuishou cl; 
			}
			//放锁
		}
		return m_instance;
	}	
	class CGarhuishou 
	{
	public:				
		~CGarhuishou()
		{
			if (CMemory::m_instance)
			{						
				delete CMemory::m_instance;
				CMemory::m_instance = NULL;				
			}
		}
	};

public:
	void *AllocMemory(int memCount,bool ifmemset)
	{	    
		void *tmpData = (void *)new char[memCount];
    	if(ifmemset) //要求内存清0
    	{
	    	memset(tmpData,0,memCount);
    	}
		return tmpData;
	}

	void FreeMemory(void *point)
	{
		delete [] ((char *)point);
	}
};

class CLock
{
public:
	CLock(pthread_mutex_t *pMutex):m_pMutex(pMutex)
	{
		pthread_mutex_lock(m_pMutex);
	}
	~CLock()
	{
		pthread_mutex_unlock(m_pMutex);
	}

private:
	pthread_mutex_t *m_pMutex;
};

struct listening_s
{
	int            	port;		//监听端口号
	int            	fd;			//socket fd
	lpconnection_t	connection;	//连接池中一个链接
};

struct connection_s
{
	int						fd;				//socket fd
	lplistening_t			listening;		//如果该链接被分配给listen socket lplistening_t

	unsigned				instance:1;		//失效位
	struct sockaddr			s_sockaddr;		//remote addr
	
	uint8_t					r_ready;		//读标记
	uint8_t					w_ready;		//写标记

	event_handler_pt		rhandler;		//读事件处理方法
	event_handler_pt		whandler;		//写事件处理方法

	std::vector<uint8_t>	v_readbuff;		//读缓存
	std::vector<uint8_t>	v_writebuff;	//写缓存

	lpconnection_t			next;			//next指针

	uint32_t				events;			//epoll events

	pthread_mutex_t			procMutex;

	std::thread				*th_worker;

	connection_s()
	{
		pthread_mutex_init(&procMutex,nullptr);
	}

	virtual ~connection_s()
	{
		pthread_mutex_destroy(&procMutex);
	}
	
	void GetOneToUse()
	{
		fd = -1;
		events = 0;
		instance = !instance;

		if(th_worker != nullptr)
		{
			if(th_worker->joinable())
			{
				th_worker->join();
				th_worker = nullptr;
			}
		}
	}

	void PutOneToFree()
	{
		rhandler = nullptr;
		whandler = nullptr;
		v_readbuff.clear();
		v_writebuff.clear();

		if(th_worker != nullptr)
		{
			if(th_worker->joinable())
			{
				th_worker->join();
				th_worker = nullptr;
			}
		}
	}

	std::vector<uint8_t> GetBuffer()
	{
		CLock lock(&procMutex);
		std::vector<uint8_t> res(this->v_readbuff);
		return res;
	}

	void RemoveLeftData(int size)
	{
		CLock lock(&procMutex);
		if(size > v_readbuff.size())
			return;
		v_readbuff.erase(v_readbuff.begin(),v_readbuff.begin()+size);
	}
};

class CSocket
{
public:
    CSocket();
    virtual ~CSocket();

public:
    virtual bool Initialize(int port, callback_handler_pt rhandle, int work_connections = 1024);
	void SetReadCallback(callback_handler_pt cb);
	ssize_t SendMsg(lpconnection_t c,const char* buff,ssize_t len);

private:
	bool open_listening_socket(int port);				//打开监听端口
	void close_listening_socket();						//关闭监听端口
	bool setnonblocking(int sockfd);					//设置非阻塞

	void event_accept(lpconnection_t oldc);
	void close_connection(lpconnection_t c);
	void read_request_handler(lpconnection_t c);
	void write_request_handler(lpconnection_t c);

	ssize_t recvproc(lpconnection_t c,char *buff,ssize_t buflen);  //接收从客户端来的数据专用函数
	void zdCloseSocketConn(lpconnection_t c);

	ssize_t sendproc(lpconnection_t c,const char* buff,ssize_t size);

public:
	int ngx_epoll_init();
	int ngx_epoll_add_event(int fd,int readevent,int writeevent,uint32_t otherflag,uint32_t eventtype,lpconnection_t c);
	int ngx_epoll_oper_event(int fd,uint32_t eventtype,uint32_t flag,int baction,lpconnection_t c);
	int ngx_epoll_process_events(int timer);
	void epoll_process_and_timer(int timer = -1);


	//连接池相关
	void initconnection();
	void clearconnection();
	lpconnection_t get_connection(int isock);
	void free_connection(lpconnection_t c);

private:
	lplistening_t					m_ListenSocket;				//监听套接字
	int								m_epollhandle;				//epollhandler
	int								m_worker_connections;	
	struct epoll_event				m_events[NGX_MAX_EVENTS];	//承载epoll_wait()中返回事件

	callback_handler_pt 			cb_rhandle;					//读回调

	//连接池相关
	std::list<lpconnection_t>		m_connectionList;			//连接池:all connections
	std::list<lpconnection_t>		m_freeConnectionList;		//空闲连接池
	std::atomic<int>				m_total_connection_n;		//连接池总数
	std::atomic<int>				m_free_connection_n;		//连接池空闲链接数
	pthread_mutex_t					m_connectionMutex;          //连接相关互斥量
};
#endif