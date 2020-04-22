#define LINEFILE_BUFSIZE 1024

enum linefile_state {
    linefile_q_init,
    linefile_q_exit,
    linefile_q_scanagain,
    linefile_q_readagain };

struct linefile {
    int fd;
    char buf[LINEFILE_BUFSIZE+1];
    bool skipping;
    bool newline_seen;
    enum linefile_state state;
    int nextix;
    char *end;
    void *jvst;
};
