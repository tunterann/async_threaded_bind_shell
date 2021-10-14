#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/un.h>
#include <sys/event.h>
#include <array>
#include <err.h>
#include <thread>
#include <future>
#include <type_traits>
#include <sys/wait.h>
#include <util.h>

#define TCP_PORT (int)3221
#define THR_LIM	 (int)5
#define BUFF_LIM (int)1024

// how to test:
// 1. compile and start this program
// 2. open new terminal and execute:
// socat STDIN,echo=0,raw tcp-connect:127.0.0.1:3221
// or
// nc 127.0.0.1 3221

int num = 0; // here will be threads counter
extern char** environ;

void sig_child(int signo)
{
// hook CTRL+C, exit(), logout and any other signals
    int status;
    pid_t pid = wait(&status);
    printf("child %d terminated.\n", pid);
    printf("Threads avaliable: %d\n", num--);
    exit(0);
}


void * socketThread(int sockfd) {
    char ptsname[32] = {0}; // there will be device address of /dev filesystem, for example /dev/ttyS01 or smth like this
    int master = -1; // here will be file descriptor (socket?) for our pty

    pid_t pid = forkpty(&master, ptsname, NULL, NULL); // creates fork with pseudo-tty
    // stack overflow:

    //  "tty" originally meant "teletype" and "pty" means "pseudo-teletype".
    //  In UNIX, /dev/tty* is any device that acts like a "teletype", ie, a terminal.
    //  (Called teletype because that's what we had for terminals in those benighted days.)
    //  A pty is a pseudotty, a device entry that acts like a terminal to the process reading and writing there,
    //  but is managed by something else. They first appeared (as I recall) for X Window and screen and the like,
    //  where you needed something that acted like a terminal but could be used from another program.
    if (pid == 0) { // execute this code only in forked child, not in parent!
        signal(SIGCHLD, sig_child); // attach our hook function
        environ[0] = "TERM=xterm-256color"; // set env for specify type of terminal
        execlp(getenv("SHELL"), "-i", (char *)0); // create process with given binary path (/bin/bash for example)
        for (int i = 0; i < 3; i++)
            dup2(sockfd, i); // redirect STDIN, STDOUT and STDERR to our socket
    }

    printf("sockfd = %d, master = %d, pty = %d\n", sockfd, master, pid);

    printf("Connection accepted!\n");
    int nb;
    struct kevent ev[2], events[5];
    unsigned char buf[BUFF_LIM];


    int epfd = kqueue();
    EV_SET( & ev[0], sockfd, EVFILT_READ, EV_ADD | EV_ENABLE, 0,
            0, 0); // add monitoring for sockfd
    EV_SET( & ev[1], master, EVFILT_READ, EV_ADD | EV_ENABLE, 0,
            0, 0); // add same monitoring for master

    for(;;)
    {
        int nfds = kevent(epfd, ev, 2, events, 2, NULL); // list with new events.
        // Function return something only if there will be new events

        for (int i = 0; i < nfds; i++) {

            if (events[i].flags & EV_EOF) { // check, if it's some kind of disconnect
                printf("Disconnect\n");
                printf("Exit socketThread \n");
                num--;
                printf("Threads = %d\n", num);
                goto safe_exit;
            }

            if (events[i].ident == sockfd) {  // read from fd from USER input and write to fd of PTY
                nb = read(sockfd, buf, BUFF_LIM);
                if(!nb){
                    printf("exit sockfd");
                    goto safe_exit;
                }
                write(master, buf, nb);
            } else if (events[i].ident == master) { // read from fd from PTY input and write back to USER fd
                nb = read(master, buf, BUFF_LIM);
                if(!nb){
                    printf("exit master");
                    goto safe_exit;
                }
                write(sockfd, buf, nb);
            }
        }
    }

    safe_exit:
        close(sockfd);
        close(master);
        close(epfd);
        pthread_exit(NULL);
}


int main(int argc, char** argv) {
    // create socket first
    sockaddr_in sddr, from;
    sockaddr *ptr, *from_ptr;
    socklen_t size;

    constexpr socklen_t sddr_size = sizeof(sockaddr_in);

    ptr = reinterpret_cast<sockaddr *>(&sddr);
    sddr.sin_addr.s_addr = INADDR_ANY;
    sddr.sin_port = htons(TCP_PORT);
    sddr.sin_family = AF_INET;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock >= 0);
    int flags = fcntl(sock, F_GETFL);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    assert(bind(sock, ptr, sddr_size) == 0);
    assert(listen(sock, 5) == 0);

    int on = 1;
    assert(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) == 0);
    from_ptr = reinterpret_cast<sockaddr *>(&from);

    for (;;) {
        int sockfd = accept(sock, from_ptr, &size); // because of NONBLOCK flag this function dosn't block our code
        // and every time it executes, every time you get result
        if ((sockfd != -1 && sockfd < 0) || num >= THR_LIM){ // check if it's user's socket fd and we have enough
            // available threads
//                printf("Threads limit reached!\n");
            if (sock >= 0) {
                shutdown(sock, SHUT_RDWR);
                close(sock);
            }
            continue;
        }
        if (sockfd > 0) {
            num++;
            printf("New client connected! Threads count: %d\n", num);
            std::thread(socketThread, sockfd).detach(); // create thread for this connection
        }
    }
//    }
}
