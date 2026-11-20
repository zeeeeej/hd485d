#include "maintenance.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#define IN_CAP 4096u
#define OUT_CAP 65536u
struct pty_session {
    int fd, state, exitcode, eof;
    pid_t pid, sid, foreground;
    uint32_t id;
    int64_t last_seen, kill_at;
    unsigned char *input, *output;
    size_t in_len, out_len;
    uint64_t in_next, out_base, out_sent;
};
static void signal_jobs(struct pty_session *p, int sig) {
    if (p->sid <= 0) return;
    if (p->fd >= 0) { pid_t fg=tcgetpgrp(p->fd); if(fg>0) p->foreground=fg; }
    if(p->foreground>0 && p->foreground!=p->sid) kill(-p->foreground,sig);
    kill(-p->sid,sig);
#ifdef __linux__
    /* Includes stopped/background jobs in this terminal session. Deliberate
     * daemonization into another session is outside the interactive contract. */
    DIR *dir=opendir("/proc");
    if(dir) {
        struct dirent *e;
        while((e=readdir(dir))) {
            char *end; long pid=strtol(e->d_name,&end,10);
            if(!*end && pid>1 && pid!=getpid() && getsid((pid_t)pid)==p->sid)
                kill((pid_t)pid,sig);
        }
        closedir(dir);
    }
#endif
}
static void release_buffers(struct pty_session *p) {
    free(p->input); free(p->output); p->input=p->output=NULL;
    p->in_len=p->out_len=0;
}
int maint_pty_busy(maintenance *m) {
    struct pty_session *p=m->pty;
    return p && (p->state==1 || p->pid>0 || p->kill_at || p->out_len);
}
void maint_pty_stop(maintenance *m, int reason) {
    struct pty_session *p=m->pty;
    if(!p) return;
    if(p->state==1) {
        p->state=reason;
        signal_jobs(p,SIGTERM);
        p->kill_at=now_ms()+500;
    }
    if(p->fd>=0) close(p->fd);
    p->fd=-1; p->eof=1;
    release_buffers(p);
}
void maint_pty_tick(maintenance *m) {
    struct pty_session *p=m->pty;
    if(!p) return;
    if((p->state==1 || p->out_len) && now_ms()-p->last_seen>=m->pty_timeout_ms)
        maint_pty_stop(m,4);
    if(p->kill_at && now_ms()>=p->kill_at) {
        signal_jobs(p,SIGKILL); p->kill_at=0;
    }
    if(p->fd>=0) {
        pid_t fg=tcgetpgrp(p->fd); if(fg>0) p->foreground=fg;
        if(p->in_len) {
            ssize_t n=write(p->fd,p->input,p->in_len);
            if(n>0) {p->in_len-=(size_t)n;memmove(p->input,p->input+n,p->in_len);}
            else if(n<0 && errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK)
                maint_pty_stop(m,3);
        }
        for(int i=0;i<8 && p->fd>=0 && p->out_len<OUT_CAP;i++) {
            size_t want=OUT_CAP-p->out_len;if(want>4096)want=4096;
            ssize_t n=read(p->fd,p->output+p->out_len,want);
            if(n>0) {p->out_len+=(size_t)n;continue;}
            if(!n || (n<0 && errno==EIO)) {
                close(p->fd);p->fd=-1;p->eof=1;break;
            }
            if(errno!=EINTR && errno!=EAGAIN && errno!=EWOULDBLOCK) maint_pty_stop(m,3);
            break;
        }
    }
    if(p->pid>0) {
        int status=0;
        /* Keep the leader unreaped during graceful shutdown: prevents reuse
         * of its session ID before the final group cleanup. */
        if(!p->kill_at && waitpid(p->pid,&status,WNOHANG)==p->pid) {
            signal_jobs(p,SIGKILL);
            p->pid=0;p->sid=0;p->foreground=0;
            p->exitcode=WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
            if(p->state==1)p->state=2;
        }
    }
    if(p->state!=1 && !p->pid && !p->out_len && p->eof) release_buffers(p);
}
static int start(maintenance *m,const unsigned char *data,size_t n) {
    if(n!=4)return EINVAL;
    unsigned rows=rd16(data), cols=rd16(data+2);
    if(!rows || !cols || rows>1000 || cols>1000)return EINVAL;
    if(maint_pty_busy(m) || m->pid>0 || m->cwd_pipe>=0 || m->file>=0 || m->listing_active)
        return EBUSY;
    if(m->pty_sequence==UINT32_MAX)return EOVERFLOW;
    struct pty_session *p=calloc(1,sizeof(*p));
    if(!p)return ENOMEM;
    p->fd=-1;p->exitcode=-1;
    p->input=malloc(IN_CAP);p->output=malloc(OUT_CAP);
    if(!p->input || !p->output)goto fail;
    p->fd=posix_openpt(O_RDWR|O_NOCTTY|O_CLOEXEC);
    if(p->fd<0 || grantpt(p->fd) || unlockpt(p->fd))goto fail;
    char *name=ptsname(p->fd);
    if(!name)goto fail;
    int slave=open(name,O_RDWR|O_NOCTTY|O_CLOEXEC);
    if(slave<0)goto fail;
    struct winsize ws={.ws_row=(unsigned short)rows,.ws_col=(unsigned short)cols};
    if(ioctl(slave,TIOCSWINSZ,&ws) || nonblock(p->fd)) {close(slave);goto fail;}
    /* Set environment before fork: the service also has a TX thread. */
    long limit=sysconf(_SC_OPEN_MAX); if(limit<0)limit=1024;
    pid_t pid=fork();
    if(pid<0){close(slave);goto fail;}
    if(!pid) {
        if(setsid()<0 || ioctl(slave,TIOCSCTTY,0)<0 || fchdir(m->cwd_fd)<0)_exit(126);
        for(int i=0;i<3;i++)if(dup2(slave,i)<0)_exit(126);
        for(int sig=1;sig<NSIG;sig++)signal(sig,SIG_DFL);
        sigset_t set;sigemptyset(&set);sigprocmask(SIG_SETMASK,&set,NULL);
        /* No daemon UART, sockets, GPIO or root directory descriptors inherited. */
        for(int fd=3;fd<limit;fd++)close(fd);
        execl("/bin/sh","sh","-i",(char*)NULL);_exit(127);
    }
    close(slave);
    p->pid=p->sid=pid;p->state=1;p->last_seen=now_ms();p->id=++m->pty_sequence;
    if(m->pty) {release_buffers(m->pty);free(m->pty);}
    m->pty=p;
    return 0;
fail: {
    int e=errno?errno:ENOMEM;
    if(p->fd>=0)close(p->fd);
    release_buffers(p);free(p);return e;
}}
/* Response: id:u32 state:u8 exit:i32 eof:u8 input_next:u64
 * output_offset:u64 more:u8 length:u16 bytes[0..200]. */
static void response(struct pty_session *p,unsigned char *out,size_t *len,int data) {
    size_t amount=data?p->out_len:0;if(amount>200)amount=200;
    wr32(out,p->id);out[4]=(unsigned char)p->state;wr32(out+5,(uint32_t)p->exitcode);
    out[9]=(unsigned char)(p->eof && !p->pid && !p->out_len);
    wr64(out+10,p->in_next);wr64(out+18,p->out_base);
    out[26]=p->out_len>amount;wr16(out+27,(uint16_t)amount);
    if(amount)memcpy(out+29,p->output,amount);
    if(data)p->out_sent=p->out_base+amount;
    *len=29+amount;
}
int maint_pty_request(maintenance *m,unsigned op,const unsigned char *data,size_t n,
                      unsigned char *out,size_t *len) {
    if(!m->allow_pty)return ENOTSUP;
    if(op==M_PTY_OPEN) {
        int e=start(m,data,n);if(e)return e;
        response(m->pty,out,len,0);return 0;
    }
    struct pty_session *p=m->pty;
    if(n<4)return EINVAL;
    if(!p || rd32(data)!=p->id)return ESTALE;
    if(op==M_PTY_IO) {
        if(n<20 || n>220)return EINVAL;
        uint64_t in=rd64(data+4),ack=rd64(data+12);
        /* Stop/timeout may already have discarded the output buffer while a
         * previously sent frame is still being acknowledged by the host. */
        if(ack<p->out_base || ack>p->out_sent || ack-p->out_base>p->out_len)return EINVAL;
        size_t input=n-20;
        if(in!=p->in_next)return ESTALE;
        if(input && p->state!=1)return EPIPE;
        if(input>IN_CAP-p->in_len)return EAGAIN;
        size_t discard=(size_t)(ack-p->out_base);
        if(discard) {p->out_len-=discard;memmove(p->output,p->output+discard,p->out_len);p->out_base=ack;}
        if(input) {memcpy(p->input+p->in_len,data+20,input);p->in_len+=input;p->in_next+=input;}
        p->last_seen=now_ms();response(p,out,len,1);return 0;
    }
    if(op==M_PTY_RESIZE) {
        if(n!=8)return EINVAL;
        unsigned rows=rd16(data+4),cols=rd16(data+6);
        if(!rows || !cols || rows>1000 || cols>1000)return EINVAL;
        if(p->state!=1 || p->fd<0)return EPIPE;
        struct winsize ws={.ws_row=(unsigned short)rows,.ws_col=(unsigned short)cols};
        if(ioctl(p->fd,TIOCSWINSZ,&ws))return errno;
    } else if(op==M_PTY_CLOSE) {
        if(n!=4)return EINVAL;
        maint_pty_stop(m,3);
    } else if(op==M_PTY_STATUS) {
        if(n!=4)return EINVAL;
    } else return ENOTSUP;
    p->last_seen=now_ms();response(p,out,len,0);return 0;
}
void maint_pty_destroy(maintenance *m) {
    struct pty_session *p=m->pty;if(!p)return;
    signal_jobs(p,SIGKILL);
    if(p->pid>0)while(waitpid(p->pid,NULL,0)<0 && errno==EINTR){}
    if(p->fd>=0)close(p->fd);
    release_buffers(p);free(p);m->pty=NULL;
}
