// rec.cpp v1.4.2 — C++23 Daemon for Live Recording & CLI Control
//   • signalfd + epoll + timerfd 事件驱动
//   • 每房间独立 std::thread 探测
//   • 在线判断：调用 streamlink.session.Streamlink.streams(url)
//     并支持 http-cookies 选项
//   • 录制：fork() + Python C-API 调用 rec(url, cookie, output)
//   • CLI: ls / add / del / hold / exit

#include <Python.h>
#include <sys/signalfd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <syslog.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <atomic>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <format>
#include <thread>
#include <chrono>

static constexpr char CFG_FILE[]  = "config.ini";
static constexpr char SOCK_PATH[] = "/tmp/recorder.sock";
static constexpr int  LINE_BUF    = 1024;
static constexpr int  EPOLL_MAX   = 16;
static constexpr int  DEFAULT_MON = 10;

struct Room {
    std::string id, url, cookie;
    bool        hold = false;
    pid_t       pid  = 0;
    int         interval = 0;
    std::chrono::steady_clock::time_point last_check{};
};

class Daemon {
public:
    Daemon(): next_idx(0) {
        openlog("recorderd", LOG_PID, LOG_DAEMON);
        setlogmask(LOG_UPTO(LOG_DEBUG));
        syslog(LOG_INFO, "recorderd v1.4.2 starting");

        // 初始化 Python 并创建 GIL
        Py_Initialize();
        PyEval_SaveThread();           // 切回无 GIL 状态

        // 定义 Python 函数 is_live 和 rec
        PyGILState_STATE g = PyGILState_Ensure();
        const char *py_init = R"py(
import streamlink
def is_live(url, cookie):
    sess = streamlink.session.Streamlink()
    if cookie:
        sess.set_option("http-cookies", cookie)
    streams = sess.streams(url)
    return bool(streams)

def rec(u, c, o):
    sess = streamlink.session.Streamlink()
    if c:
        sess.set_option("http-cookies", c)
    s = sess.streams(u).get("best") or next(iter(sess.streams(u).values()), None)
    if not s: return
    fd = s.open()
    import os
    os.makedirs(os.path.dirname(o), exist_ok=True)
    with open(o, "wb") as f:
        while True:
            data = fd.read(1024)
            if not data:
                break
            f.write(data)
    fd.close()
)py";
        PyRun_SimpleString(py_init);
        PyGILState_Release(g);

        load_config();
    }

    ~Daemon() {
        syslog(LOG_INFO, "recorderd shutting down");
        cleanup();
        // 终结 Python
        PyGILState_STATE g = PyGILState_Ensure();
        Py_Finalize();
        PyGILState_Release(g);
        closelog();
    }

    void run() {
        if (!rooms.empty() && !rooms[0].hold) {
            schedule_probe(&rooms[0]);
            next_idx = 1;
        }
        init_drivers();
        epoll_loop();
        // 退出前 kill 子进程
        for (auto &r : rooms)
            if (r.pid > 0)
                kill(r.pid, SIGTERM);
    }

    static int client_mode(int argc, char** argv) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (c<0) { perror("socket"); return 1; }
        sockaddr_un ua{ .sun_family = AF_UNIX };
        strncpy(ua.sun_path, SOCK_PATH, sizeof(ua.sun_path)-1);
        if (connect(c,(sockaddr*)&ua,sizeof(ua))<0) {
            perror("connect"); return 1;
        }
        std::string cmd;
        for (int i = 1; i < argc; ++i) {
            cmd += argv[i];
            cmd += ' ';
        }
        write(c, cmd.c_str(), cmd.size());
        char buf[4096]; int n;
        while ((n = read(c, buf, sizeof(buf))) > 0)
            write(1, buf, n);
        close(c);
        return 0;
    }

private:
    std::vector<Room> rooms;
    int               global_mon = DEFAULT_MON;
    std::string       out_dir     = "out";

    int               sig_fd=-1, tim_fd=-1, ctl_fd=-1, ep_fd=-1;
    std::atomic<bool> stop_flag{false};
    size_t            next_idx;

    static std::string trim(const std::string &s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return "";
        size_t e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
    }

    void load_config() {
        std::ifstream f(CFG_FILE);
        if (!f.is_open()) {
            std::ofstream w(CFG_FILE);
            w << "[global]\nmon_interval=" << global_mon
              << "\nout_dir=" << out_dir << "\n\n";
            return;
        }
        rooms.clear();
        std::string sec, line;
        while (std::getline(f, line)) {
            auto s = trim(line);
            if (s.empty() || s[0]=='#' || s[0]==';') continue;
            if (s.front()=='[' && s.back()==']') {
                sec = s.substr(1, s.size()-2);
                if (sec.rfind("room:",0)==0) {
                    rooms.emplace_back();
                    rooms.back().id = sec.substr(5);
                }
                continue;
            }
            auto p = s.find('=');
            if (p == std::string::npos) continue;
            auto key = trim(s.substr(0,p));
            auto val = trim(s.substr(p+1));
            if (sec=="global") {
                if (key=="mon_interval") global_mon = std::stoi(val);
                else if (key=="out_dir") out_dir = val;
            }
            else if (sec.rfind("room:",0)==0) {
                Room &r = rooms.back();
                if (key=="url")           r.url      = val;
                else if (key=="cookie")   r.cookie   = val;
                else if (key=="hold")     r.hold     = (std::stoi(val)!=0);
                else if (key=="interval") r.interval = std::stoi(val);
            }
        }
    }

    void write_config() {
        std::ofstream f(CFG_FILE);
        f << std::format("[global]\nmon_interval={}\nout_dir={}\n\n",
                         global_mon, out_dir);
        for (auto &r : rooms) {
            f << std::format("[room:{}]\n", r.id)
              << std::format("url={}\n", r.url);
            if (!r.cookie.empty())
                f << std::format("cookie={}\n", r.cookie);
            f << std::format("hold={}\n", r.hold?1:0);
            if (r.interval>0)
                f << std::format("interval={}\n", r.interval);
            f << "\n";
        }
    }

    // 获取 __main__ 中可调用函数
    PyObject* get_callable(const char* name) {
        PyObject *m = PyImport_AddModule("__main__");
        if (!m) { PyErr_Print(); return nullptr; }
        PyObject *fn = PyObject_GetAttrString(m, name);
        if (!fn || !PyCallable_Check(fn)) {
            PyErr_Format(PyExc_AttributeError, "no %s", name);
            PyErr_Print();
            Py_XDECREF(fn);
            return nullptr;
        }
        return fn;
    }

    // 异步探测
    void schedule_probe(Room *rp) {
        rp->last_check = std::chrono::steady_clock::now();
        std::thread([this,rp]{
            PyGILState_STATE g = PyGILState_Ensure();
            PyObject *fn = get_callable("is_live");
            bool live = false;
            if (fn) {
                PyObject *args = PyTuple_Pack(2,
                    PyUnicode_FromString(rp->url.c_str()),
                    PyUnicode_FromString(rp->cookie.c_str())
                );
                PyObject *res = PyObject_CallObject(fn, args);
                Py_DECREF(args);
                if (res) {
                    live = PyObject_IsTrue(res);
                    Py_DECREF(res);
                } else {
                    PyErr_Print();
                }
                Py_DECREF(fn);
            }
            PyGILState_Release(g);

            if (live && rp->pid==0 && !rp->hold) {
                syslog(LOG_INFO, "room %s live ➜ spawn", rp->id.c_str());
                spawn_recorder(*rp);
            }
        }).detach();
    }

    // fork + 调用 rec()
    void spawn_recorder(Room &r) {
        pid_t pid = fork();
        if (pid < 0) {
            syslog(LOG_ERR, "fork %s: %s", r.id.c_str(), strerror(errno));
            return;
        }
        if (pid == 0) {
            // 子进程
            Py_Initialize();
            PyGILState_STATE g = PyGILState_Ensure();
            PyObject *fn = get_callable("rec");
            if (fn) {
                char ts[32], outp[512];
                auto now = std::chrono::system_clock::now();
                std::time_t t0 = std::chrono::system_clock::to_time_t(now);
                strftime(ts,sizeof(ts),"%F_%H-%M-%S",localtime(&t0));
                std::snprintf(outp,sizeof(outp),
                              "%s/%s_%s.ts",
                              out_dir.c_str(), r.id.c_str(), ts);
                mkdir(out_dir.c_str(),0755);

                PyObject *args = PyTuple_Pack(3,
                    PyUnicode_FromString(r.url.c_str()),
                    PyUnicode_FromString(r.cookie.c_str()),
                    PyUnicode_FromString(outp)
                );
                PyObject *res = PyObject_CallObject(fn, args);
                Py_DECREF(args);
                if (!res) PyErr_Print();
                else Py_DECREF(res);
                Py_DECREF(fn);
            }
            PyGILState_Release(g);
            Py_Finalize();
            _exit(0);
        }
        r.pid = pid;
        syslog(LOG_INFO, "spawned pid=%d for %s", pid, r.id.c_str());
    }

    void init_drivers() {
        sigset_t mask; sigemptyset(&mask);
        for (int s:{SIGCHLD,SIGHUP,SIGTERM,SIGINT})
            sigaddset(&mask, s);
        pthread_sigmask(SIG_BLOCK, &mask, nullptr);

        sig_fd = signalfd(-1, &mask, SFD_NONBLOCK|SFD_CLOEXEC);
        tim_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        itimerspec it{{1,0},{1,0}};
        timerfd_settime(tim_fd, 0, &it, nullptr);

        unlink(SOCK_PATH);
        ctl_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un ua{ .sun_family=AF_UNIX };
        std::strncpy(ua.sun_path, SOCK_PATH, sizeof(ua.sun_path)-1);
        bind(ctl_fd,(sockaddr*)&ua,sizeof(ua));
        listen(ctl_fd,5); chmod(SOCK_PATH,0666);

        ep_fd = epoll_create1(0);
        for (int fd : {sig_fd, tim_fd, ctl_fd}) {
            epoll_event ev{ .events=EPOLLIN, .data={.fd=fd} };
            epoll_ctl(ep_fd, EPOLL_CTL_ADD, fd, &ev);
        }
    }

    void epoll_loop() {
        std::array<epoll_event,EPOLL_MAX> evs;
        while (!stop_flag.load()) {
            int n = epoll_wait(ep_fd, evs.data(), EPOLL_MAX, -1);
            if (n<0 && errno==EINTR) continue;
            for (int i=0; i<n; ++i) {
                int fd = evs[i].data.fd;
                if (fd==sig_fd)      handle_signal();
                else if (fd==tim_fd) handle_timer();
                else if (fd==ctl_fd) {
                    int c = accept(ctl_fd,nullptr,nullptr);
                    if (c>0) handle_client(c);
                }
            }
        }
    }

    void handle_signal() {
        signalfd_siginfo si;
        if (read(sig_fd,&si,sizeof(si))!=sizeof(si)) return;
        switch (si.ssi_signo) {
        case SIGCHLD:
            while (true) {
                int st; pid_t c = waitpid(-1,&st,WNOHANG);
                if (c<=0) break;
                for (auto &r:rooms) if (r.pid==c) { r.pid=0; break; }
            }
            break;
        case SIGHUP:
            write_config(); load_config();
            break;
        default:
            stop_flag.store(true);
        }
    }

    void handle_timer() {
        uint64_t exp; read(tim_fd,&exp,sizeof(exp));
        auto now = std::chrono::steady_clock::now();
        size_t n = rooms.size();
        for (size_t i=0; i<n; ++i) {
            size_t idx = (next_idx + i) % n;
            Room &r = rooms[idx];
            int iv = r.interval>0? r.interval : global_mon;
            long d = std::chrono::duration_cast<std::chrono::seconds>(
                now - r.last_check).count();
            if (!r.hold && r.pid==0 && d>=iv) {
                next_idx = (idx+1)%n;
                schedule_probe(&r);
                break;
            }
        }
    }

    void handle_client(int cfd) {
        char buf[LINE_BUF]; int n = read(cfd,buf,sizeof(buf)-1);
        if(n<=0){ close(cfd); return; }
        buf[n]=0; std::istringstream iss(buf);
        std::string cmd; iss>>cmd;

        if (cmd=="ls"||cmd.empty()) {
            std::ostringstream oss;
            oss<<std::format(
                "State   | PID    | Interval | ID | Cookie\n"
                "--------+--------+----------+----+--------\n"
            );
            for (auto &r:rooms) {
                oss<<std::format(
                  "{:<7} | {:>6} | {:>8} | {:<3} | {}\n",
                  r.hold?"Hold":"Rec",
                  r.pid,
                  r.interval>0?r.interval:global_mon,
                  r.id,
                  r.cookie
                );
            }
            auto out=oss.str();
            write(cfd,out.c_str(),out.size());
        }
        else if(cmd=="add") {
            std::string id,url,ck; int iv=0;
            iss>>id>>url>>ck>>iv;
            if (id.empty()||url.empty()) {
                const char*u="Usage: add <id> <url> [cookie] [interval]\n";
                write(cfd,u,strlen(u));
            } else {
                bool ex=false; for(auto &r:rooms) if(r.id==id){ex=true;break;}
                if(ex){
                    auto s=std::format("Exists {}\n",id);
                    write(cfd,s.c_str(),s.size());
                } else {
                    Room r{id,url,ck,false,0,iv,{}}; 
                    r.last_check = std::chrono::steady_clock::now();
                    rooms.push_back(r);
                    write_config();
                    if(!r.hold) schedule_probe(&rooms.back());
                    auto s=std::format("Added {}\n",id);
                    write(cfd,s.c_str(),s.size());
                }
            }
        }
        else if(cmd=="del") {
            std::string id; iss>>id;
            for(size_t i=0;i<rooms.size();++i){
                if(rooms[i].id==id){
                    if(rooms[i].pid>0) kill(rooms[i].pid,SIGTERM);
                    rooms.erase(rooms.begin()+i);
                    write_config();
                    auto s=std::format("Deleted {}\n",id);
                    write(cfd,s.c_str(),s.size());
                    break;
                }
            }
        }
        else if(cmd=="hold") {
            std::string id; int f; iss>>id>>f;
            for(auto &r:rooms){
                if(r.id==id){
                    r.hold = f?true:!r.hold;
                    if(r.hold && r.pid>0){ kill(r.pid,SIGTERM); r.pid=0; }
                    write_config();
                    auto s=std::format("Set {} hold={}\n",id,r.hold);
                    write(cfd,s.c_str(),s.size());
                    break;
                }
            }
        }
        else if(cmd=="exit"||cmd=="quit"){
            stop_flag.store(true);
        }
        else {
            const char*u="Unknown. Supported: ls add del hold exit\n";
            write(cfd,u,strlen(u));
        }
        close(cfd);
    }

    void cleanup(){
        close(sig_fd); close(tim_fd); close(ctl_fd); close(ep_fd);
        unlink(SOCK_PATH);
    }
};

int main(int argc, char** argv) {
    if(argc>1)
        return Daemon::client_mode(argc, argv);

    // daemonize
    if(fork()>0) return EXIT_SUCCESS;
    setsid(); umask(0);
    long m=sysconf(_SC_OPEN_MAX);
    for(int fd=3;fd<m;fd++) close(fd);
    freopen("/dev/null","r",stdin);
    freopen("/dev/null","w",stdout);
    freopen("/dev/null","w",stderr);

    Daemon d; d.run();
    return EXIT_SUCCESS;
}
