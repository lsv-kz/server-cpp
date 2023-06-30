#include "main.h"

using namespace std;

static mutex mtx_conn;
static condition_variable cond_close_conn;
static int num_conn = 0, close_conn = 0;
//======================================================================
RequestManager::RequestManager(unsigned int n)
{
    list_start = list_end = NULL;
    all_req = stop_manager = 0;
    NumProc = n;
}
//----------------------------------------------------------------------
RequestManager::~RequestManager() {}
//----------------------------------------------------------------------
int RequestManager::get_num_proc()
{
    return NumProc;
}
//----------------------------------------------------------------------
void RequestManager::push_resp_list(Connect *req)
{
mtx_list.lock();
    req->next = NULL;
    req->prev = list_end;
    if (list_start)
    {
        list_end->next = req;
        list_end = req;
    }
    else
        list_start = list_end = req;

    ++all_req;
mtx_list.unlock();
    cond_list.notify_one();
}
//----------------------------------------------------------------------
Connect *RequestManager::pop_resp_list()
{
unique_lock<mutex> lk(mtx_list);
    while ((list_start == NULL) && !stop_manager)
    {
        cond_list.wait(lk);
    }

    if (stop_manager)
        return NULL;

    Connect *req = list_start;
    if (list_start->next)
    {
        list_start->next->prev = NULL;
        list_start = list_start->next;
    }
    else
        list_start = list_end = NULL;

    return req;
}
//----------------------------------------------------------------------
void RequestManager::close_manager()
{
    stop_manager = 1;
    cond_list.notify_all();
}
//======================================================================
void start_conn()
{
mtx_conn.lock();
    ++num_conn;
mtx_conn.unlock();
}
//======================================================================
void wait_close_all_conn()
{
    //close_conn = 1;
unique_lock<mutex> lk(mtx_conn);
    while (num_conn > 0)
    {
        cond_close_conn.wait(lk);
    }
}
//======================================================================
static int fd_close_conn;
static int nProc;
//======================================================================
void end_response(Connect *req)
{
    if (req->connKeepAlive == 0 || (req->err < 0) || close_conn)
    { // ----- Close connect -----
        if (req->err <= -RS101)
        {
            req->respStatus = -req->err;
            req->err = -1;
            req->hdrs = "";
            if (send_message(req, NULL) == 1)
                return;
        }

        if (req->operation != READ_REQUEST)
        {
            print_log(req);
        }

        shutdown(req->clientSocket, SHUT_RDWR);
        close(req->clientSocket);
        delete req;
    mtx_conn.lock();
        --num_conn;
    mtx_conn.unlock();
        char ch = nProc;
        if (write(fd_close_conn, &ch, 1) <= 0)
        {
            print_err("<%s:%d> Error write(): %s\n", __func__, __LINE__, strerror(errno));
            exit(1);
        }
        cond_close_conn.notify_all();
    }
    else
    { // ----- KeepAlive -----
    #ifdef TCP_CORK_
        if (conf->TcpCork == 'y')
        {
        #if defined(LINUX_)
            int optval = 0;
            setsockopt(req->clientSocket, SOL_TCP, TCP_CORK, &optval, sizeof(optval));
        #elif defined(FREEBSD_)
            int optval = 0;
            setsockopt(req->clientSocket, IPPROTO_TCP, TCP_NOPUSH, &optval, sizeof(optval));
        #endif
        }
    #endif
        print_log(req);
        req->init();
        req->timeout = conf->TimeoutKeepAlive;
        ++req->numReq;
        req->operation = READ_REQUEST;
        push_pollin_list(req);
    }
}
//======================================================================
int servSock, uxSock;
static RequestManager *ReqMan;
unsigned long allConn = 0;
//======================================================================
void push_resp_list(Connect *r)
{
    ReqMan->push_resp_list(r);
}
//----------------------------------------------------------------------
Connect *pop_resp_list()
{
    return ReqMan->pop_resp_list();
}
//======================================================================
static void signal_handler_child(int sig)
{
    int nProc = ReqMan->get_num_proc();
    if (sig == SIGINT)
    {
        print_err("[%d]<%s:%d> ### SIGINT ### all req: %d\n", nProc, __func__, __LINE__, ReqMan->get_all_request());
    }
    else if (sig == SIGSEGV)
    {
        print_err("[%d]<%s:%d> ### SIGSEGV ###\n", nProc, __func__, __LINE__);
        shutdown(uxSock, SHUT_RDWR);
        close(uxSock);
        exit(1);
    }
    else
        print_err("[%d]<%s:%d> ### SIG=%d ###\n", nProc, __func__, __LINE__, sig);
}
//======================================================================
Connect *create_req();
void set_max_conn(int n);
int set_max_fd(int max_open_fd);
//======================================================================
void manager(int sockServer, int numProc, int unixSock, int to_parent)
{
    uxSock = unixSock;

    ReqMan = new(nothrow) RequestManager(numProc);
    if (!ReqMan)
    {
        print_err("<%s:%d> *********** Exit child %d ***********\n", __func__, __LINE__, numProc);
        close_logs();
        exit(1);
    }

    fd_close_conn = to_parent;
    nProc = numProc;
    servSock = sockServer;
    //------------------------------------------------------------------
    if (signal(SIGINT, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGINT): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGSEGV, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d]<%s:%d> Error signal(SIGSEGV): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }
    //------------------------------------------------------------------
    if (chdir(conf->DocumentRoot.c_str()))
    {
        print_err("[%d]<%s:%d> Error chdir(%s): %s\n", numProc, __func__, __LINE__, conf->DocumentRoot.c_str(), strerror(errno));
        exit(1);
    }
    //----------------------------------------
    thread EventHandler;
    try
    {
        EventHandler = thread(event_handler, numProc);
    }
    catch (...)
    {
        print_err("[%d]<%s:%d> Error create thread(send_file_): errno=%d\n", numProc, __func__, __LINE__, errno);
        exit(errno);
    }
    //------------------------------------------------------------------
    unsigned int n = 0;
    while (n < conf->NumThreads)
    {
        thread thr;
        try
        {
            thr = thread(response1, numProc);
        }
        catch (...)
        {
            print_err("[%d]<%s:%d> Error create thread: errno=%d\n", numProc, __func__, __LINE__, errno);
            exit(errno);
        }

        thr.detach();
        ++n;
    }
    //------------------------------------------------------------------
    printf("[%d] +++++ num threads=%d, pid=%d, uid=%d, gid=%d  +++++\n", numProc,
                            n, getpid(), getuid(), getgid());
    //------------------------------------------------------------------
    int run = 1;
    unsigned char ch = numProc;
    if (write(to_parent, &ch, sizeof(ch)) < 0)
    {
        print_err("[%d]<%s:%d> Error write(): %s\n", numProc, __func__, __LINE__, strerror(errno));
        run = 0;
    }

    while (run)
    {
        struct sockaddr_storage clientAddr;
        socklen_t addrSize = sizeof(struct sockaddr_storage);
        char data[1] = "";
        int sz = sizeof(data);

        int clientSocket = recv_fd(unixSock, numProc, data, (int*)&sz);
        if (clientSocket < 0)
        {
            print_err("[%d]<%s:%d> Error recv_fd()\n", numProc, __func__, __LINE__);
            break;
        }

        Connect *req;
        req = create_req();
        if (!req)
        {
            shutdown(clientSocket, SHUT_RDWR);
            close(clientSocket);
            break;
        }

        int opt = 1;
        ioctl(clientSocket, FIONBIO, &opt);

        req->init();
        req->numProc = numProc;
        req->numConn = ++allConn;
        req->numReq = 1;
        req->clientSocket = clientSocket;
        req->timeout = conf->Timeout;
        req->remoteAddr[0] = '\0';
        getpeername(clientSocket,(struct sockaddr *)&clientAddr, &addrSize);
        
        int err;
        if ((err = getnameinfo((struct sockaddr *)&clientAddr,
                addrSize,
                req->remoteAddr,
                sizeof(req->remoteAddr),
                req->remotePort,
                sizeof(req->remotePort),
                NI_NUMERICHOST | NI_NUMERICSERV)))
        {
            print_err(req, "<%s:%d> Error getnameinfo()=%d: %s\n", __func__, __LINE__, err, gai_strerror(err));
            req->remoteAddr[0] = 0;
        }

        req->operation = READ_REQUEST;

        start_conn();
        push_pollin_list(req);// --- First request ---
    }

    wait_close_all_conn();

    print_err("[%d]<%s:%d> all_req=%u; open_conn=%d\n", numProc,
                    __func__, __LINE__, ReqMan->get_all_request(), num_conn);

    close_event_handler();
    EventHandler.join();

    ReqMan->close_manager();

    usleep(100000);
    delete ReqMan;
}
//======================================================================
Connect *create_req()
{
    Connect *req = new(nothrow) Connect;
    if (!req)
        print_err("<%s:%d> Error malloc(): %s\n", __func__, __LINE__, strerror(errno));
    return req;
}
