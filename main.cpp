#include <iostream>
#include <thread>
#include <chrono>
#include "CSocket.h"

static void rhandle(lpconnection_t c)
{
    std::cout<<"call back"<<std::endl;
    auto v = c->GetBuffer();
    std::cout<<c->fd<<"    "<<std::string(v.begin(),v.end())<<std::endl;




    
    c->RemoveLeftData(v.size());
    //std::this_thread::sleep_for(std::chrono::seconds(10));
}

int main()
{
    CSocket cs;
    int port = 7892;
    cs.Initialize(port,&rhandle,3);
    cs.SetReadCallback(&rhandle);

    cs.ngx_epoll_init();
    while(true)
    {
        cs.epoll_process_and_timer();
    }
    return 0;
}